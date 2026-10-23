#pragma once

#include "market_data.h"
#include "strategy.h"
#include "parameter_types.h"
#include "execution.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

struct BacktestConfig {
    std::string job_id;
    std::string asset;
    std::string start_date;
    std::string end_date;
    double initial_cash = 10000.0;
    ParameterMap parameters;

    // Commission charged on each executed order, as a fraction of the
    // traded notional (0.001 = 10 basis points).
    double transaction_cost_rate = 0.0;

    // Adverse price movement applied to each fill, as a fraction of the
    // bar's open price (0.0005 = 5 basis points). Buys fill above the open
    // and sells fill below it.
    double slippage_rate = 0.0;
};

struct BacktestMetrics {
    double total_return = 0.0;
    double sharpe_ratio = 0.0;
    double max_drawdown = 0.0;

    // Completed round trips (a buy followed by a full exit).
    std::size_t number_of_trades = 0;

    // Total commission plus slippage incurred, in cash terms.
    double total_costs = 0.0;
};

struct JobResult {
    std::string job_id;
    std::string asset;
    ParameterMap parameters;
    BacktestMetrics metrics;

    // Every order the engine actually filled, in execution order.
    std::vector<Trade> trades;
};

JobResult run_backtest(
    const std::vector<MarketBar>& dataset,
    Strategy& strategy,
    const BacktestConfig& config
);