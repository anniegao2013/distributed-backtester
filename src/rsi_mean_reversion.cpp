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
    synchronize(history);

    if (!has_rsi()) {
        return Signal::Hold;
    }

    const double rsi = current_rsi();

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

void RsiMeanReversion::synchronize(
    const std::vector<MarketBar>& history
) {
    // The engine appends one bar per call, so the common path folds a
    // single change. If the caller provides a different series (shorter
    // history or a changed last close), rebuild instead of carrying stale
    // state across unrelated series.
    const bool continues_previous =
        bars_processed_ > 0 &&
        history.size() >= bars_processed_ &&
        history[bars_processed_ - 1].close == previous_close_;

    if (!continues_previous) {
        reset_state();
    }

    for (std::size_t i = bars_processed_; i < history.size(); ++i) {
        fold_change(history[i].close);
    }
}

void RsiMeanReversion::reset_state() {
    bars_processed_ = 0;
    changes_seen_ = 0;
    previous_close_ = 0.0;
    seed_gain_sum_ = 0.0;
    seed_loss_sum_ = 0.0;
    avg_gain_ = 0.0;
    avg_loss_ = 0.0;
}

void RsiMeanReversion::fold_change(double close) {
    if (bars_processed_ == 0) {
        // The first close anchors the change sequence.
        previous_close_ = close;
        bars_processed_ = 1;
        return;
    }

    const double change = close - previous_close_;
    const double gain = std::max(change, 0.0);
    const double loss = std::max(-change, 0.0);

    if (changes_seen_ < period_) {
        // Seed the averages with a simple average of the first `period`
        // changes.
        seed_gain_sum_ += gain;
        seed_loss_sum_ += loss;
        ++changes_seen_;

        if (changes_seen_ == period_) {
            avg_gain_ = seed_gain_sum_ / static_cast<double>(period_);
            avg_loss_ = seed_loss_sum_ / static_cast<double>(period_);
        }
    } else {
        // Wilder's recursive smoothing for every later change.
        avg_gain_ = (avg_gain_ * (static_cast<double>(period_) - 1.0) +
                     gain) / static_cast<double>(period_);
        avg_loss_ = (avg_loss_ * (static_cast<double>(period_) - 1.0) +
                     loss) / static_cast<double>(period_);
    }

    previous_close_ = close;
    ++bars_processed_;
}

bool RsiMeanReversion::has_rsi() const {
    return changes_seen_ >= period_;
}

double RsiMeanReversion::current_rsi() const {
    if (!has_rsi()) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    if (avg_loss_ == 0.0) {
        // No losses in the window: the asset is fully overbought.
        return 100.0;
    }

    if (avg_gain_ == 0.0) {
        // No gains in the window: the asset is fully oversold.
        return 0.0;
    }

    return 100.0 * avg_gain_ / (avg_gain_ + avg_loss_);
}
