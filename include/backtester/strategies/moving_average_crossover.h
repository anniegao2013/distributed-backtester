#pragma once

#include "trading/strategy.h"

#include <cstddef>
#include <vector>

class MovingAverageCrossover final : public Strategy {
public:
    MovingAverageCrossover(
        std::size_t short_window,
        std::size_t long_window
    );

    Signal on_bar(
        const std::vector<MarketBar>& history,
        bool has_position
    ) override;

private:
    std::size_t short_window_;
    std::size_t long_window_;

    // prefix_sums_[k] is the sum of the closes of bars [0, k). Keeping the
    // running prefix lets each window average be evaluated in O(1) instead of
    // rescanning the window on every bar.
    std::vector<double> prefix_sums_{0.0};
    std::size_t bars_processed_ = 0;
    double previous_close_ = 0.0;

    // Folds any bars in history not yet reflected in the prefix sums. If
    // history is not a continuation of the series seen so far, the state is
    // rebuilt from scratch so it never reflects stale prices.
    void synchronize(const std::vector<MarketBar>& history);

    void reset_state();

    double window_average(std::size_t end, std::size_t window) const;
};
