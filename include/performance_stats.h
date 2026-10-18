#ifndef PERFORMANCE_STATS_H
#define PERFORMANCE_STATS_H

#include <cstddef>
#include <string>
#include <vector>

struct CombinationOutcome {
    std::size_t index = 0;
    bool ok = false;
    double duration_seconds = 0.0;
};

// Aggregate performance statistics for a batch of concurrent backtest jobs.
struct PerformanceStats {
    std::size_t jobs_completed = 0;
    std::size_t jobs_total = 0;
    std::size_t max_concurrent = 0;
    double wall_seconds = 0.0;
    double throughput = 0.0;      // jobs per second
    double mean_latency = 0.0;    // seconds
    double median_latency = 0.0;  // seconds
    double min_latency = 0.0;     // seconds
    double max_latency = 0.0;     // seconds
    double p95_latency = 0.0;     // seconds
    double total_work_seconds = 0.0;  // sum of job durations (CPU-s)
    double speedup = 0.0;         // total_work_seconds / wall_seconds
    double cpu_seconds = 0.0;     // process CPU time consumed (getrusage)
    double cpu_utilization = 0.0; // cpu_seconds / wall_seconds * 100 (%)
    double cpu_efficiency = 0.0;  // cpu_utilization / max_concurrent * 100 (%)
};

// Computes performance statistics from completed job outcomes.
PerformanceStats compute_performance_stats(
    const std::vector<CombinationOutcome>& outcomes,
    double wall_seconds,
    double cpu_seconds,
    std::size_t max_concurrent
);

#endif
