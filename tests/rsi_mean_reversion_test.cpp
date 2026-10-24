#include "rsi_mean_reversion.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int passed = 0;
int failed = 0;

MarketBar make_bar(const std::string& date, double close) {
    return MarketBar{
        date,
        close,  // open
        close,  // high
        close,  // low
        close,  // close
        close,  // adjusted close
        1000    // volume
    };
}

std::vector<MarketBar> make_history(
    const std::vector<double>& closes
) {

    std::vector<MarketBar> history;
    history.reserve(closes.size());

    for (std::size_t i = 0; i < closes.size(); ++i) {
        history.push_back(make_bar(
            "2020-01-" + std::string(i + 1 < 10 ? "0" : "") +
                std::to_string(i + 1),
            closes[i]
        ));
    }

    return history;
}

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_signal(
    Signal actual,
    Signal expected,
    const std::string& message
) {

    expect(actual == expected, message);
}

void run_test(
    const std::string& name,
    const std::function<void()>& test) {

    try {
        test();
        ++passed;
        std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "[FAIL] " << name << ": "
                  << error.what() << '\n';
    }
}

void test_invalid_parameters_rejected() {
    bool period_threw = false;

    try {
        (void)RsiMeanReversion(0, 30.0, 70.0);
    } catch (const std::invalid_argument&) {
        period_threw = true;
    }

    expect(period_threw, "Expected invalid_argument for RSI period of 0");

    bool oversold_threw = false;

    try {
        (void)RsiMeanReversion(14, 0.0, 70.0);
    } catch (const std::invalid_argument&) {
        oversold_threw = true;
    }

    expect(
        oversold_threw,
        "Expected invalid_argument for non-positive oversold threshold"
    );

    bool exit_threw = false;

    try {
        (void)RsiMeanReversion(14, 30.0, 30.0);
    } catch (const std::invalid_argument&) {
        exit_threw = true;
    }

    expect(
        exit_threw,
        "Expected invalid_argument for exit threshold at or below oversold"
    );

    bool exit_too_high_threw = false;

    try {
        (void)RsiMeanReversion(14, 30.0, 100.0);
    } catch (const std::invalid_argument&) {
        exit_too_high_threw = true;
    }

    expect(
        exit_too_high_threw,
        "Expected invalid_argument for exit threshold of 100"
    );
}

void test_insufficient_history_returns_hold() {
    RsiMeanReversion strategy(3, 30.0, 70.0);
    const auto history = make_history({10.0, 9.0, 8.0});

    // An RSI period of 3 needs four closes before a signal can be evaluated.
    expect_signal(
        strategy.on_bar(history, false),
        Signal::Hold,
        "Expected Hold before enough history exists for an RSI"
    );
}

void test_buy_when_rsi_below_oversold() {
    RsiMeanReversion strategy(3, 30.0, 70.0);
    const auto history = make_history({10.0, 10.0, 10.0, 6.0});

    // The window is all losses, so the RSI is 0 and falls below oversold.
    expect_signal(
        strategy.on_bar(history, false),
        Signal::Buy,
        "Expected Buy when RSI falls below the oversold threshold"
    );
}

void test_hold_when_rsi_in_range() {
    RsiMeanReversion strategy(3, 30.0, 70.0);
    const auto history = make_history({10.0, 11.0, 12.0, 11.0});

    // RSI stays inside the neutral band, so no mean-reversion entry fires.
    expect_signal(
        strategy.on_bar(history, false),
        Signal::Hold,
        "Expected Hold when RSI is between the oversold and exit levels"
    );
}

void test_no_sell_while_flat() {
    RsiMeanReversion strategy(3, 30.0, 70.0);
    const auto history = make_history({10.0, 10.0, 10.0, 6.0, 20.0});

    // The RSI recovered above the exit level, but there is no position.
    expect_signal(
        strategy.on_bar(history, false),
        Signal::Hold,
        "Should not Sell when there is no position"
    );
}

void test_no_repeated_buy_while_in_position() {
    RsiMeanReversion strategy(3, 30.0, 70.0);
    const auto history = make_history({10.0, 10.0, 10.0, 6.0, 7.0});

    // The RSI is back below oversold, but the position is already open.
    expect_signal(
        strategy.on_bar(history, true),
        Signal::Hold,
        "Should not Buy when already holding a position"
    );
}

void test_sell_when_rsi_above_exit() {
    RsiMeanReversion strategy(3, 30.0, 70.0);

    // RSI of 0 on the fourth bar: buy signal.
    const auto buy_history = make_history({10.0, 10.0, 10.0, 6.0});
    expect_signal(
        strategy.on_bar(buy_history, false),
        Signal::Buy,
        "Expected Buy when RSI falls below the oversold threshold"
    );

    // The rally pushes the RSI back above the exit level: sell signal.
    const auto sell_history = make_history({10.0, 10.0, 10.0, 6.0, 20.0});
    expect_signal(
        strategy.on_bar(sell_history, true),
        Signal::Sell,
        "Expected Sell when RSI rises above the exit threshold"
    );
}

}  // namespace

int main() {
    try {
        run_test("invalid parameters rejected", test_invalid_parameters_rejected);
        run_test("insufficient history returns Hold", test_insufficient_history_returns_hold);
        run_test("buy when RSI below oversold", test_buy_when_rsi_below_oversold);
        run_test("hold when RSI in range", test_hold_when_rsi_in_range);
        run_test("no sell while flat", test_no_sell_while_flat);
        run_test("no repeated buy while in position", test_no_repeated_buy_while_in_position);
        run_test("sell when RSI above exit", test_sell_when_rsi_above_exit);

        std::cout << "\nResults: " << passed << " passed, "
                  << failed << " failed\n";

        return failed == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
