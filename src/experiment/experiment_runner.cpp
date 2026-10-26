#include "experiment_internal.h"

#include "core/market_data.h"

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

    std::vector<ExperimentJobOutcome> outcomes;
    outcomes.reserve(request.jobs.size());

    std::vector<std::future<ExperimentJobOutcome>> in_flight;
    in_flight.reserve(max_concurrent);

    std::size_t next_job = 0;

    while (next_job < request.jobs.size() || !in_flight.empty()) {
        while (next_job < request.jobs.size() &&
               in_flight.size() < max_concurrent) {
            in_flight.push_back(
                std::async(
                    std::launch::async,
                    run_job,
                    std::cref(dataset),
                    std::cref(request.data),
                    std::cref(request.execution),
                    request.jobs[next_job]
                )
            );
            ++next_job;
        }

        // Retire the oldest job before launching another one.
        outcomes.push_back(in_flight.front().get());
        in_flight.erase(in_flight.begin());
    }

    const auto wall_end = std::chrono::steady_clock::now();
    const double cpu_end = process_cpu_seconds();

    ExperimentResult result;
    result.schema_version = request.schema_version;
    result.experiment_id = request.experiment_id;
    result.data = request.data;
    result.data_summary = data_summary;
    result.execution = request.execution;
    result.output = request.output;
    result.execution.max_concurrent_jobs = max_concurrent;
    result.max_concurrent_jobs = max_concurrent;
    result.results = std::move(outcomes);
    result.summary.jobs_total = result.results.size();

    std::vector<CombinationOutcome> stat_outcomes;
    stat_outcomes.reserve(result.results.size());

    const ExperimentJobOutcome* best = nullptr;

    for (std::size_t i = 0; i < result.results.size(); ++i) {
        const ExperimentJobOutcome& outcome = result.results[i];

        stat_outcomes.push_back({
            i,
            outcome.ok,
            outcome.duration_seconds
        });

        if (!outcome.ok) {
            ++result.summary.jobs_failed;
            continue;
        }

        ++result.summary.jobs_completed;

        if (best == nullptr ||
            outcome.result.metrics.sharpe_ratio >
                best->result.metrics.sharpe_ratio ||
            (outcome.result.metrics.sharpe_ratio ==
                 best->result.metrics.sharpe_ratio &&
             outcome.result.metrics.total_return >
                 best->result.metrics.total_return)) {
            best = &outcome;
        }
    }

    if (result.summary.jobs_failed == 0) {
        result.status = "completed";
    } else if (result.summary.jobs_completed > 0) {
        result.status = "partial";
    } else {
        result.status = "failed";
    }

    const double wall_seconds =
        std::chrono::duration<double>(wall_end - wall_start).count();

    result.summary.wall_seconds = wall_seconds;
    if (best != nullptr) {
        result.summary.best_job_id = best->result.job_id;
        result.summary.best_sharpe_ratio =
            best->result.metrics.sharpe_ratio;
        result.summary.best_total_return =
            best->result.metrics.total_return;
    }

    result.summary.performance = compute_performance_stats(
        stat_outcomes,
        wall_seconds,
        cpu_end - cpu_start,
        max_concurrent
    );

    return result;
}
