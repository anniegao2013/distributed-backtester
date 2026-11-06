#include "experiment_internal.h"

#include "core/market_data.h"
#include "experiment/result_collector.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sys/resource.h>

namespace {

ExperimentJobOutcome run_job(
    const std::vector<MarketBar>& dataset,
    const ExperimentData& data,
    const ExperimentExecution& execution,
    const ExperimentJob& job
) {
    ExperimentJobOutcome outcome;
    outcome.job = job;

    const auto start = std::chrono::steady_clock::now();

    try {
        const BacktestConfig config{
            job.job_id,
            data.asset,
            data.start_date,
            data.end_date,
            execution.initial_cash,
            job.parameters,
            execution.transaction_cost_rate,
            execution.slippage_rate
        };

        const auto strategy =
            create_strategy(job.strategy_type, job.parameters);

        outcome.result = run_backtest(dataset, *strategy, config);
        outcome.ok = true;
    } catch (const std::exception& error) {
        outcome.error = error.what();
    }

    const auto end = std::chrono::steady_clock::now();
    outcome.duration_seconds =
        std::chrono::duration<double>(end - start).count();

    return outcome;
}

ExperimentJobOutcome failed_outcome(
    const ExperimentJob& job,
    const std::string& error
) {
    ExperimentJobOutcome outcome;
    outcome.job = job;
    outcome.error = error.empty()
        ? "job did not reach a terminal state"
        : error;
    return outcome;
}

struct InFlightJob {
    std::size_t index = 0;
    std::future<ExperimentJobOutcome> future;
};

double process_cpu_seconds() {
    struct rusage usage;

    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }

    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
           usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}

}  // namespace

ExperimentResult run_experiment(
    const ExperimentRequest& request,
    const ExperimentProgressCallback& on_started
) {
    if (request.jobs.empty()) {
        throw std::invalid_argument(
            "Experiment has no jobs; call finalize_experiment() first"
        );
    }

    const std::vector<MarketBar> dataset =
        load_market_data(request.data.path);

    if (dataset.empty()) {
        throw std::runtime_error(
            "Market data file '" + request.data.path + "' is empty"
        );
    }

    const ExperimentDataSummary data_summary{
        dataset.size(),
        dataset.front().date,
        dataset.back().date
    };

    const std::size_t hardware_threads = std::max<std::size_t>(
        1,
        std::thread::hardware_concurrency()
    );
    const std::size_t max_concurrent =
        request.execution.max_concurrent_jobs > 0
            ? request.execution.max_concurrent_jobs
            : hardware_threads;

    if (on_started) {
        on_started(data_summary, request.jobs.size(), max_concurrent);
    }

    const double cpu_start = process_cpu_seconds();
    const auto wall_start = std::chrono::steady_clock::now();

    // Local execution reports into the same collector the coordinator uses.
    // The runner never inspects execution details; it only waits for every job
    // to become terminal and then hands the ordered outcomes to aggregation.
    ExperimentResultCollector collector(request.jobs);

    std::vector<InFlightJob> in_flight;
    in_flight.reserve(max_concurrent);

    std::size_t next_job = 0;

    while (next_job < request.jobs.size() || !in_flight.empty()) {
        while (next_job < request.jobs.size() &&
               in_flight.size() < max_concurrent) {
            in_flight.push_back(
                InFlightJob{
                    next_job,
                    std::async(
                        std::launch::async,
                        run_job,
                        std::cref(dataset),
                        std::cref(request.data),
                        std::cref(request.execution),
                        request.jobs[next_job]
                    )
                }
            );
            ++next_job;
        }

        // Retire the oldest job before launching another one.
        InFlightJob& current = in_flight.front();
        ExperimentJobOutcome outcome;

        try {
            outcome = current.future.get();
        } catch (const std::exception& error) {
            outcome = failed_outcome(
                request.jobs[current.index],
                error.what()
            );
        } catch (...) {
            outcome = failed_outcome(
                request.jobs[current.index],
                "job threw an unknown exception"
            );
        }

        collector.record(current.index, std::move(outcome));
        in_flight.erase(in_flight.begin());
    }

    collector.wait();
    std::vector<ExperimentJobOutcome> outcomes = collector.take();

    const auto wall_end = std::chrono::steady_clock::now();
    const double cpu_end = process_cpu_seconds();

    const double wall_seconds =
        std::chrono::duration<double>(wall_end - wall_start).count();

    return build_experiment_result(
        request,
        data_summary,
        std::move(outcomes),
        wall_seconds,
        cpu_end - cpu_start,
        max_concurrent
    );
}
