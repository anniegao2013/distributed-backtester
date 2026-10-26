#include "trading/portfolio.h"

#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool near(double a, double b, double tolerance = 1e-9) {
    return std::abs(a - b) < tolerance;
}

void test_initial_cash() {
    const Portfolio portfolio(10000.0);

    expect(near(portfolio.cash(), 10000.0), "Incorrect initial cash");
    expect(portfolio.positions().empty(), "Portfolio should start flat");
    expect(!portfolio.owns("AAPL"), "No symbol should be owned initially");
    expect(near(portfolio.quantity("AAPL"), 0.0), "Quantity should be zero");
}

void test_buy_records_position_and_reduces_cash() {
    Portfolio portfolio(1000.0);

    portfolio.buy("AAPL", 10.0, 50.0, 1.0);

    expect(near(portfolio.cash(), 499.0), "Cash should drop by cost");
    expect(portfolio.owns("AAPL"), "AAPL should be owned");
    expect(near(portfolio.quantity("AAPL"), 10.0), "Incorrect quantity");

    const Position* position = portfolio.find_position("AAPL");
    expect(position != nullptr, "AAPL position should exist");
    expect(near(position->cost_basis, 501.0), "Incorrect cost basis");
    expect(near(position->average_cost(), 50.1), "Incorrect average cost");
}

void test_positions_are_tracked_per_symbol() {
    Portfolio portfolio(10000.0);

    portfolio.buy("AAPL", 10.0, 100.0, 1.0);
    portfolio.buy("MSFT", 5.0, 200.0, 0.0);

    expect(portfolio.positions().size() == 2, "Expected two positions");
    expect(near(portfolio.quantity("AAPL"), 10.0), "Incorrect AAPL quantity");
    expect(near(portfolio.quantity("MSFT"), 5.0), "Incorrect MSFT quantity");
    expect(near(portfolio.cash(), 7999.0), "Incorrect cash after two buys");
}

void test_average_cost_after_additional_buy() {
    Portfolio portfolio(10000.0);

    portfolio.buy("AAPL", 10.0, 100.0, 1.0);
    portfolio.buy("AAPL", 10.0, 120.0, 0.0);

    const Position* position = portfolio.find_position("AAPL");
    expect(position != nullptr, "AAPL position should exist");
    expect(near(position->quantity, 20.0), "Quantities should accumulate");
    expect(
        near(position->average_cost(), 110.05),
        "Average cost should include commission"
    );
}

void test_sell_reduces_position_and_increases_cash() {
    Portfolio portfolio(10000.0);

    portfolio.buy("AAPL", 20.0, 100.0, 0.0);
    portfolio.sell("AAPL", 5.0, 150.0, 0.5);

    expect(near(portfolio.quantity("AAPL"), 15.0), "Incorrect remaining qty");
    expect(portfolio.owns("AAPL"), "AAPL should still be owned");
    expect(near(portfolio.cash(), 8749.5), "Incorrect cash after sell");

    const Position* position = portfolio.find_position("AAPL");
    expect(position != nullptr, "AAPL position should exist");
    expect(near(position->cost_basis, 1500.0), "Incorrect remaining basis");
}

void test_sell_all_removes_position() {
    Portfolio portfolio(1000.0);

    portfolio.buy("AAPL", 10.0, 50.0, 0.0);
    portfolio.sell("AAPL", 10.0, 60.0, 1.0);

    expect(!portfolio.owns("AAPL"), "Sold-out symbol should not be owned");
    expect(portfolio.positions().empty(), "Positions should be empty");
    expect(near(portfolio.cash(), 1099.0), "Incorrect cash after full exit");
}

void test_equity_is_cash_plus_market_value() {
    Portfolio portfolio(10000.0);

    portfolio.buy("AAPL", 10.0, 100.0, 1.0);
    portfolio.buy("MSFT", 5.0, 200.0, 0.0);

    // Cash: 10000 - 1001 - 1000 = 7999.
    // Market value: 10 * 160 + 5 * 210 = 2650.
    const std::map<std::string, double> prices{
        {"AAPL", 160.0},
        {"MSFT", 210.0}
    };

    expect(
        near(portfolio.market_value(prices), 2650.0),
        "Incorrect market value"
    );
    expect(
        near(portfolio.equity(prices), 10649.0),
        "Incorrect portfolio equity"
    );
}

void test_equity_with_only_cash() {
    const Portfolio portfolio(500.0);

    expect(
        near(portfolio.equity({}), 500.0),
        "Flat portfolio equity should equal cash"
    );
}

void test_equity_requires_prices_for_held_symbols() {
    Portfolio portfolio(10000.0);
    portfolio.buy("AAPL", 10.0, 100.0);

    bool threw = false;

    try {
        (void)portfolio.equity({{"MSFT", 200.0}});
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    expect(threw, "Missing price for a held symbol should throw");
}

void test_invalid_trades_throw() {
    Portfolio portfolio(100.0);

    auto expect_throws = [](auto&& operation) {
        bool threw = false;

        try {
            operation();
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        expect(threw, "Invalid portfolio operation should throw");
    };

    // Insufficient cash.
    expect_throws([&portfolio] {
        portfolio.buy("AAPL", 10.0, 50.0);
    });

    // Invalid quantities and prices.
    expect_throws([&portfolio] {
        portfolio.buy("AAPL", 0.0, 10.0);
    });
    expect_throws([&portfolio] {
        portfolio.buy("AAPL", 1.0, 0.0);
    });
    expect_throws([&portfolio] {
        portfolio.buy("", 1.0, 10.0);
    });
    expect_throws([&portfolio] {
        portfolio.buy("AAPL", 1.0, 10.0, -1.0);
    });

    // Selling more than held.
    portfolio.buy("AAPL", 1.0, 10.0);
    expect_throws([&portfolio] {
        portfolio.sell("AAPL", 2.0, 10.0);
    });

    // Selling a symbol that is not owned.
    expect_throws([&portfolio] {
        portfolio.sell("MSFT", 1.0, 10.0);
    });

    // Commission larger than proceeds.
    expect_throws([&portfolio] {
        portfolio.sell("AAPL", 1.0, 10.0, 10.01);
    });
}

void test_invalid_initial_cash_throws() {
    auto expect_throws = [](double cash) {
        bool threw = false;

        try {
            Portfolio portfolio(cash);
            (void)portfolio;
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        expect(threw, "Invalid initial cash should throw");
    };

    expect_throws(-1.0);
}

}  // namespace

int main() {
    try {
        test_initial_cash();
        test_buy_records_position_and_reduces_cash();
        test_positions_are_tracked_per_symbol();
        test_average_cost_after_additional_buy();
        test_sell_reduces_position_and_increases_cash();
        test_sell_all_removes_position();
        test_equity_is_cash_plus_market_value();
        test_equity_with_only_cash();
        test_equity_requires_prices_for_held_symbols();
        test_invalid_trades_throw();
        test_invalid_initial_cash_throws();

        std::cout << "All portfolio tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
