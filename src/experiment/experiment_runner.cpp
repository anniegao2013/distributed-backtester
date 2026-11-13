#include "experiment_internal.h"

#include "core/market_data.h"
#include "experiment/result_collector.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
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

    // Fixed pool: each worker pulls the next job index and reports its outcome
    // directly to the collector, which restores request order during
    // aggregation. Unlike the previous front-wait design, a slow job does not
    // hold a capacity slot while later jobs finish, and no thread is created
    // per job.
    std::atomic<std::size_t> next_job{0};

    const auto run_queued_jobs = [&]() {
        while (true) {
            const std::size_t index =
                next_job.fetch_add(1, std::memory_order_relaxed);

            if (index >= request.jobs.size()) {
                return;
            }

            ExperimentJobOutcome outcome;

            try {
                outcome = run_job(
                    dataset,
                    request.data,
                    request.execution,
                    request.jobs[index]
                );
            } catch (const std::exception& error) {
                outcome = failed_outcome(
                    request.jobs[index],
                    error.what()
                );
            } catch (...) {
                outcome = failed_outcome(
                    request.jobs[index],
                    "job threw an unknown exception"
                );
            }

            collector.record(index, std::move(outcome));
        }
    };

    const std::size_t pool_size =
        std::min(max_concurrent, request.jobs.size());

    std::vector<std::thread> workers;
    workers.reserve(pool_size);

    try {
        for (std::size_t i = 0; i < pool_size; ++i) {
            workers.emplace_back(run_queued_jobs);
        }
    } catch (...) {
        // Drain already-started workers before propagating so their outcomes
        // are not lost and their threads are joined.
        for (std::thread& worker : workers) {
            worker.join();
        }
        throw;
    }

    for (std::thread& worker : workers) {
        worker.join();
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
