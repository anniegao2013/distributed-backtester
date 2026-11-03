#include "experiment/experiment.h"

#include <cstddef>
#include <utility>
#include <vector>

ExperimentResult build_experiment_result(
    const ExperimentRequest& request,
    const ExperimentDataSummary& data_summary,
    std::vector<ExperimentJobOutcome> outcomes,
    double wall_seconds,
    double cpu_seconds,
    std::size_t max_concurrent
) {
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
        cpu_seconds,
        max_concurrent
    );

    return result;
}
