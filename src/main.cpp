#include "market_data.h"
#include "moving_average_crossover.h"
#include "backtest_engine.h"

#include <exception>
#include <iostream>
#include <iomanip>

int main() {
    try {
        const auto bars =
            load_market_data("data/market/sp500_1950_2018.csv");

        std::cout << "Successfully loaded " << bars.size()
                  << " trading days.\n";

        std::cout << "First date: " << bars.front().date << '\n';
        std::cout << "Last date:  " << bars.back().date << '\n';

        const auto& first = bars.front();
        std::cout << "First close: " << first.close << '\n';

    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

        try {
        const auto dataset = load_market_data(
            "data/market/sp500_1950_2018.csv"
        );

        const BacktestConfig config{
            "job-001",
            "SP500",
            "2000-01-01",
            "2010-12-31",
            10000.0,
            {
                {"short_window", 20},
                {"long_window", 50}
            }
        };

        MovingAverageCrossover strategy(20, 50);
        const JobResult result =
            run_backtest(dataset, strategy, config);

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
        std::cout << "  Sharpe ratio: "
                  << result.metrics.sharpe_ratio << '\n';
        std::cout << "  Max drawdown: "
                  << result.metrics.max_drawdown * 100.0 << "%\n";
        std::cout << "  Number of trades: "
                  << result.metrics.number_of_trades << '\n';

    } catch (const std::exception& error) {
        std::cerr << "Backtest failed: " << error.what() << '\n';
        return 1;
    }

    return 0;
}