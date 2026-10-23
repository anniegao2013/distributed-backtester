#include "execution.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace {

void validate_open_price(double open_price) {
    if (!std::isfinite(open_price) || open_price <= 0.0) {
        throw std::invalid_argument(
            "Open price must be positive and finite"
        );
    }
}

void validate_order(const Order& order) {
    if (order.symbol.empty()) {
        throw std::invalid_argument("Order symbol cannot be empty");
    }

    if (!std::isfinite(order.quantity) || order.quantity <= 0.0) {
        throw std::invalid_argument(
            "Order quantity must be positive and finite"
        );
    }
}

}  // namespace

void validate_execution_config(const ExecutionConfig& execution) {
    if (!std::isfinite(execution.transaction_cost_rate) ||
        execution.transaction_cost_rate < 0.0 ||
        execution.transaction_cost_rate >= 1.0) {
        throw std::invalid_argument(
            "Transaction cost rate must be in [0, 1)"
        );
    }

    if (!std::isfinite(execution.slippage_rate) ||
        execution.slippage_rate < 0.0 ||
        execution.slippage_rate >= 1.0) {
        throw std::invalid_argument(
            "Slippage rate must be in [0, 1)"
        );
    }
}

std::optional<Order> create_order(
    Signal signal,
    const Portfolio& portfolio,
    const std::string& symbol,
    double open_price,
    const ExecutionConfig& execution
) {
    validate_execution_config(execution);

    if (signal == Signal::Hold) {
        return std::nullopt;
    }

    validate_open_price(open_price);

    if (signal == Signal::Buy) {
        // The engine owns position sizing: invest all available cash.
        if (portfolio.owns(symbol) || portfolio.cash() <= 0.0) {
            return std::nullopt;
        }

        const double fill_price =
            open_price * (1.0 + execution.slippage_rate);
        const double cost_per_share =
            fill_price * (1.0 + execution.transaction_cost_rate);
        const double quantity = portfolio.cash() / cost_per_share;

        if (!std::isfinite(quantity) || quantity <= 0.0) {
            return std::nullopt;
        }

        return Order{symbol, OrderSide::Buy, quantity};
    }

    // Sell: close the entire position, if one exists.
    if (!portfolio.owns(symbol)) {
        return std::nullopt;
    }

    return Order{symbol, OrderSide::Sell, portfolio.quantity(symbol)};
}

Trade execute_order(
    const Order& order,
    double open_price,
    const ExecutionConfig& execution
) {
    validate_execution_config(execution);
    validate_order(order);
    validate_open_price(open_price);

    const double direction =
        order.side == OrderSide::Buy ? 1.0 : -1.0;
    const double fill_price =
        open_price * (1.0 + direction * execution.slippage_rate);
    const double notional = order.quantity * fill_price;
    const double commission =
        notional * execution.transaction_cost_rate;
    const double slippage_cost =
        order.quantity * std::abs(fill_price - open_price);

    return Trade{
        order.symbol,
        order.side,
        order.quantity,
        fill_price,
        commission,
        slippage_cost
    };
}

void apply_trade(Portfolio& portfolio, const Trade& trade) {
    if (trade.side == OrderSide::Buy) {
        portfolio.buy(
            trade.symbol,
            trade.quantity,
            trade.fill_price,
            trade.commission
        );
    } else {
        portfolio.sell(
            trade.symbol,
            trade.quantity,
            trade.fill_price,
            trade.commission
        );
    }
}
