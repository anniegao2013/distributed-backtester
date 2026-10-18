#include "market_data.h"
#include "moving_average_crossover.h"
#include "backtest_engine.h"
#include "parameter_grid.h"

#include <exception>
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

// Runs one backtest for a single parameter combination, wiring the
// combination into the BacktestConfig's parameter map.
JobResult run_combination(
    const std::vector<MarketBar>& dataset,
    const ParameterMap& parameters,
    std::size_t index
) {
    const BacktestConfig config{
        "grid-" + std::to_string(index),
        "SP500",
        "2000-01-01",
        "2010-12-31",
        10000.0,
        parameters
    };

    MovingAverageCrossover strategy = make_strategy(parameters);
    return run_backtest(dataset, strategy, config);
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
                  << " parameter combinations sequentially.\n";

        std::vector<JobResult> results;
        results.reserve(combinations.size());

        std::size_t index = 0;
        for (const auto& parameters : combinations) {
            ++index;

            JobResult result;
            try {
                result = run_combination(dataset, parameters, index);
            } catch (const std::exception& error) {
                std::cerr << "Combination " << index << " failed: "
                          << error.what() << '\n';
                continue;
            }

            results.push_back(result);

            std::cout << "\n--- Result " << index << " ---\n";
            print_result(result);
        }

        std::cout << "\nCompleted " << results.size() << " of "
                  << combinations.size() << " backtests.\n";

    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
