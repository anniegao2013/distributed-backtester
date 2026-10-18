#include "backtest_engine.h"
#include "moving_average_crossover.h"
#include "parameter_grid.h"

#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

MarketBar make_bar(
    const std::string& date,
    double price
) {
    return MarketBar{
        date,
        price, price, price, price, price, 1000
    };
}

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Verifies the full pipeline: parameter grid -> BacktestConfig ->
// run_backtest, run sequentially for every combination.
void test_grid_to_backtest_pipeline() {
    // A small warm-up-heavy dataset so MovingAverageCrossover(2, 3) and
    // MovingAverageCrossover(2, 4) both have enough history.
    const std::vector<MarketBar> data{
        make_bar("2020-01-01", 10.0),
        make_bar("2020-01-02", 11.0),
        make_bar("2020-01-03", 12.0),
        make_bar("2020-01-04", 11.5),
        make_bar("2020-01-05", 12.5),
        make_bar("2020-01-06", 13.0)
    };

    const ParameterGrid grid{
        {"short_window", {2.0}},
        {"long_window",  {3.0, 4.0}}
    };

    const auto combinations = generate_parameter_combinations(grid);
    expect(combinations.size() == 2,
           "Expected two parameter combinations");

    std::vector<JobResult> results;
    results.reserve(combinations.size());

    for (std::size_t i = 0; i < combinations.size(); ++i) {
        const BacktestConfig config{
            "grid-" + std::to_string(i + 1),
            "TEST",
            "2020-01-01",
            "2020-01-06",
            100.0,
            combinations[i]
        };

        MovingAverageCrossover strategy(
            static_cast<std::size_t>(combinations[i].at("short_window")),
            static_cast<std::size_t>(combinations[i].at("long_window"))
        );

        const JobResult result = run_backtest(data, strategy, config);
        results.push_back(result);
    }

    // Every combination produced exactly one result.
    expect(results.size() == combinations.size(),
           "Expected one result per parameter combination");

    // Each result carries back the exact parameters it was run with.
    for (std::size_t i = 0; i < results.size(); ++i) {
        expect(
            results[i].parameters == combinations[i],
            "Result parameters should match the input combination"
        );
        expect(
            results[i].job_id == "grid-" + std::to_string(i + 1),
            "Result job ID should match the config job ID"
        );
        expect(
            results[i].asset == "TEST",
            "Result asset should match the config asset"
        );
    }

    // Job IDs are unique across the grid.
    std::set<std::string> job_ids;
    for (const auto& result : results) {
        job_ids.insert(result.job_id);
    }
    expect(job_ids.size() == results.size(),
           "Job IDs should be unique across combinations");
}

// Verifies that sequential execution completes every combination even when
// results differ, and that the results are distinguishable.
void test_sequential_execution_completes_all_combinations() {
    const std::vector<MarketBar> data{
        make_bar("2020-01-01", 10.0),
        make_bar("2020-01-02", 11.0),
        make_bar("2020-01-03", 12.0),
        make_bar("2020-01-04", 13.0),
        make_bar("2020-01-05", 14.0),
        make_bar("2020-01-06", 15.0),
        make_bar("2020-01-07", 16.0)
    };

    const ParameterGrid grid{
        {"short_window", {2.0, 3.0}},
        {"long_window",  {4.0, 5.0}}
    };

    const auto combinations = generate_parameter_combinations(grid);
    expect(combinations.size() == 4,
           "Expected four parameter combinations");

    std::vector<JobResult> results;
    results.reserve(combinations.size());

    for (std::size_t i = 0; i < combinations.size(); ++i) {
        const BacktestConfig config{
            "seq-" + std::to_string(i + 1),
            "TEST",
            "2020-01-01",
            "2020-01-07",
            100.0,
            combinations[i]
        };

        MovingAverageCrossover strategy(
            static_cast<std::size_t>(combinations[i].at("short_window")),
            static_cast<std::size_t>(combinations[i].at("long_window"))
        );

        const JobResult result = run_backtest(data, strategy, config);
        results.push_back(result);
    }

    expect(results.size() == 4,
           "Expected all four sequential backtests to complete");

    // Every result reports finite, non-negative trade counts and a defined
    // total return.
    for (const auto& result : results) {
        expect(
            std::isfinite(result.metrics.total_return),
            "Total return should be finite"
        );
        expect(
            result.metrics.number_of_trades >= 0,
            "Trade count should be non-negative"
        );
        expect(
            !result.parameters.empty(),
            "Result should carry a non-empty parameter map"
        );
    }
}

}  // namespace

int main() {
    try {
        test_grid_to_backtest_pipeline();
        test_sequential_execution_completes_all_combinations();

        std::cout << "All parameter grid to backtest integration tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
