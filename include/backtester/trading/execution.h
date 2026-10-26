#pragma once

#include "trading/portfolio.h"
#include "trading/strategy.h"

#include <optional>
#include <string>

enum class OrderSide {
    Buy,
    Sell
};

// An explicit instruction to trade a quantity of one symbol. The engine
// creates orders from strategy signals; strategies never create orders or
// choose execution details themselves.
struct Order {
    std::string symbol;
    OrderSide side = OrderSide::Buy;
    double quantity = 0.0;
};

// How the engine fills orders. These settings belong to the engine, not to
// the strategy.
struct ExecutionConfig {
    // Commission charged on traded notional (0.001 = 10 basis points).
    double transaction_cost_rate = 0.0;

    // Adverse price movement applied to each fill (0.0005 = 5 basis points).
    double slippage_rate = 0.0;
};

// A filled order: the concrete price, commission, and slippage the engine
// realized. Applying a trade to a portfolio is a separate step.
struct Trade {
    std::string symbol;
    OrderSide side = OrderSide::Buy;
    double quantity = 0.0;
    double fill_price = 0.0;
    double commission = 0.0;
    double slippage_cost = 0.0;
};

void validate_execution_config(const ExecutionConfig& execution);

// Converts a strategy signal into a concrete order for the next bar's open.
// The engine sizes the order against the current portfolio: buys invest all
// available cash, sells close the entire position. Returns std::nullopt when
// the signal implies no actionable order (hold, already long, flat on a
// sell, or no cash to invest).
std::optional<Order> create_order(
    Signal signal,
    const Portfolio& portfolio,
    const std::string& symbol,
    double open_price,
    const ExecutionConfig& execution
);

// Fills an order at the given bar open, applying slippage and commission.
Trade execute_order(
    const Order& order,
    double open_price,
    const ExecutionConfig& execution
);

// Applies an executed trade to the portfolio's cash and positions.
void apply_trade(Portfolio& portfolio, const Trade& trade);
