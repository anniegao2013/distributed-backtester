#include "backtest_engine.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

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

    if (!std::isfinite(config.commission_rate) ||
        config.commission_rate < 0.0) {
        throw std::invalid_argument(
            "Commission rate must be finite and nonnegative"
        );
    }

    if (!std::isfinite(config.slippage_rate) ||
        config.slippage_rate < 0.0 ||
        config.slippage_rate >= 1.0) {
        throw std::invalid_argument(
            "Slippage rate must be finite and in [0, 1)"
        );
    }

    // Keep all bars up to the end date, including pre-range warm-up bars.
    // This relies on the dataset being sorted by date.
    std::vector<MarketBar> bars;
    for (const MarketBar& bar : dataset) {
        if (bar.date <= config.end_date) {
            bars.push_back(bar);
        }
    }

    // Find the first bar that belongs to the actual backtest period.
    const auto start_it = std::lower_bound(
        bars.begin(),
        bars.end(),
        config.start_date,
        [](const MarketBar& bar, const std::string& date) {
            return bar.date < date;
        }
    );

    const std::size_t start_index =
        static_cast<std::size_t>(start_it - bars.begin());

    if (start_index == bars.size()) {
        throw std::invalid_argument(
            "No market data found in the requested date range"
        );
    }

    // Verify that at least one bar falls within the requested range.
    // Since bars are sorted and clipped at end_date, this also checks
    // that the first bar on/after start_date is not after end_date.
    if (bars[start_index].date > config.end_date) {
        throw std::invalid_argument(
            "No market data found in the requested date range"
        );
    }

    double cash = config.initial_cash;
    double shares = 0.0;
    std::size_t completed_trades = 0;
    double total_commission = 0.0;
    double total_slippage = 0.0;

    Signal pending_signal = Signal::Hold;

    std::vector<MarketBar> history;
    history.reserve(bars.size());

    std::vector<double> equity_curve;
    equity_curve.reserve(bars.size() - start_index);

    // Warm-up: give the strategy historical context, but ignore signals.
    // No positions, trades, or performance observations are created here.
    for (std::size_t i = 0; i < start_index; ++i) {
        history.push_back(bars[i]);
        (void)strategy.on_bar(history, false);
    }

    // Actual backtest: portfolio accounting starts at start_index.
    for (std::size_t i = start_index; i < bars.size(); ++i) {
        const MarketBar& bar = bars[i];

        // Execute the previous in-range bar's signal at today's open.
        if (pending_signal == Signal::Buy && shares == 0.0) {
            const double execution_price =
                bar.open * (1.0 + config.slippage_rate);
            const double cost_per_share =
                execution_price * (1.0 + config.commission_rate);

            // Size the position so purchase cost plus commission fits cash.
            shares = cash / cost_per_share;
            const double traded_notional = shares * execution_price;
            const double commission =
                traded_notional * config.commission_rate;
            total_commission += commission;
            total_slippage += shares * (execution_price - bar.open);
            cash -= traded_notional + commission;

            // Avoid tiny negative balances caused by floating-point rounding.
            if (cash < 0.0 && cash > -1e-9 * config.initial_cash) {
                cash = 0.0;
            }
        } else if (
            pending_signal == Signal::Sell && shares > 0.0
        ) {
            const double execution_price =
                bar.open * (1.0 - config.slippage_rate);
            const double traded_notional = shares * execution_price;
            const double commission =
                traded_notional * config.commission_rate;
            cash += traded_notional - commission;
            total_commission += commission;
            total_slippage += shares * (bar.open - execution_price);
            shares = 0.0;
            ++completed_trades;
        }

        // Add today's bar. The strategy sees warm-up bars plus
        // all completed bars through today's close.
        history.push_back(bar);

        // Record equity only for bars inside the requested range.
        const double equity = cash + shares * bar.close;
        equity_curve.push_back(equity);

        // Today's signal can execute only at the next bar's open.
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
    result.metrics.total_commission = total_commission;
    result.metrics.total_slippage = total_slippage;

    return result;
}