#include "backtest_engine.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {

constexpr double kAnnualTradingDays = 252.0;

double calculate_sharpe_ratio(
    const std::vector<double>& equity_curve,
    double initial_cash
) {
    if (equity_curve.size() < 2) {
        return 0.0;
    }

    std::vector<double> daily_returns;
    daily_returns.reserve(equity_curve.size());

    double previous_equity = initial_cash;

    for (const double equity : equity_curve) {
        daily_returns.push_back(
            equity / previous_equity - 1.0
        );
        previous_equity = equity;
    }

    double mean = 0.0;
    for (const double daily_return : daily_returns) {
        mean += daily_return;
    }
    mean /= static_cast<double>(daily_returns.size());

    // Sample standard deviation.
    double squared_deviations = 0.0;
    for (const double daily_return : daily_returns) {
        const double difference = daily_return - mean;
        squared_deviations += difference * difference;
    }

    if (daily_returns.size() < 2) {
        return 0.0;
    }

    const double variance = squared_deviations /
        static_cast<double>(daily_returns.size() - 1);
    const double standard_deviation = std::sqrt(variance);

    // A zero-volatility series has no meaningful Sharpe ratio.
    if (standard_deviation == 0.0) {
        return 0.0;
    }

    return (mean / standard_deviation) *
        std::sqrt(kAnnualTradingDays);
}

double calculate_max_drawdown(
    const std::vector<double>& equity_curve,
    double initial_cash
) {
    double peak = initial_cash;
    double max_drawdown = 0.0;

    for (const double equity : equity_curve) {
        peak = std::max(peak, equity);

        if (peak > 0.0) {
            const double drawdown = (peak - equity) / peak;
            max_drawdown = std::max(max_drawdown, drawdown);
        }
    }

    return max_drawdown;
}

}  // namespace

JobResult run_backtest(
    const std::vector<MarketBar>& dataset,
    Strategy& strategy,
    const BacktestConfig& config
) {
    if (config.job_id.empty()) {
        throw std::invalid_argument("Job ID cannot be empty");
    }

    if (config.asset.empty()) {
        throw std::invalid_argument("Asset cannot be empty");
    }

    if (config.start_date.empty() ||
        config.end_date.empty() ||
        config.start_date > config.end_date) {
        throw std::invalid_argument("Invalid backtest date range");
    }

    if (!std::isfinite(config.initial_cash) ||
        config.initial_cash <= 0.0) {
        throw std::invalid_argument(
            "Initial cash must be positive and finite"
        );
    }

    // Select bars within the inclusive date range.
    std::vector<MarketBar> bars;
    for (const MarketBar& bar : dataset) {
        if (bar.date >= config.start_date &&
            bar.date <= config.end_date) {
            bars.push_back(bar);
        }
    }

    if (bars.empty()) {
        throw std::invalid_argument(
            "No market data found in the requested date range"
        );
    }

    double cash = config.initial_cash;
    double shares = 0.0;
    std::size_t completed_trades = 0;

    // Signals generated after a close execute at the next bar's open.
    Signal pending_signal = Signal::Hold;

    // Contains only bars in the requested backtest range.
    std::vector<MarketBar> history;
    history.reserve(bars.size());

    // Portfolio value at each bar's close.
    std::vector<double> equity_curve;
    equity_curve.reserve(bars.size());

    for (std::size_t i = 0; i < bars.size(); ++i) {
        const MarketBar& bar = bars[i];

        // 1. Execute yesterday's signal at today's open.
        if (pending_signal == Signal::Buy && shares == 0.0) {
            shares = cash / bar.open;
            cash = 0.0;
        } else if (
            pending_signal == Signal::Sell && shares > 0.0
        ) {
            cash += shares * bar.open;
            shares = 0.0;
            ++completed_trades;
        }

        // 2. Add today's completed bar to the available history.
        history.push_back(bar);

        // 3. Mark the portfolio to market at today's close.
        const double equity = cash + shares * bar.close;
        equity_curve.push_back(equity);

        // 4. Generate a signal using no future data.
        // That signal cannot execute until the next bar's open.
        pending_signal = strategy.on_bar(history, shares > 0.0);
    }

    const double final_equity = equity_curve.back();

    JobResult result;
    result.job_id = config.job_id;
    result.asset = config.asset;
    result.parameters = config.parameters;

    result.metrics.total_return =
        final_equity / config.initial_cash - 1.0;

    result.metrics.sharpe_ratio =
        calculate_sharpe_ratio(equity_curve, config.initial_cash);

    result.metrics.max_drawdown =
        calculate_max_drawdown(equity_curve, config.initial_cash);

    result.metrics.number_of_trades = completed_trades;

    return result;
}