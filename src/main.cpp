#include "market_data.h"
#include "moving_average_crossover.h"
#include "backtest_engine.h"
#include "parameter_grid.h"

#include <algorithm>
#include <exception>
#include <future>
#include <iostream>
#include <iomanip>
#include <string>
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

// Outcome of running a single parameter combination.
struct CombinationOutcome {
    std::size_t index = 0;
    JobResult result;
    bool ok = false;
    std::string error;
};

// Runs one backtest for a single parameter combination, wiring the
// combination into the BacktestConfig's parameter map. Captures exceptions
// so a single failing combination does not abort the whole grid.
CombinationOutcome run_combination(
    const std::vector<MarketBar>& dataset,
    const ParameterMap& parameters,
    std::size_t index
) {
    CombinationOutcome outcome;
    outcome.index = index;

    try {
        const BacktestConfig config{
            "grid-" + std::to_string(index),
            "SP500",
            "2000-01-01",
            "2010-12-31",
            10000.0,
            parameters
        };

        MovingAverageCrossover strategy = make_strategy(parameters);
        outcome.result = run_backtest(dataset, strategy, config);
        outcome.ok = true;
    } catch (const std::exception& error) {
        outcome.error = error.what();
    }

    return outcome;
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
              << "Sharpe\tTotal Return\tMax Drawdown\tTrades\tParameters\n";

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

        // Define the parameter grid: every value of each parameter is
        // combined with every value of every other parameter.
        const ParameterGrid grid{
            {"short_window", {10.0, 20.0, 30.0}},
            {"long_window",  {50.0, 100.0, 200.0}}
        };

        // Expand the grid into every concrete parameter combination.
        const auto combinations = generate_parameter_combinations(grid);
        std::cout << "Running " << combinations.size()
                  << " parameter combinations concurrently.\n";

        // Launch one async task per combination. The dataset is read-only
        // and each task builds its own strategy/config, so combinations are
        // independent and safe to execute in parallel.
        std::vector<std::future<CombinationOutcome>> futures;
        futures.reserve(combinations.size());

        std::size_t index = 0;
        for (const auto& parameters : combinations) {
            ++index;
            futures.push_back(
                std::async(
                    std::launch::async,
                    run_combination,
                    std::cref(dataset),
                    parameters,
                    index
                )
            );
        }

        // Collect outcomes in combination order for deterministic output.
        std::vector<CombinationOutcome> outcomes;
        outcomes.reserve(futures.size());

        for (auto& future : futures) {
            outcomes.push_back(future.get());
        }

        // Sort outcomes by index so output is deterministic regardless of
        // completion order.
        std::sort(
            outcomes.begin(),
            outcomes.end(),
            [](const CombinationOutcome& a, const CombinationOutcome& b) {
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
                  << combinations.size() << " backtests.\n";

        print_ranked_results(results);

    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
