#include "strategies/moving_average_crossover.h"

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
    const std::vector<double>& closes) {

    std::vector<MarketBar> history;
    history.reserve(closes.size());

    for (std::size_t i = 0; i < closes.size(); ++i) {
        history.push_back(make_bar(
            "2020-01-" + std::string(i + 1 < 10 ? "0" : "")
                + std::to_string(i + 1),
            closes[i]));
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
    const std::string& message) {

    expect(actual == expected, message);
}

void expect_invalid_windows(
    std::size_t short_window,
    std::size_t long_window) {

    bool threw = false;

    try {
        MovingAverageCrossover strategy(short_window, long_window);
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    expect(threw, "Expected invalid_argument for invalid windows");
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

void test_invalid_windows() {
    expect_invalid_windows(0, 5);
    expect_invalid_windows(3, 0);
    expect_invalid_windows(5, 5);
    expect_invalid_windows(6, 5);
}

void test_valid_windows() {
    MovingAverageCrossover strategy(2, 3);
    (void)strategy;
}

void test_insufficient_history() {
    MovingAverageCrossover strategy(2, 3);
    const auto history = make_history({10, 9, 8, 7});

    expect_signal(
        strategy.on_bar(history, false),
        Signal::Hold,
        "Expected Hold before crossover can be evaluated");
}

void test_no_crossover() {
    MovingAverageCrossover strategy(2, 3);
    const auto history = make_history({10, 10, 10, 10, 10});

    expect_signal(
        strategy.on_bar(history, false),
        Signal::Hold,
        "Equal averages should not produce a crossover");
}

void test_buy_crossover() {
    MovingAverageCrossover strategy(2, 3);

    // Before the final bar, short SMA = 8.5 and long SMA = 9.
    // After the final bar, short SMA = 11 and long SMA = 10.
    const auto history = make_history({10, 9, 8, 9, 13});

    expect_signal(
        strategy.on_bar(history, false),
        Signal::Buy,
        "Expected Buy when short SMA crosses above long SMA");
}

void test_sell_crossover() {
    MovingAverageCrossover strategy(2, 3);

    // Before the final bar, short SMA = 11 and long SMA = 10.
    // After the final bar, short SMA = 8.5 and long SMA = 9.
    const auto history = make_history({8, 9, 10, 13, 4});

    expect_signal(
        strategy.on_bar(history, true),
        Signal::Sell,
        "Expected Sell when short SMA crosses below long SMA");
}

void test_no_repeated_buy_while_in_position() {
    MovingAverageCrossover strategy(2, 3);

    const auto history = make_history({10, 9, 8, 9, 13});

    expect_signal(
        strategy.on_bar(history, true),
        Signal::Hold,
        "Should not Buy when already holding a position");
}

void test_no_sell_while_flat() {
    MovingAverageCrossover strategy(2, 3);

    const auto history = make_history({8, 9, 10, 13, 4});

    expect_signal(
        strategy.on_bar(history, false),
        Signal::Hold,
        "Should not Sell when there is no position");
}

void test_no_signal_without_crossover() {
    MovingAverageCrossover strategy(2, 3);

    // Short average remains above long average, but does not cross.
    const auto history = make_history({8, 9, 10, 11, 12});

    expect_signal(
        strategy.on_bar(history, false),
        Signal::Hold,
        "Being above the long SMA is not itself a crossover");
}

} // namespace

int main() {
    run_test("invalid windows rejected", test_invalid_windows);
    run_test("valid windows accepted", test_valid_windows);
    run_test("insufficient history returns Hold", test_insufficient_history);
    run_test("no crossover returns Hold", test_no_crossover);
    run_test("bullish crossover returns Buy", test_buy_crossover);
    run_test("bearish crossover returns Sell", test_sell_crossover);
    run_test("no repeated Buy while in position",
             test_no_repeated_buy_while_in_position);
    run_test("no Sell while flat", test_no_sell_while_flat);
    run_test("relative position alone does not trigger trade",
             test_no_signal_without_crossover);

    std::cout << "\nResults: " << passed << " passed, "
              << failed << " failed\n";

    return failed == 0 ? 0 : 1;
}