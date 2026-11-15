#include "strategies/moving_average_crossover.h"

#include <numeric>
#include <stdexcept>

MovingAverageCrossover::MovingAverageCrossover(
    std::size_t short_window,
    std::size_t long_window
)
    : short_window_(short_window),
      long_window_(long_window) {

    if (short_window_ == 0 ||
        long_window_ == 0 ||
        short_window_ >= long_window_) {
        throw std::invalid_argument(
            "Moving-average windows must satisfy "
            "0 < short window < long window");
    }
}

Signal MovingAverageCrossover::on_bar(
    const std::vector<MarketBar>& history,
    bool has_position
) {
    synchronize(history);

    // Need a full long window and a previous observation
    // to detect a crossover.
    if (history.size() < long_window_ + 1) {
        return Signal::Hold;
    }

    const std::size_t n = history.size();

    // Calculate averages through the current bar.
    const double short_now = window_average(n, short_window_);
    const double long_now = window_average(n, long_window_);

    // Calculate averages through the previous bar.
    const double short_prev = window_average(n - 1, short_window_);
    const double long_prev = window_average(n - 1, long_window_);

    const bool crossed_above =
        short_prev <= long_prev && short_now > long_now;

    const bool crossed_below =
        short_prev >= long_prev && short_now < long_now;

    if (crossed_above && !has_position) {
        return Signal::Buy;
    }

    if (crossed_below && has_position) {
        return Signal::Sell;
    }

    return Signal::Hold;
}

void MovingAverageCrossover::synchronize(
    const std::vector<MarketBar>& history
) {
    // on_bar is normally called with one more bar each time. If the caller
    // supplies a different series, rebuild rather than carrying stale sums.
    const bool continues_previous =
        bars_processed_ > 0 &&
        history.size() >= bars_processed_ &&
        history[bars_processed_ - 1].close == previous_close_;

    if (!continues_previous) {
        reset_state();
    }

    if (history.size() > bars_processed_) {
        // Grow geometrically through push_back. Reserving the exact size on
        // every call would reallocate and copy the whole prefix each bar.
        for (std::size_t i = bars_processed_; i < history.size(); ++i) {
            prefix_sums_.push_back(
                prefix_sums_.back() + history[i].close);
        }

        bars_processed_ = history.size();
        previous_close_ = history.back().close;
    }
}

void MovingAverageCrossover::reset_state() {
    bars_processed_ = 0;
    previous_close_ = 0.0;
    prefix_sums_.assign(1, 0.0);
}

double MovingAverageCrossover::window_average(
    std::size_t end,
    std::size_t window
) const {
    const std::size_t begin = end - window;

    return (prefix_sums_[end] - prefix_sums_[begin]) /
        static_cast<double>(window);
}