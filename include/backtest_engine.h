#pragma once

#include "market_data.h"
#include "strategy.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

struct BacktestConfig {
    std::string job_id;
    std::string asset;
    std::string start_date;  // Inclusive: YYYY-MM-DD
    std::string end_date;    // Inclusive: YYYY-MM-DD

    double initial_cash = 10000.0;
    std::map<std::string, double> parameters;
};

struct BacktestMetrics {
    double total_return = 0.0;
    double sharpe_ratio = 0.0;
    double max_drawdown = 0.0;
    std::size_t number_of_trades = 0;
};

struct JobResult {
    std::string job_id;
    std::string asset;
    std::map<std::string, double> parameters;
    BacktestMetrics metrics;
};

JobResult run_backtest(
    const std::vector<MarketBar>& dataset,
    Strategy& strategy,
    const BacktestConfig& config
);