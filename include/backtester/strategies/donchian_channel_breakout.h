#pragma once

#include "trading/strategy.h"

#include <cstddef>
#include <deque>
#include <vector>

// Long-only Donchian channel breakout strategy: buys when the close
// breaks above the highest high of the previous `entry_period` bars and
// sells when the close falls below the lowest low of the previous
// `exit_period` bars (the trailing exit channel).
//
// Channels use the bars before the current bar, so a new high or low can
// actually break out of the range that preceded it.
//
// The channel extremes are maintained with monotonic deques, so each bar
// costs O(1) amortized instead of rescanning the window.
class DonchianChannelBreakout final : public Strategy {
public:
    DonchianChannelBreakout(
        std::size_t entry_period,
        std::size_t exit_period
    );

    Signal on_bar(
        const std::vector<MarketBar>& history,
        bool has_position
    ) override;

private:
    std::size_t entry_period_;
    std::size_t exit_period_;

    // Indices of prior bars in each channel window. The high deque is
    // decreasing and the low deque is increasing, so the front of each is
    // the current channel extreme.
    std::deque<std::size_t> entry_highs_;
    std::deque<std::size_t> exit_lows_;

    std::size_t enqueued_ = 0;        // prior bars folded into the deques
    std::size_t bars_processed_ = 0;  // bars seen at the end of the last call
    double previous_close_ = 0.0;

    // Folds any prior bars in history not yet reflected in the deques. The
    // final bar is the decision bar and is deliberately excluded. If history
    // is not a continuation of the series seen so far, the state is rebuilt.
    void synchronize(const std::vector<MarketBar>& history);

    void reset_state();
};
