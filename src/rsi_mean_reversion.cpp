#include "rsi_mean_reversion.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

RsiMeanReversion::RsiMeanReversion(
    std::size_t period,
    double oversold_threshold,
    double exit_threshold
)
    : period_(period),
      oversold_threshold_(oversold_threshold),
      exit_threshold_(exit_threshold) {
    if (period_ == 0) {
        throw std::invalid_argument("RSI period must be positive");
    }

    if (!std::isfinite(oversold_threshold_) ||
        oversold_threshold_ <= 0.0 ||
        oversold_threshold_ >= 100.0) {
        throw std::invalid_argument(
            "Oversold threshold must be in (0, 100)"
        );
    }

    if (!std::isfinite(exit_threshold_) ||
        exit_threshold_ <= oversold_threshold_ ||
        exit_threshold_ >= 100.0) {
        throw std::invalid_argument(
            "Exit threshold must be greater than the oversold threshold "
            "and less than 100"
        );
    }
}

Signal RsiMeanReversion::on_bar(
    const std::vector<MarketBar>& history,
    bool has_position
) {
    // The first RSI value needs `period` consecutive changes, so at least
    // `period + 1` closes are required before a signal can be evaluated.
    if (history.size() < period_ + 1) {
        return Signal::Hold;
    }

    const double rsi = calculate_rsi(history);

    if (!std::isfinite(rsi)) {
        return Signal::Hold;
    }

    // Buy when the RSI falls below the oversold threshold...
    if (!has_position && rsi < oversold_threshold_) {
        return Signal::Buy;
    }

    // ...and sell when it rises above the exit threshold.
    if (has_position && rsi > exit_threshold_) {
        return Signal::Sell;
    }

    return Signal::Hold;
}

double RsiMeanReversion::calculate_rsi(
    const std::vector<MarketBar>& history
) const {
    const std::size_t period = period_;
    const std::size_t n = history.size();

    if (n < period + 1) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    // Wilder's smoothing: seed the average gain/loss with the simple
    // average of the first `period` changes, then recursively smooth each
    // subsequent change.
    double avg_gain = 0.0;
    double avg_loss = 0.0;

    for (std::size_t i = 0; i < period; ++i) {
        const double change = history[i + 1].close - history[i].close;
        avg_gain += std::max(change, 0.0);
        avg_loss += std::max(-change, 0.0);
    }

    avg_gain /= static_cast<double>(period);
    avg_loss /= static_cast<double>(period);

    for (std::size_t i = period; i + 1 < n; ++i) {
        const double change = history[i + 1].close - history[i].close;
        const double gain = std::max(change, 0.0);
        const double loss = std::max(-change, 0.0);

        avg_gain = (avg_gain * (static_cast<double>(period) - 1.0) +
                    gain) / static_cast<double>(period);
        avg_loss = (avg_loss * (static_cast<double>(period) - 1.0) +
                    loss) / static_cast<double>(period);
    }

    if (avg_loss == 0.0) {
        // No losses in the window: the asset is fully overbought.
        return 100.0;
    }

    if (avg_gain == 0.0) {
        // No gains in the window: the asset is fully oversold.
        return 0.0;
    }

    return 100.0 * avg_gain / (avg_gain + avg_loss);
}
