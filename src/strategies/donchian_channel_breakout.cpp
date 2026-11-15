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
    synchronize(history);

    const std::size_t n = history.size();

    if (!has_position) {
        // The entry channel covers the `entry_period` bars before the
        // current bar, so one extra bar of history is required.
        if (n < entry_period_ + 1) {
            return Signal::Hold;
        }

        const double channel_high =
            history[entry_highs_.front()].high;

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

    const double channel_low = history[exit_lows_.front()].low;

    if (history[n - 1].close < channel_low) {
        return Signal::Sell;
    }

    return Signal::Hold;
}

void DonchianChannelBreakout::synchronize(
    const std::vector<MarketBar>& history
) {
    const bool continues_previous =
        bars_processed_ > 0 &&
        history.size() >= bars_processed_ &&
        history[bars_processed_ - 1].close == previous_close_;

    if (!continues_previous) {
        reset_state();
    }

    // The last bar is the decision bar and is excluded from both channels.
    // Fold every earlier bar not seen yet into the monotonic deques.
    for (std::size_t i = enqueued_; i + 1 < history.size(); ++i) {
        const MarketBar& bar = history[i];

        while (!entry_highs_.empty() &&
               history[entry_highs_.back()].high <= bar.high) {
            entry_highs_.pop_back();
        }
        entry_highs_.push_back(i);

        while (!exit_lows_.empty() &&
               history[exit_lows_.back()].low >= bar.low) {
            exit_lows_.pop_back();
        }
        exit_lows_.push_back(i);

        ++enqueued_;

        // Each deque keeps only the most recent window of prior bars.
        if (enqueued_ > entry_period_) {
            const std::size_t oldest = enqueued_ - entry_period_;

            while (!entry_highs_.empty() &&
                   entry_highs_.front() < oldest) {
                entry_highs_.pop_front();
            }
        }

        if (enqueued_ > exit_period_) {
            const std::size_t oldest = enqueued_ - exit_period_;

            while (!exit_lows_.empty() &&
                   exit_lows_.front() < oldest) {
                exit_lows_.pop_front();
            }
        }
    }

    if (history.size() > bars_processed_) {
        bars_processed_ = history.size();
        previous_close_ = history.back().close;
    }
}

void DonchianChannelBreakout::reset_state() {
    entry_highs_.clear();
    exit_lows_.clear();
    enqueued_ = 0;
    bars_processed_ = 0;
    previous_close_ = 0.0;
}
