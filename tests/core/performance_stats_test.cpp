#include "core/performance_stats.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_near(double actual, double expected, double tolerance,
                 const std::string& message) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(
            message + " (expected ~" + std::to_string(expected) +
            ", got " + std::to_string(actual) + ")"
        );
    }
}

// Empty input yields zero stats.
void test_empty_outcomes() {
    std::vector<CombinationOutcome> outcomes;

    const PerformanceStats stats =
        compute_performance_stats(outcomes, 1.0, 0.5, 4);

    expect(stats.jobs_completed == 0, "Completed jobs should be zero");
    expect(stats.jobs_total == 0, "Total jobs should be zero");
    expect(stats.wall_seconds == 1.0, "Wall time should be preserved");
    expect(stats.cpu_seconds == 0.5, "CPU time should be preserved");
    expect(stats.max_concurrent == 4, "Max concurrent should be preserved");
    expect(stats.throughput == 0.0, "Throughput should be zero");
    expect(stats.mean_latency == 0.0, "Mean latency should be zero");
    expect(stats.median_latency == 0.0, "Median latency should be zero");
    expect(stats.min_latency == 0.0, "Min latency should be zero");
    expect(stats.max_latency == 0.0, "Max latency should be zero");
    expect(stats.p95_latency == 0.0, "P95 latency should be zero");
    expect(stats.total_work_seconds == 0.0, "Total work should be zero");
    expect(stats.speedup == 0.0, "Speedup should be zero");
    expect(stats.cpu_utilization == 0.0, "CPU utilization should be zero");
    expect(stats.cpu_efficiency == 0.0, "CPU efficiency should be zero");
}

// Single job: all latency percentiles equal the single duration.
void test_single_job() {
    std::vector<CombinationOutcome> outcomes{
        {1, true, 0.5}
    };

    const PerformanceStats stats =
        compute_performance_stats(outcomes, 0.5, 0.5, 4);

    expect(stats.jobs_completed == 1, "Completed jobs should be one");
    expect(stats.jobs_total == 1, "Total jobs should be one");
    expect(stats.total_work_seconds == 0.5, "Total work should be 0.5");
    expect(stats.mean_latency == 0.5, "Mean latency should be 0.5");
    expect(stats.min_latency == 0.5, "Min latency should be 0.5");
    expect(stats.max_latency == 0.5, "Max latency should be 0.5");
    expect(stats.median_latency == 0.5, "Median latency should be 0.5");
    expect(stats.p95_latency == 0.5, "P95 latency should be 0.5");
    expect(stats.throughput == 2.0, "Throughput should be 2 jobs/s");
    expect(stats.speedup == 1.0, "Speedup should be 1.0x");
    expect(stats.cpu_utilization == 100.0, "CPU utilization should be 100%");
    expect(stats.cpu_efficiency == 25.0, "CPU efficiency should be 25%");
}

// Multiple jobs: verifies sorted latency percentiles and aggregate stats.
void test_multiple_jobs() {
    std::vector<CombinationOutcome> outcomes{
        {1, true, 0.1},
        {2, true, 0.2},
        {3, true, 0.3},
        {4, true, 0.4},
        {5, true, 0.5}
    };

    const PerformanceStats stats =
        compute_performance_stats(outcomes, 1.0, 2.0, 2);

    expect(stats.jobs_completed == 5, "Completed jobs should be five");
    expect(stats.jobs_total == 5, "Total jobs should be five");
    expect(stats.total_work_seconds == 1.5, "Total work should be 1.5");
    expect(stats.mean_latency == 0.3, "Mean latency should be 0.3");
    expect(stats.min_latency == 0.1, "Min latency should be 0.1");
    expect(stats.max_latency == 0.5, "Max latency should be 0.5");
    expect(stats.median_latency == 0.3, "Median latency should be 0.3");
    // P95 of 5 sorted values: ceil(0.95 * 5) - 1 = ceil(4.75) - 1 = 5 - 1 = 4
    expect(stats.p95_latency == 0.5, "P95 latency should be 0.5");
    expect(stats.throughput == 5.0, "Throughput should be 5 jobs/s");
    expect(stats.speedup == 1.5, "Speedup should be 1.5x");
    expect(stats.cpu_utilization == 200.0, "CPU utilization should be 200%");
    expect(stats.cpu_efficiency == 100.0, "CPU efficiency should be 100%");
}

// Failed jobs are excluded from latency and throughput stats.
void test_failed_jobs_excluded() {
    std::vector<CombinationOutcome> outcomes{
        {1, true, 0.1},
        {2, false, 0.0},   // failed
        {3, true, 0.3},
        {4, false, 0.0},   // failed
        {5, true, 0.5}
    };

    const PerformanceStats stats =
        compute_performance_stats(outcomes, 1.0, 0.9, 2);

    expect(stats.jobs_completed == 3, "Completed jobs should be three");
    expect(stats.jobs_total == 5, "Total jobs should be five");
    expect(stats.total_work_seconds == 0.9, "Total work should be 0.9");
    expect(stats.mean_latency == 0.3, "Mean latency should be 0.3");
    expect(stats.min_latency == 0.1, "Min latency should be 0.1");
    expect(stats.max_latency == 0.5, "Max latency should be 0.5");
    expect(stats.throughput == 3.0, "Throughput should be 3 jobs/s");
    expect(stats.speedup == 0.9, "Speedup should be 0.9x");
}

// Wall time of zero avoids division by zero.
void test_zero_wall_time() {
    std::vector<CombinationOutcome> outcomes{
        {1, true, 0.5}
    };

    const PerformanceStats stats =
        compute_performance_stats(outcomes, 0.0, 0.5, 4);

    expect(stats.throughput == 0.0, "Throughput should be zero with zero wall time");
    expect(stats.speedup == 0.0, "Speedup should be zero with zero wall time");
    expect(stats.cpu_utilization == 0.0, "CPU utilization should be zero with zero wall time");
    expect(stats.cpu_efficiency == 0.0, "CPU efficiency should be zero with zero wall time");
}

// Max concurrent of zero avoids division by zero in efficiency.
void test_zero_max_concurrent() {
    std::vector<CombinationOutcome> outcomes{
        {1, true, 0.5}
    };

    const PerformanceStats stats =
        compute_performance_stats(outcomes, 0.5, 0.5, 0);

    expect(stats.cpu_efficiency == 0.0, "CPU efficiency should be zero with zero max concurrent");
    expect(stats.cpu_utilization == 100.0, "CPU utilization should still be 100%");
}

// P95 index calculation: even number of jobs.
void test_p95_even_count() {
    // 10 jobs, p95 index = ceil(0.95 * 10) - 1 = ceil(9.5) - 1 = 10 - 1 = 9 (last)
    std::vector<CombinationOutcome> outcomes;
    for (std::size_t i = 0; i < 10; ++i) {
        outcomes.push_back(
            {i + 1, true, static_cast<double>(i + 1) * 0.1}
        );
    }

    const PerformanceStats stats =
        compute_performance_stats(outcomes, 1.0, 1.0, 1);

    expect(stats.p95_latency == 1.0, "P95 latency should be 1.0 for 10 jobs");
}

// CPU efficiency never exceeds 100% when workload is perfectly parallel.
void test_cpu_efficiency_capped() {
    // 8 jobs, each 1.0s, wall time 1.0s, 8 cores: cpu_seconds=8.0,
    // utilization=800%, efficiency=100%
    std::vector<CombinationOutcome> outcomes;
    for (std::size_t i = 0; i < 8; ++i) {
        outcomes.push_back({i + 1, true, 1.0});
    }

    const PerformanceStats stats =
        compute_performance_stats(outcomes, 1.0, 8.0, 8);

    expect(stats.speedup == 8.0, "Speedup should be 8.0x");
    expect(stats.cpu_utilization == 800.0, "CPU utilization should be 800%");
    expect(stats.cpu_efficiency == 100.0, "CPU efficiency should be capped at 100%");
}

int main() {
    try {
        test_empty_outcomes();
        test_single_job();
        test_multiple_jobs();
        test_failed_jobs_excluded();
        test_zero_wall_time();
        test_zero_max_concurrent();
        test_p95_even_count();
        test_cpu_efficiency_capped();

        std::cout << "All performance stats tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
