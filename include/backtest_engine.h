#pragma once

#include "market_data.h"
#include "strategy.h"
#include "parameter_types.h"

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
    double commission_rate = 0.0005;
    double slippage_rate = 0.001;
};

struct BacktestMetrics {
    double total_return = 0.0;
    double sharpe_ratio = 0.0;
    double max_drawdown = 0.0;
    std::size_t number_of_trades = 0;
    double total_commission = 0.0;
    double total_slippage = 0.0;
};

struct JobResult {
    std::string job_id;
    std::string asset;
    ParameterMap parameters;
    BacktestMetrics metrics;
};

JobResult run_backtest(
    const std::vector<MarketBar>& dataset,
    Strategy& strategy,
    const BacktestConfig& config
);