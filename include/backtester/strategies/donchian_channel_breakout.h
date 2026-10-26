#pragma once

#include "trading/strategy.h"

#include <cstddef>

// Long-only Donchian channel breakout strategy: buys when the close
// breaks above the highest high of the previous `entry_period` bars and
// sells when the close falls below the lowest low of the previous
// `exit_period` bars (the trailing exit channel).
//
// Channels use the bars before the current bar, so a new high or low can
// actually break out of the range that preceded it.
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
};
