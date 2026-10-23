#include "market_data.h"
#include "moving_average_crossover.h"
#include "backtest_engine.h"
#include "parameter_grid.h"
#include "performance_stats.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <future>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

namespace {

// Builds a MovingAverageCrossover strategy from a single parameter
// combination produced by the parameter grid.
MovingAverageCrossover make_strategy(const ParameterMap& parameters) {
    const std::size_t short_window =
        static_cast<std::size_t>(parameters.at("short_window"));
    const std::size_t long_window =
        static_cast<std::size_t>(parameters.at("long_window"));
    return MovingAverageCrossover(short_window, long_window);
}

// Outcome of running a single parameter combination, carrying the JobResult.
struct JobOutcome {
    std::size_t index = 0;
    JobResult result;
    bool ok = false;
    std::string error;
    std::chrono::duration<double> duration{0.0};
};

// Runs one backtest for a single parameter combination, wiring the
// combination into the BacktestConfig's parameter map. Captures exceptions
// so a single failing combination does not abort the whole grid, and times
// the job for latency statistics.
JobOutcome run_combination(
    const std::vector<MarketBar>& dataset,
    const ParameterMap& parameters,
    std::size_t index
) {
    JobOutcome outcome;
    outcome.index = index;

    const auto t0 = std::chrono::steady_clock::now();

    try {
        const BacktestConfig config{
            "grid-" + std::to_string(index),
            "SP500",
            "2000-01-01",
            "2010-12-31",
            10000.0,
            parameters,
            0.001,   // 10 bps commission on traded notional
            0.0005   // 5 bps adverse slippage on each fill
        };

        MovingAverageCrossover strategy = make_strategy(parameters);
        outcome.result = run_backtest(dataset, strategy, config);
        outcome.ok = true;
    } catch (const std::exception& error) {
        outcome.error = error.what();
    }

    const auto t1 = std::chrono::steady_clock::now();
    outcome.duration = t1 - t0;

    return outcome;
}

// Returns process CPU time (user + system) in seconds.
double process_cpu_seconds() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
           usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}

void print_result(const JobResult& result) {
    std::cout << std::fixed << std::setprecision(4);

    std::cout << "Job ID: " << result.job_id << '\n';
    std::cout << "Asset: " << result.asset << '\n';

    std::cout << "Parameters:\n";
    for (const auto& [name, value] : result.parameters) {
        std::cout << "  " << name << ": " << value << '\n';
    }

    std::cout << "Metrics:\n";
    std::cout << "  Total return: "
              << result.metrics.total_return * 100.0 << "%\n";
    std::cout << "  Sharpe ratio: " << result.metrics.sharpe_ratio << '\n';
    std::cout << "  Max drawdown: "
              << result.metrics.max_drawdown * 100.0 << "%\n";
    std::cout << "  Number of trades: "
              << result.metrics.number_of_trades << '\n';
    std::cout << "  Total costs: $"
              << result.metrics.total_costs << '\n';
}

// Ranks results by Sharpe ratio (highest first) and prints a summary table.
// Ties receive the same rank; the next rank skips the tied positions.
void print_ranked_results(const std::vector<JobResult>& results) {
    if (results.empty()) {
        return;
    }

    // Sort a copy by Sharpe ratio descending, then total return descending
    // as a tie-breaker.
    std::vector<const JobResult*> ranked;
    ranked.reserve(results.size());
    for (const auto& result : results) {
        ranked.push_back(&result);
    }

    std::sort(
        ranked.begin(),
        ranked.end(),
        [](const JobResult* a, const JobResult* b) {
            if (a->metrics.sharpe_ratio != b->metrics.sharpe_ratio) {
                return a->metrics.sharpe_ratio > b->metrics.sharpe_ratio;
            }
            return a->metrics.total_return > b->metrics.total_return;
        }
    );

    std::cout << "\n=== Ranked Results (by Sharpe ratio) ===\n";
    std::cout << std::left << std::setw(6) << "Rank"
              << std::setw(10) << "Job ID"
              << "Sharpe\tTotal Return\tMax Drawdown\tCosts\tTrades\tParameters\n";

    std::size_t rank = 0;
    for (std::size_t i = 0; i < ranked.size(); ++i) {
        const JobResult& result = *ranked[i];

        // Assign competition ranking ("1224"): ties share a rank.
        if (i == 0) {
            rank = 1;
        } else {
            const JobResult& previous = *ranked[i - 1];
            if (
                result.metrics.sharpe_ratio == previous.metrics.sharpe_ratio &&
                result.metrics.total_return == previous.metrics.total_return
            ) {
                // Same rank as previous.
            } else {
                rank = i + 1;
            }
        }

        std::cout << std::left << std::setw(6) << rank
                  << std::setw(10) << result.job_id
                  << std::fixed << std::setprecision(4)
                  << result.metrics.sharpe_ratio
                  << "\t"
                  << std::fixed << std::setprecision(4)
                  << result.metrics.total_return * 100.0 << "%"
                  << "\t"
                  << std::fixed << std::setprecision(4)
                  << result.metrics.max_drawdown * 100.0 << "%"
                  << "\t"
                  << std::fixed << std::setprecision(2)
                  << result.metrics.total_costs
                  << "\t"
                  << std::fixed << std::setprecision(0)
                  << result.metrics.number_of_trades
                  << "\t";

        bool first = true;
        for (const auto& [name, value] : result.parameters) {
            if (!first) {
                std::cout << ", ";
            }
            std::cout << name << "=" << std::fixed << std::setprecision(0)
                      << value;
            first = false;
        }
        std::cout << '\n';
    }

    std::cout << "\nBest configuration: "
              << ranked.front()->job_id
              << " (Sharpe " << std::fixed << std::setprecision(4)
              << ranked.front()->metrics.sharpe_ratio << ", total return "
              << std::fixed << std::setprecision(4)
              << ranked.front()->metrics.total_return * 100.0 << "%)\n";
}

// Prints a PerformanceStats report to stdout.
void print_performance_stats(const PerformanceStats& stats) {
    if (stats.jobs_completed == 0) {
        std::cout << "\nNo jobs completed; skipping performance stats.\n";
        return;
    }

    std::cout << "\n=== Performance Stats ===\n";
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "Jobs completed:    " << stats.jobs_completed << " of "
              << stats.jobs_total << '\n';
    std::cout << "Max concurrent:    " << stats.max_concurrent << '\n';
    std::cout << "Wall time:         " << stats.wall_seconds << " s\n";
    std::cout << "Throughput:        " << stats.throughput << " jobs/s\n";
    std::cout << "Latency (mean):    " << stats.mean_latency << " s\n";
    std::cout << "Latency (median):  " << stats.median_latency << " s\n";
    std::cout << "Latency (min):     " << stats.min_latency << " s\n";
    std::cout << "Latency (max):     " << stats.max_latency << " s\n";
    std::cout << "Latency (p95):     " << stats.p95_latency << " s\n";
    std::cout << "Total work:        " << stats.total_work_seconds << " CPU-s\n";
    std::cout << "Speedup:           " << stats.speedup << "x\n";
    std::cout << "CPU time:          " << stats.cpu_seconds << " s\n";
    std::cout << "CPU utilization:   " << stats.cpu_utilization << "% of one core\n";
    std::cout << "CPU efficiency:    " << stats.cpu_efficiency << "%\n";
}

}  // namespace

int main() {
    try {
        const auto dataset = load_market_data(
            "data/market/sp500_1950_2018.csv"
        );

        std::cout << "Successfully loaded " << dataset.size()
                  << " trading days.\n";
        std::cout << "First date: " << dataset.front().date << '\n';
        std::cout << "Last date:  " << dataset.back().date << '\n';

        // Define a large parameter grid so parallel execution shows a
        // noticeable speed improvement. All short_window values are below
        // the minimum long_window so every combination is valid.
        const ParameterGrid grid{
            {"short_window",
             {10.0, 15.0, 20.0, 25.0, 30.0, 35.0, 40.0, 45.0, 50.0,
              55.0, 60.0, 65.0, 70.0, 75.0, 80.0, 85.0, 90.0, 95.0}},
            {"long_window",
             {100.0, 110.0, 120.0, 130.0, 140.0, 150.0, 160.0, 170.0,
              180.0, 190.0, 200.0, 210.0, 220.0, 230.0, 240.0, 250.0,
              260.0, 270.0, 280.0, 290.0, 300.0}}
        };

        // Expand the grid into every concrete parameter combination.
        const auto combinations = generate_parameter_combinations(grid);
        const std::size_t max_concurrent =
            std::thread::hardware_concurrency();

        std::cout << "Running " << combinations.size()
                  << " parameter combinations concurrently "
                  << "(up to " << max_concurrent << " jobs at a time).\n";

        const double cpu_start = process_cpu_seconds();
        const auto t0 = std::chrono::steady_clock::now();

        // Launch combinations in bounded batches so concurrency never
        // exceeds hardware capacity. Each task is independent: the dataset
        // is read-only and each task builds its own strategy/config.
        std::vector<JobOutcome> outcomes;
        outcomes.reserve(combinations.size());

        std::size_t index = 0;
        auto it = combinations.begin();
        while (it != combinations.end()) {
            std::vector<std::future<JobOutcome>> batch;
            batch.reserve(max_concurrent);

            std::size_t batch_size = 0;
            while (it != combinations.end() && batch_size < max_concurrent) {
                ++index;
                batch.push_back(
                    std::async(
                        std::launch::async,
                        run_combination,
                        std::cref(dataset),
                        *it,
                        index
                    )
                );
                ++it;
                ++batch_size;
            }

            for (auto& future : batch) {
                outcomes.push_back(future.get());
            }
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double cpu_end = process_cpu_seconds();

        const double wall_seconds =
            std::chrono::duration<double>(t1 - t0).count();
        const double cpu_seconds = cpu_end - cpu_start;

        // Sort outcomes by index so output is deterministic regardless of
        // completion order.
        std::sort(
            outcomes.begin(),
            outcomes.end(),
            [](const JobOutcome& a, const JobOutcome& b) {
                return a.index < b.index;
            }
        );

        std::vector<JobResult> results;
        results.reserve(outcomes.size());

        for (const auto& outcome : outcomes) {
            if (!outcome.ok) {
                std::cerr << "Combination " << outcome.index << " failed: "
                          << outcome.error << '\n';
                continue;
            }

            results.push_back(outcome.result);

            std::cout << "\n--- Result " << outcome.index << " ---\n";
            print_result(outcome.result);
        }

        std::cout << "\nCompleted " << results.size() << " of "
                  << combinations.size() << " backtests in "
                  << wall_seconds << " seconds.\n";

        print_ranked_results(results);

        // Convert to stats-compatible outcomes and compute performance stats.
        std::vector<CombinationOutcome> stat_outcomes;
        stat_outcomes.reserve(outcomes.size());
        for (const auto& outcome : outcomes) {
            stat_outcomes.push_back(
                {
                    outcome.index,
                    outcome.ok,
                    outcome.duration.count()
                }
            );
        }

        const PerformanceStats stats = compute_performance_stats(
            stat_outcomes,
            wall_seconds,
            cpu_seconds,
            max_concurrent
        );

        print_performance_stats(stats);

    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
