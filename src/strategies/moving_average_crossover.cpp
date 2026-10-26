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
    // Need a full long window and a previous observation
    // to detect a crossover.
    if (history.size() < long_window_ + 1) {
        return Signal::Hold;
    }

    const auto average_close = [&history](
        std::size_t end,
        std::size_t window) {
        const auto begin = end - window;

        double sum = 0.0;

        for (std::size_t i = begin; i < end; ++i) {
            sum += history[i].close;
        }

        return sum / static_cast<double>(window);
    };

    const std::size_t n = history.size();

    // Calculate averages through the current bar.
    const double short_now =
        average_close(n, short_window_);
    const double long_now =
        average_close(n, long_window_);

    // Calculate averages through the previous bar.
    const double short_prev =
        average_close(n - 1, short_window_);
    const double long_prev =
        average_close(n - 1, long_window_);

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