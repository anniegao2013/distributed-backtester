#include "donchian_channel_breakout.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int passed = 0;
int failed = 0;

MarketBar make_bar(
    const std::string& date,
    double high,
    double low,
    double close
) {
    return MarketBar{
        date,
        close,  // open
        high,
        low,
        close,  // close
        close,  // adjusted close
        1000    // volume
    };
}

// Builds a flat bar where open, high, low, and close are all the same.
MarketBar make_flat_bar(const std::string& date, double price) {
    return make_bar(date, price, price, price);
}

std::string make_date(std::size_t index) {
    return "2020-01-" + std::string(index + 1 < 10 ? "0" : "") +
           std::to_string(index + 1);
}

// Builds a history where each bar's high and low bracket its close, using
// the supplied highs and lows. `closes` supplies the final close of each
// bar and must be the same length as `highs`/`lows`.
std::vector<MarketBar> make_history(
    const std::vector<double>& highs,
    const std::vector<double>& lows,
    const std::vector<double>& closes
) {
    if (highs.size() != lows.size() || highs.size() != closes.size()) {
        throw std::runtime_error("History inputs must have equal length");
    }

    std::vector<MarketBar> history;
    history.reserve(closes.size());

    for (std::size_t i = 0; i < closes.size(); ++i) {
        history.push_back(make_bar(
            make_date(i),
            highs[i],
            lows[i],
            closes[i]
        ));
    }

    return history;
}

// Convenience overload for flat bars.
std::vector<MarketBar> make_history(const std::vector<double>& closes) {
    std::vector<MarketBar> history;
    history.reserve(closes.size());

    for (std::size_t i = 0; i < closes.size(); ++i) {
        history.push_back(make_flat_bar(make_date(i), closes[i]));
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
    bool entry_threw = false;

    try {
        (void)DonchianChannelBreakout(0, 5);
    } catch (const std::invalid_argument&) {
        entry_threw = true;
    }

    expect(
        entry_threw,
        "Expected invalid_argument for entry period of 0"
    );

    bool exit_threw = false;

    try {
        (void)DonchianChannelBreakout(20, 0);
    } catch (const std::invalid_argument&) {
        exit_threw = true;
    }

    expect(
        exit_threw,
        "Expected invalid_argument for exit period of 0"
    );
}

void test_insufficient_entry_history_returns_hold() {
    DonchianChannelBreakout strategy(3, 2);

    // Entry needs three prior bars plus the current one. A close far above
    // the range must still be ignored with only three bars of history.
    const auto history = make_history({10.0, 10.0, 10.0});

    expect_signal(
        strategy.on_bar(history, false),
        Signal::Hold,
        "Insufficient entry history should return Hold"
    );
}

void test_insufficient_exit_history_returns_hold() {
    DonchianChannelBreakout strategy(2, 5);

    // Holding a position, but the exit channel needs five prior bars plus
    // the current one.
    const auto history = make_history({10.0, 9.0, 8.0});

    expect_signal(
        strategy.on_bar(history, true),
        Signal::Hold,
        "Insufficient exit history should return Hold"
    );
}

void test_buy_on_breakout_above_previous_high() {
    DonchianChannelBreakout strategy(3, 2);

    // The previous three highs top out at 12; a close of 12.5 breaks out.
    const auto history = make_history(
        {10.0, 11.0, 12.0, 12.5},   // highs
        {9.0, 9.5, 10.0, 11.0},     // lows
        {9.5, 10.0, 11.0, 12.5}     // closes
    );

    expect_signal(
        strategy.on_bar(history, false),
        Signal::Buy,
        "Close above the previous N-bar high should buy"
    );
}

void test_hold_at_or_inside_channel_high() {
    DonchianChannelBreakout strategy(3, 2);

    // Closing exactly at the channel high is not a breakout...
    const auto at_high = make_history(
        {10.0, 11.0, 12.0, 12.0},
        {9.0, 9.5, 10.0, 11.0},
        {9.5, 10.0, 11.0, 12.0}
    );

    expect_signal(
        strategy.on_bar(at_high, false),
        Signal::Hold,
        "Close at the channel high should not buy"
    );

    // ...and neither is closing inside the channel.
    const auto inside = make_history(
        {10.0, 11.0, 12.0, 11.5},
        {9.0, 9.5, 10.0, 10.5},
        {9.5, 10.0, 11.0, 11.5}
    );

    expect_signal(
        strategy.on_bar(inside, false),
        Signal::Hold,
        "Close inside the channel should not buy"
    );
}

void test_sell_below_trailing_exit_low() {
    DonchianChannelBreakout strategy(5, 3);

    // The previous three lows bottom out at 8; a close of 7.5 breaks down.
    const auto history = make_history(
        {11.0, 10.5, 10.0, 9.5},
        {10.0, 9.0, 8.0, 7.5},
        {10.5, 9.5, 8.5, 7.5}
    );

    expect_signal(
        strategy.on_bar(history, true),
        Signal::Sell,
        "Close below the trailing exit low should sell"
    );
}

void test_no_sell_while_flat() {
    DonchianChannelBreakout strategy(3, 2);

    // Price is below every prior low, but without a position the strategy
    // may only consider the entry channel and must never emit Sell.
    const auto history = make_history(
        {12.0, 12.0, 12.0, 8.0},
        {11.0, 11.0, 11.0, 8.0},
        {11.5, 11.5, 11.5, 8.0}
    );

    expect_signal(
        strategy.on_bar(history, false),
        Signal::Hold,
        "A flat strategy should never sell"
    );
}

void test_no_repeated_buy_while_in_position() {
    DonchianChannelBreakout strategy(3, 2);

    // Price is above the entry channel, but we already hold a position and
    // have not broken the exit channel, so the strategy should hold.
    const auto history = make_history(
        {10.0, 10.0, 10.0, 20.0},
        {8.0, 8.0, 8.0, 19.0},
        {9.0, 9.0, 9.0, 20.0}
    );

    expect_signal(
        strategy.on_bar(history, true),
        Signal::Hold,
        "An open position should not re-buy on a breakout"
    );
}

void test_entry_channel_uses_highs_not_closes() {
    DonchianChannelBreakout strategy(3, 2);

    // The prior closes are all 9, but the prior highs reach 12. A close of
    // 11 must be compared against the high (12), not the close (9).
    const auto history = make_history(
        {10.0, 12.0, 10.0, 11.0},
        {9.0, 9.0, 9.0, 10.0},
        {9.0, 9.0, 9.0, 11.0}
    );

    expect_signal(
        strategy.on_bar(history, false),
        Signal::Hold,
        "Entry channel should use highs, not closes"
    );
}

void test_exit_channel_uses_lows_not_closes() {
    DonchianChannelBreakout strategy(5, 3);

    // The prior closes are all 10, but the prior lows reach 5. A close of
    // 6 must be compared against the low (5), not the close (10).
    const auto history = make_history(
        {11.0, 11.0, 11.0, 10.0},
        {8.0, 5.0, 8.0, 6.0},
        {10.0, 10.0, 10.0, 6.0}
    );

    expect_signal(
        strategy.on_bar(history, true),
        Signal::Hold,
        "Exit channel should use lows, not closes"
    );
}

}  // namespace

int main() {
    run_test("invalid parameters rejected", test_invalid_parameters_rejected);
    run_test("insufficient entry history returns Hold", test_insufficient_entry_history_returns_hold);
    run_test("insufficient exit history returns Hold", test_insufficient_exit_history_returns_hold);
    run_test("buy on breakout above previous high", test_buy_on_breakout_above_previous_high);
    run_test("hold at or inside channel high", test_hold_at_or_inside_channel_high);
    run_test("sell below trailing exit low", test_sell_below_trailing_exit_low);
    run_test("no sell while flat", test_no_sell_while_flat);
    run_test("no repeated buy while in position", test_no_repeated_buy_while_in_position);
    run_test("entry channel uses highs not closes", test_entry_channel_uses_highs_not_closes);
    run_test("exit channel uses lows not closes", test_exit_channel_uses_lows_not_closes);

    std::cout << "\nResults: " << passed << " passed, "
              << failed << " failed\n";

    return failed == 0 ? 0 : 1;
}
