#pragma once

#include "strategy.h"

#include <cstddef>

// Long-only mean-reversion strategy: buys when the Relative Strength
// Index (RSI) falls below the oversold threshold and sells when it rises
// above the exit threshold.
class RsiMeanReversion final : public Strategy {
public:
    RsiMeanReversion(
        std::size_t period,
        double oversold_threshold,
        double exit_threshold
    );

    Signal on_bar(
        const std::vector<MarketBar>& history,
        bool has_position
    ) override;

private:
    std::size_t period_;
    double oversold_threshold_;
    double exit_threshold_;

    // Wilder's smoothed Relative Strength Index over the closes in
    // history. Returns NaN when there is not enough history to compute it.
    double calculate_rsi(const std::vector<MarketBar>& history) const;
};
