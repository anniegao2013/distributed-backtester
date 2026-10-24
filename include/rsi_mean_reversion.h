#pragma once

#include "strategy.h"

#include <cstddef>

// Long-only mean-reversion strategy: buys when the Relative Strength
// Index (RSI) falls below the oversold threshold and sells when it rises
// above the exit threshold.
//
// Wilder's RSI is maintained incrementally: on_bar folds only the bars
// appended since the previous call, so a long warm-up costs O(1) per bar
// instead of recomputing over the entire history.
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

    // Incremental Wilder state.
    std::size_t bars_processed_ = 0;  // closes folded into the state
    std::size_t changes_seen_ = 0;    // changes folded, capped at period_
    double previous_close_ = 0.0;
    double seed_gain_sum_ = 0.0;
    double seed_loss_sum_ = 0.0;
    double avg_gain_ = 0.0;
    double avg_loss_ = 0.0;

    // Folds any bars in history not yet reflected in the RSI state. If
    // history is not a continuation of the series seen so far, the state
    // is rebuilt from scratch so it never reflects stale prices.
    void synchronize(const std::vector<MarketBar>& history);

    void reset_state();
    void fold_change(double close);

    bool has_rsi() const;
    double current_rsi() const;
};
