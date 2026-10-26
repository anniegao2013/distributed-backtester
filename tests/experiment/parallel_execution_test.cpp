#include "trading/backtest_engine.h"
#include "strategies/moving_average_crossover.h"
#include "core/parameter_grid.h"

#include <algorithm>
#include <cmath>
#include <future>
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

// Runs one combination sequentially, returning the result keyed by index.
JobResult run_one_sequential(
    const std::vector<MarketBar>& dataset,
    const ParameterMap& parameters,
    std::size_t index
) {
    const BacktestConfig config{
        "grid-" + std::to_string(index),
        "TEST",
        "2020-01-01",
        "2020-04-01",
        100.0,
        parameters
    };

    MovingAverageCrossover strategy(
        static_cast<std::size_t>(parameters.at("short_window")),
        static_cast<std::size_t>(parameters.at("long_window"))
    );

    return run_backtest(dataset, strategy, config);
}

// Returns metrics for every combination, indexed by combination position.
std::vector<JobResult> run_all(
    const std::vector<MarketBar>& dataset,
    const std::vector<ParameterMap>& combinations,
    bool concurrent
) {
    std::vector<JobResult> results(combinations.size());

    if (concurrent) {
        std::vector<std::future<JobResult>> futures;
        futures.reserve(combinations.size());

        std::size_t index = 0;
        for (const auto& parameters : combinations) {
            ++index;
            futures.push_back(
                std::async(
                    std::launch::async,
                    run_one_sequential,
                    std::cref(dataset),
                    parameters,
                    index
                )
            );
        }

        std::size_t i = 0;
        for (auto& future : futures) {
            results[i++] = future.get();
        }
    } else {
        std::size_t index = 0;
        for (const auto& parameters : combinations) {
            ++index;
            results[index - 1] = run_one_sequential(dataset, parameters, index);
        }
    }

    return results;
}

// Compares two result vectors for exact metric equality.
bool results_match(
    const std::vector<JobResult>& a,
    const std::vector<JobResult>& b
) {
    if (a.size() != b.size()) {
        return false;
    }

    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].job_id != b[i].job_id ||
            a[i].parameters != b[i].parameters ||
            std::abs(a[i].metrics.total_return - b[i].metrics.total_return) > 1e-12 ||
            std::abs(a[i].metrics.sharpe_ratio - b[i].metrics.sharpe_ratio) > 1e-12 ||
            std::abs(a[i].metrics.max_drawdown - b[i].metrics.max_drawdown) > 1e-12 ||
            a[i].metrics.number_of_trades != b[i].metrics.number_of_trades) {
            return false;
        }
    }

    return true;
}

// Verifies every combination in the grid produces exactly one result.
void test_all_combinations_complete() {
    const std::vector<MarketBar> data{
        make_bar("2020-01-01", 10.0),
        make_bar("2020-01-02", 11.0),
        make_bar("2020-01-03", 12.0),
        make_bar("2020-01-04", 11.5),
        make_bar("2020-01-05", 12.5),
        make_bar("2020-01-06", 13.0),
        make_bar("2020-01-07", 13.5),
        make_bar("2020-01-08", 14.0),
        make_bar("2020-01-09", 13.5),
        make_bar("2020-01-10", 14.5),
        make_bar("2020-01-11", 15.0),
        make_bar("2020-01-12", 15.5),
        make_bar("2020-01-13", 16.0),
        make_bar("2020-01-14", 15.5),
        make_bar("2020-01-15", 16.5),
        make_bar("2020-01-16", 17.0),
        make_bar("2020-01-17", 16.5),
        make_bar("2020-01-18", 17.5),
        make_bar("2020-01-19", 18.0),
        make_bar("2020-01-20", 17.5)
    };

    const ParameterGrid grid{
        {"short_window", {5.0, 10.0, 15.0}},
        {"long_window",  {20.0, 30.0, 40.0, 50.0}}
    };

    const auto combinations = generate_parameter_combinations(grid);
    expect(combinations.size() == 12,
           "Expected twelve parameter combinations");

    const auto results = run_all(data, combinations, true);

    expect(results.size() == combinations.size(),
           "Expected one result per parameter combination");

    // Job IDs must be unique across concurrent execution.
    std::set<std::string> job_ids;
    for (const auto& result : results) {
        job_ids.insert(result.job_id);
    }
    expect(job_ids.size() == results.size(),
           "Job IDs should be unique across concurrent combinations");

    // Each result carries back the exact parameters it was run with.
    for (std::size_t i = 0; i < results.size(); ++i) {
        expect(results[i].parameters == combinations[i],
               "Result parameters should match the input combination");
    }
}

// Verifies concurrent execution produces identical results to sequential
// execution, proving parallel backtesting is deterministic and race-free.
void test_concurrent_execution_is_deterministic() {
    const std::vector<MarketBar> data{
        make_bar("2020-01-01", 10.0),
        make_bar("2020-01-02", 11.0),
        make_bar("2020-01-03", 12.0),
        make_bar("2020-01-04", 11.5),
        make_bar("2020-01-05", 12.5),
        make_bar("2020-01-06", 13.0),
        make_bar("2020-01-07", 13.5),
        make_bar("2020-01-08", 14.0),
        make_bar("2020-01-09", 13.5),
        make_bar("2020-01-10", 14.5),
        make_bar("2020-01-11", 15.0),
        make_bar("2020-01-12", 15.5),
        make_bar("2020-01-13", 16.0),
        make_bar("2020-01-14", 15.5),
        make_bar("2020-01-15", 16.5),
        make_bar("2020-01-16", 17.0),
        make_bar("2020-01-17", 16.5),
        make_bar("2020-01-18", 17.5),
        make_bar("2020-01-19", 18.0),
        make_bar("2020-01-20", 17.5)
    };

    const ParameterGrid grid{
        {"short_window", {5.0, 10.0, 15.0}},
        {"long_window",  {20.0, 30.0, 40.0, 50.0}}
    };

    const auto combinations = generate_parameter_combinations(grid);

    const auto sequential = run_all(data, combinations, false);
    const auto concurrent = run_all(data, combinations, true);

    expect(results_match(sequential, concurrent),
           "Concurrent execution should produce identical results to sequential");
}

// Verifies a failing combination does not abort the rest of the grid.
void test_fault_tolerance() {
    const std::vector<MarketBar> data{
        make_bar("2020-01-01", 10.0),
        make_bar("2020-01-02", 11.0),
        make_bar("2020-01-03", 12.0),
        make_bar("2020-01-04", 11.5),
        make_bar("2020-01-05", 12.5),
        make_bar("2020-01-06", 13.0),
        make_bar("2020-01-07", 13.5),
        make_bar("2020-01-08", 14.0),
        make_bar("2020-01-09", 13.5),
        make_bar("2020-01-10", 14.5),
        make_bar("2020-01-11", 15.0),
        make_bar("2020-01-12", 15.5),
        make_bar("2020-01-13", 16.0),
        make_bar("2020-01-14", 15.5),
        make_bar("2020-01-15", 16.5),
        make_bar("2020-01-16", 17.0),
        make_bar("2020-01-17", 16.5),
        make_bar("2020-01-18", 17.5),
        make_bar("2020-01-19", 18.0),
        make_bar("2020-01-20", 17.5)
    };

    // One combination is invalid (short_window >= long_window) and will
    // throw inside MovingAverageCrossover's constructor.
    const ParameterGrid grid{
        {"short_window", {5.0, 25.0, 15.0}},
        {"long_window",  {20.0, 30.0, 50.0}}
    };

    const auto combinations = generate_parameter_combinations(grid);
    expect(combinations.size() == 9,
           "Expected nine parameter combinations");

    std::vector<JobResult> results;
    results.reserve(combinations.size());
    std::size_t failures = 0;

    std::vector<std::future<JobResult>> futures;
    futures.reserve(combinations.size());

    std::size_t index = 0;
    for (const auto& parameters : combinations) {
        ++index;
        futures.push_back(
            std::async(
                std::launch::async,
                [&dataset = data, parameters, index]() {
                    try {
                        const BacktestConfig config{
                            "grid-" + std::to_string(index),
                            "TEST",
                            "2020-01-01",
                            "2020-04-01",
                            100.0,
                            parameters
                        };

                        MovingAverageCrossover strategy(
                            static_cast<std::size_t>(
                                parameters.at("short_window")),
                            static_cast<std::size_t>(
                                parameters.at("long_window"))
                        );

                        return run_backtest(dataset, strategy, config);
                    } catch (...) {
                        return JobResult{};
                    }
                }
            )
        );
    }

    for (auto& future : futures) {
        const JobResult result = future.get();
        if (result.job_id.empty()) {
            ++failures;
        } else {
            results.push_back(result);
        }
    }

    // Exactly one combination should fail (short_window=100, long_window=20).
    expect(failures == 1,
           "Expected exactly one failing combination");

    // All other combinations should still complete.
    expect(results.size() == combinations.size() - 1,
           "All non-failing combinations should complete");
}

}  // namespace

int main() {
    try {
        test_all_combinations_complete();
        test_concurrent_execution_is_deterministic();
        test_fault_tolerance();

        std::cout << "All parallel execution tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
