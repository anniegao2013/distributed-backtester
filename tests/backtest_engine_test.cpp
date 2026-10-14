#include "backtest_engine.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
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

}  // namespace

int main() {
    try {
        test_buy_sell_accounting();
        test_empty_date_range_throws();

        std::cout << "All backtest engine tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}