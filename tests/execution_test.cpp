#include "execution.h"
#include "portfolio.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool near(double a, double b, double tolerance = 1e-9) {
    return std::abs(a - b) < tolerance;
}

void test_hold_signal_creates_no_order() {
    const Portfolio portfolio(1000.0);
    const ExecutionConfig execution;

    const auto order = create_order(
        Signal::Hold,
        portfolio,
        "AAPL",
        10.0,
        execution
    );

    expect(!order.has_value(), "Hold should not create an order");
}

void test_buy_signal_creates_sized_order() {
    const Portfolio portfolio(1000.0);

    ExecutionConfig execution;
    execution.transaction_cost_rate = 0.01;
    execution.slippage_rate = 0.01;

    const auto order = create_order(
        Signal::Buy,
        portfolio,
        "AAPL",
        10.0,
        execution
    );

    expect(order.has_value(), "Buy should create an order");
    expect(order->symbol == "AAPL", "Order should target the asset");
    expect(order->side == OrderSide::Buy, "Order side should be Buy");

    const double fill_price = 10.0 * 1.01;
    const double expected_quantity = 1000.0 / (fill_price * 1.01);

    expect(
        near(order->quantity, expected_quantity),
        "Buy order should invest all available cash"
    );
}

void test_buy_signal_ignored_when_already_long() {
    Portfolio portfolio(1000.0);
    portfolio.buy("AAPL", 10.0, 10.0);

    const auto order = create_order(
        Signal::Buy,
        portfolio,
        "AAPL",
        10.0,
        ExecutionConfig{}
    );

    expect(!order.has_value(), "Already-long buy should be ignored");
}

void test_sell_signal_requires_position() {
    const Portfolio portfolio(1000.0);

    const auto order = create_order(
        Signal::Sell,
        portfolio,
        "AAPL",
        10.0,
        ExecutionConfig{}
    );

    expect(!order.has_value(), "Sell without a position should be ignored");
}

void test_sell_signal_closes_full_position() {
    Portfolio portfolio(1000.0);
    portfolio.buy("AAPL", 10.0, 10.0);

    const auto order = create_order(
        Signal::Sell,
        portfolio,
        "AAPL",
        10.0,
        ExecutionConfig{}
    );

    expect(order.has_value(), "Sell should create an order");
    expect(order->side == OrderSide::Sell, "Order side should be Sell");
    expect(
        near(order->quantity, 10.0),
        "Sell order should close the entire position"
    );
}

void test_execute_buy_applies_adverse_slippage_and_commission() {
    const Order order{"AAPL", OrderSide::Buy, 10.0};

    ExecutionConfig execution;
    execution.transaction_cost_rate = 0.01;
    execution.slippage_rate = 0.01;

    const Trade trade = execute_order(order, 10.0, execution);

    const double expected_fill = 10.1;

    expect(trade.symbol == "AAPL", "Trade should keep the symbol");
    expect(trade.side == OrderSide::Buy, "Trade side should be Buy");
    expect(near(trade.quantity, 10.0), "Trade quantity mismatch");
    expect(near(trade.fill_price, expected_fill), "Buy fill should be higher");
    expect(
        near(trade.commission, 10.0 * expected_fill * 0.01),
        "Buy commission mismatch"
    );
    expect(
        near(trade.slippage_cost, 10.0 * 0.1),
        "Buy slippage cost mismatch"
    );
}

void test_execute_sell_applies_adverse_slippage_and_commission() {
    const Order order{"AAPL", OrderSide::Sell, 10.0};

    ExecutionConfig execution;
    execution.transaction_cost_rate = 0.01;
    execution.slippage_rate = 0.01;

    const Trade trade = execute_order(order, 10.0, execution);

    const double expected_fill = 9.9;

    expect(near(trade.fill_price, expected_fill), "Sell fill should be lower");
    expect(
        near(trade.commission, 10.0 * expected_fill * 0.01),
        "Sell commission mismatch"
    );
    expect(
        near(trade.slippage_cost, 10.0 * 0.1),
        "Sell slippage cost mismatch"
    );
}

void test_apply_buy_and_sell_trades_updates_portfolio() {
    Portfolio portfolio(1000.0);

    const Order buy_order{"AAPL", OrderSide::Buy, 10.0};
    const Trade buy_trade =
        execute_order(buy_order, 10.0, ExecutionConfig{});
    apply_trade(portfolio, buy_trade);

    expect(portfolio.owns("AAPL"), "Buy trade should create a position");
    expect(near(portfolio.quantity("AAPL"), 10.0), "Wrong quantity after buy");
    expect(near(portfolio.cash(), 900.0), "Wrong cash after buy");

    const Order sell_order{"AAPL", OrderSide::Sell, 10.0};
    const Trade sell_trade =
        execute_order(sell_order, 12.0, ExecutionConfig{});
    apply_trade(portfolio, sell_trade);

    expect(!portfolio.owns("AAPL"), "Sell trade should close the position");
    expect(near(portfolio.cash(), 1020.0), "Wrong cash after sell");
}

void test_invalid_execution_config_throws() {
    const Order order{"AAPL", OrderSide::Buy, 1.0};

    auto expect_throws = [&order](double transaction_cost, double slippage) {
        ExecutionConfig execution;
        execution.transaction_cost_rate = transaction_cost;
        execution.slippage_rate = slippage;

        bool threw = false;

        try {
            (void)execute_order(order, 10.0, execution);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        expect(threw, "Invalid execution config should throw");
    };

    expect_throws(-0.01, 0.0);
    expect_throws(0.0, -0.01);
    expect_throws(1.0, 0.0);
    expect_throws(0.0, 1.0);
}

void test_invalid_orders_throw() {
    auto expect_throws = [](const Order& order, double open_price) {
        bool threw = false;

        try {
            (void)execute_order(order, open_price, ExecutionConfig{});
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        expect(threw, "Invalid order should throw");
    };

    expect_throws(Order{"AAPL", OrderSide::Buy, 0.0}, 10.0);
    expect_throws(Order{"AAPL", OrderSide::Buy, -1.0}, 10.0);
    expect_throws(Order{"", OrderSide::Buy, 1.0}, 10.0);
    expect_throws(Order{"AAPL", OrderSide::Buy, 1.0}, 0.0);
}

}  // namespace

int main() {
    try {
        test_hold_signal_creates_no_order();
        test_buy_signal_creates_sized_order();
        test_buy_signal_ignored_when_already_long();
        test_sell_signal_requires_position();
        test_sell_signal_closes_full_position();
        test_execute_buy_applies_adverse_slippage_and_commission();
        test_execute_sell_applies_adverse_slippage_and_commission();
        test_apply_buy_and_sell_trades_updates_portfolio();
        test_invalid_execution_config_throws();
        test_invalid_orders_throw();

        std::cout << "All execution tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
