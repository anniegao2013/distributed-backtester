#include "strategies/donchian_channel_breakout.h"

#include <algorithm>
#include <stdexcept>

DonchianChannelBreakout::DonchianChannelBreakout(
    std::size_t entry_period,
    std::size_t exit_period
)
    : entry_period_(entry_period),
      exit_period_(exit_period) {
    if (entry_period_ == 0) {
        throw std::invalid_argument(
            "Donchian entry period must be positive"
        );
    }

    if (exit_period_ == 0) {
        throw std::invalid_argument(
            "Donchian exit period must be positive"
        );
    }
}

Signal DonchianChannelBreakout::on_bar(
    const std::vector<MarketBar>& history,
    bool has_position
) {
    const std::size_t n = history.size();

    if (!has_position) {
        // The entry channel covers the `entry_period` bars before the
        // current bar, so one extra bar of history is required.
        if (n < entry_period_ + 1) {
            return Signal::Hold;
        }

        double channel_high =
            history[n - entry_period_ - 1].high;

        for (std::size_t i = n - entry_period_; i + 1 < n; ++i) {
            channel_high = std::max(channel_high, history[i].high);
        }

        if (history[n - 1].close > channel_high) {
            return Signal::Buy;
        }

        return Signal::Hold;
    }

    // The trailing exit channel covers the `exit_period` bars before the
    // current bar.
    if (n < exit_period_ + 1) {
        return Signal::Hold;
    }

    double channel_low =
        history[n - exit_period_ - 1].low;

    for (std::size_t i = n - exit_period_; i + 1 < n; ++i) {
        channel_low = std::min(channel_low, history[i].low);
    }

    if (history[n - 1].close < channel_low) {
        return Signal::Sell;
    }

    return Signal::Hold;
}
