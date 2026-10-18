#include "performance_stats.h"

#include <algorithm>
#include <cmath>
#include <numeric>

PerformanceStats compute_performance_stats(
    const std::vector<CombinationOutcome>& outcomes,
    double wall_seconds,
    double cpu_seconds,
    std::size_t max_concurrent
) {
    PerformanceStats stats;
    stats.jobs_total = outcomes.size();
    stats.max_concurrent = max_concurrent;
    stats.wall_seconds = wall_seconds;
    stats.cpu_seconds = cpu_seconds;

    std::size_t completed = 0;
    std::vector<double> durations;
    for (const auto& outcome : outcomes) {
        if (outcome.ok) {
            durations.push_back(outcome.duration_seconds);
            ++completed;
        }
    }
    stats.jobs_completed = completed;

    if (completed == 0) {
        return stats;
    }

    std::sort(durations.begin(), durations.end());

    const double total_work_seconds =
        std::accumulate(durations.begin(), durations.end(), 0.0);
    stats.total_work_seconds = total_work_seconds;
    stats.mean_latency = total_work_seconds / static_cast<double>(completed);
    stats.min_latency = durations.front();
    stats.max_latency = durations.back();
    stats.median_latency = durations[completed / 2];
    stats.p95_latency =
        durations[static_cast<std::size_t>(
            std::ceil(0.95 * static_cast<double>(completed)) - 1)];

    stats.throughput =
        (wall_seconds > 0.0)
            ? static_cast<double>(completed) / wall_seconds
            : 0.0;

    stats.speedup =
        (wall_seconds > 0.0) ? total_work_seconds / wall_seconds : 0.0;

    stats.cpu_utilization =
        (wall_seconds > 0.0) ? (cpu_seconds / wall_seconds) * 100.0 : 0.0;

    stats.cpu_efficiency =
        (wall_seconds > 0.0 && max_concurrent > 0)
            ? (cpu_seconds / wall_seconds /
               static_cast<double>(max_concurrent)) * 100.0
            : 0.0;

    return stats;
}
