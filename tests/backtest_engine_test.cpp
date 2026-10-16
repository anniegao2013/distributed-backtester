#include "backtest_engine.h"
#include "moving_average_crossover.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <string>

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

class TestStrategy final : public Strategy {
public:
    Signal on_bar(
        const std::vector<MarketBar>& history,
        bool has_position
    ) override {
        if (history.size() == 1 && !has_position) {
            return Signal::Buy;
        }

        if (history.size() == 3 && has_position) {
            return Signal::Sell;
        }

        return Signal::Hold;
    }
};

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_buy_sell_accounting() {
    const std::vector<MarketBar> data{
        make_bar("2020-01-01", 10.0),
        make_bar("2020-01-02", 12.0),
        make_bar("2020-01-03", 13.0),
        make_bar("2020-01-04", 14.0)
    };

    const BacktestConfig config{
        "test-001",
        "TEST",
        "2020-01-01",
        "2020-01-04",
        100.0,
        {}
    };

    TestStrategy strategy;
    const JobResult result =
        run_backtest(data, strategy, config);

    // Buy at 12, sell at 14:
    // final value = 100 * (14 / 12).
    const double expected_return = 14.0 / 12.0 - 1.0;

    expect(result.job_id == "test-001", "Incorrect job ID");
    expect(result.asset == "TEST", "Incorrect asset");
    expect(
        std::abs(result.metrics.total_return - expected_return) < 1e-9,
        "Incorrect total return"
    );
    expect(
        result.metrics.number_of_trades == 1,
        "Expected one completed trade"
    );
    expect(
        result.metrics.max_drawdown == 0.0,
        "Expected zero drawdown"
    );
}

void test_empty_date_range_throws() {
    const std::vector<MarketBar> data{
        make_bar("2020-01-01", 10.0)
    };

    const BacktestConfig config{
        "test-002",
        "TEST",
        "2021-01-01",
        "2021-12-31",
        100.0,
        {}
    };

    TestStrategy strategy;
    bool threw = false;

    try {
        (void)run_backtest(data, strategy, config);
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    expect(threw, "Expected empty date range to throw");
}

MarketBar make_ohlc_bar(
    const std::string& date,
    double open,
    double high,
    double low,
    double close
) {
    return MarketBar{
        date,
        open,
        high,
        low,
        close,
        close,
        1000
    };
}

// Buys after the first bar's close, then holds indefinitely.
class BuyAndHoldStrategy final : public Strategy {
public:
    Signal on_bar(
        const std::vector<MarketBar>& history,
        bool has_position
    ) override {
        if (history.size() == 1 && !has_position) {
            return Signal::Buy;
        }

        return Signal::Hold;
    }
};

// Generates a buy signal after the specified bar's close.
class BuyAtBarStrategy final : public Strategy {
public:
    explicit BuyAtBarStrategy(std::size_t signal_bar)
        : signal_bar_(signal_bar) {}

    Signal on_bar(
        const std::vector<MarketBar>& history,
        bool has_position
    ) override {
        if (history.size() == signal_bar_ && !has_position) {
            return Signal::Buy;
        }

        return Signal::Hold;
    }

private:
    std::size_t signal_bar_;
};

void test_max_drawdown() {
    const std::vector<MarketBar> data{
        make_bar("2020-01-01", 10.0),
        make_ohlc_bar("2020-01-02", 10.0, 20.0, 10.0, 20.0),
        make_ohlc_bar("2020-01-03", 20.0, 20.0, 10.0, 10.0)
    };

    const BacktestConfig config{
        "drawdown-test",
        "TEST",
        "2020-01-01",
        "2020-01-03",
        100.0,
        {}
    };

    BuyAndHoldStrategy strategy;
    const JobResult result = run_backtest(data, strategy, config);

    // Buy 10 shares at $10.
    // Equity peaks at $200, then falls to $100.
    // Drawdown = (200 - 100) / 200 = 0.50.
    expect(
        std::abs(result.metrics.max_drawdown - 0.50) < 1e-9,
        "Expected maximum drawdown of 50%"
    );
}

void test_sharpe_ratio() {
    const std::vector<MarketBar> data{
        make_bar("2020-01-01", 10.0),
        make_ohlc_bar("2020-01-02", 10.0, 11.0, 10.0, 11.0),
        make_ohlc_bar("2020-01-03", 11.0, 12.1, 11.0, 12.1),
        make_bar("2020-01-04", 12.1)
    };

    const BacktestConfig config{
        "sharpe-test",
        "TEST",
        "2020-01-01",
        "2020-01-04",
        100.0,
        {}
    };

    BuyAndHoldStrategy strategy;
    const JobResult result = run_backtest(data, strategy, config);

    // Equity: 100, 110, 121, 121.
    // Daily returns: 0, 0.1, 0.1, 0.
    const double mean_return = 0.05;
    const double sample_stddev = std::sqrt(0.01 / 3.0);
    const double expected_sharpe =
        (mean_return / sample_stddev) * std::sqrt(252.0);

    expect(
        std::abs(
            result.metrics.sharpe_ratio - expected_sharpe
        ) < 1e-9,
        "Incorrect annualized Sharpe ratio"
    );
}

void test_sharpe_ratio_zero_volatility() {
    const std::vector<MarketBar> data{
        make_bar("2020-01-01", 10.0),
        make_bar("2020-01-02", 10.0),
        make_bar("2020-01-03", 10.0)
    };

    const BacktestConfig config{
        "zero-volatility-test",
        "TEST",
        "2020-01-01",
        "2020-01-03",
        100.0,
        {}
    };

    BuyAtBarStrategy strategy(3);
    const JobResult result = run_backtest(data, strategy, config);

    expect(
        result.metrics.sharpe_ratio == 0.0,
        "Expected zero Sharpe ratio for zero volatility"
    );
}


void test_open_position_marked_to_market() {
    const std::vector<MarketBar> data{
        make_bar("2020-01-01", 10.0),
        make_ohlc_bar("2020-01-02", 12.0, 15.0, 12.0, 15.0)
    };

    const BacktestConfig config{
        "final-position-test",
        "TEST",
        "2020-01-01",
        "2020-01-02",
        100.0,
        {}
    };

    BuyAndHoldStrategy strategy;
    const JobResult result = run_backtest(data, strategy, config);

    // Buy at the second bar's open ($12).
    // Final equity = $100 * ($15 / $12) = $125.
    // Total return = ($125 - $100) / $100 = 25%.
    expect(
        std::abs(result.metrics.total_return - 0.25) < 1e-9,
        "Open position should be valued at the final close"
    );

    expect(
        result.metrics.number_of_trades == 0,
        "Open position should not count as a completed trade"
    );
}


void test_final_bar_signal_not_executed() {
    const std::vector<MarketBar> data{
        make_bar("2020-01-01", 10.0),
        make_bar("2020-01-02", 12.0)
    };

    const BacktestConfig config{
        "pending-signal-test",
        "TEST",
        "2020-01-01",
        "2020-01-02",
        100.0,
        {}
    };

    // The buy signal is generated after the final bar's close.
    BuyAtBarStrategy strategy(2);
    const JobResult result = run_backtest(data, strategy, config);

    expect(
        std::abs(result.metrics.total_return) < 1e-9,
        "Final-bar buy signal must not change portfolio value"
    );

    expect(
        result.metrics.number_of_trades == 0,
        "Unexecuted final-bar signal must not count as a trade"
    );
}

void test_signal_executes_at_next_open() {
    const std::vector<MarketBar> data{
        make_bar("2020-01-01", 10.0),
        make_ohlc_bar(
            "2020-01-02",
            20.0,  // Open
            25.0,  // High
            15.0,  // Low
            22.0   // Close
        ),
        make_ohlc_bar(
            "2020-01-03",
            30.0,  // Open
            35.0,  // High
            28.0,  // Low
            33.0   // Close
        )
    };

    const BacktestConfig config{
        "next-open-test",
        "TEST",
        "2020-01-01",
        "2020-01-03",
        100.0,
        {}
    };

    BuyAndHoldStrategy strategy;
    const JobResult result = run_backtest(data, strategy, config);

    // The strategy signals BUY after Jan 1 closes at $10.
    // The engine should buy at Jan 2's open of $20.
    // It should hold through Jan 3's close of $33.
    // Final equity = ($100 / $20) * $33 = $165.
    // Total return = 65%.
    expect(
        std::abs(result.metrics.total_return - 0.65) < 1e-9,
        "Buy signal should execute at the next bar's open"
    );
}

void test_pre_range_warmup() {
    const std::vector<MarketBar> data{
        make_bar("2020-01-01", 10.0),
        make_bar("2020-01-02", 9.0),
        make_bar("2020-01-03", 8.0),
        make_bar("2020-01-04", 9.0),

        // First day of the backtest: crossover occurs at this close.
        make_bar("2020-01-05", 13.0),

        // The buy executes here, at the open of $14.
        make_ohlc_bar(
            "2020-01-06", 14.0, 16.0, 14.0, 15.0
        )
    };

    const BacktestConfig config{
        "warmup-test",
        "TEST",
        "2020-01-05",
        "2020-01-06",
        100.0,
        {
            {"short_window", 2},
            {"long_window", 3}
        }
    };

    MovingAverageCrossover strategy(2, 3);
    const JobResult result = run_backtest(data, strategy, config);

    // The Jan 5 signal uses earlier bars to detect the crossover.
    // It executes at $14 on Jan 6, and the position is valued at $15.
    const double expected_return = 15.0 / 14.0 - 1.0;

    expect(
        std::abs(
            result.metrics.total_return - expected_return
        ) < 1e-9,
        "Warm-up history should allow a crossover on the first backtest bar"
    );

    expect(
        result.metrics.number_of_trades == 0,
        "The open final position should not count as a completed trade"
    );
}


void test_reversed_date_range_throws() {
    std::vector<MarketBar> dataset = {
        make_bar("2024-01-01", 100.0),
        make_bar("2024-01-02", 110.0),
        make_bar("2024-01-03", 120.0)
    };

    BuyAndHoldStrategy strategy;

    BacktestConfig config;
    config.job_id = "test-reversed-range";
    config.asset = "TEST";
    config.start_date = "2024-01-03";
    config.end_date = "2024-01-01";

    bool threw = false;

    try {
        run_backtest(dataset, strategy, config);
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    expect(threw, "Reversed date range should throw");
}

void test_missing_in_range_data_throws() {
    std::vector<MarketBar> dataset = {
        make_bar("2024-01-01", 100.0),
        make_bar("2024-01-02", 110.0),
        make_bar("2024-01-03", 120.0)
    };

    BuyAndHoldStrategy strategy;

    BacktestConfig config;
    config.job_id = "test-missing-range";
    config.asset = "TEST";
    config.start_date = "2024-02-01";
    config.end_date = "2024-02-10";

    bool threw = false;

    try {
        run_backtest(dataset, strategy, config);
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    expect(threw, "Missing in-range market data should throw");
}

}  // namespace

int main() {
    try {
        test_buy_sell_accounting();
        test_empty_date_range_throws();

        test_max_drawdown();
        test_sharpe_ratio();
        test_sharpe_ratio_zero_volatility();
        test_open_position_marked_to_market();
        test_final_bar_signal_not_executed();
        test_signal_executes_at_next_open();

        test_pre_range_warmup();

        test_reversed_date_range_throws();
        test_missing_in_range_data_throws();

        std::cout << "All backtest engine tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}