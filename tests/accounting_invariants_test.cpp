#include "backtest_engine.h"
#include "execution.h"
#include "portfolio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Relative tolerance so comparisons hold across cash magnitudes.
bool approx(double a, double b, double tolerance = 1e-9) {
    const double scale = std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= tolerance * scale;
}

using PositionSnapshot =
    std::map<std::string, std::pair<double, double>>;

PositionSnapshot snapshot(const Portfolio& portfolio) {
    PositionSnapshot result;

    for (const auto& [symbol, position] : portfolio.positions()) {
        result[symbol] = {position.quantity, position.cost_basis};
    }

    return result;
}

bool same_snapshot(
    const PositionSnapshot& a,
    const PositionSnapshot& b
) {
    if (a.size() != b.size()) {
        return false;
    }

    auto a_it = a.begin();
    auto b_it = b.begin();

    for (; a_it != a.end(); ++a_it, ++b_it) {
        if (a_it->first != b_it->first ||
            !approx(a_it->second.first, b_it->second.first) ||
            !approx(a_it->second.second, b_it->second.second)) {
            return false;
        }
    }

    return true;
}

// A rejected trade must leave cash and every position untouched.
template <typename Operation>
void expect_rejected_without_mutation(
    Portfolio& portfolio,
    Operation&& operation,
    const std::string& message
) {
    const double cash_before = portfolio.cash();
    const PositionSnapshot positions_before = snapshot(portfolio);

    bool threw = false;

    try {
        operation();
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    expect(threw, message);
    expect(
        approx(portfolio.cash(), cash_before),
        "Rejected trade must not change cash"
    );
    expect(
        same_snapshot(snapshot(portfolio), positions_before),
        "Rejected trade must not change positions"
    );
}

// Drives randomized buys and sells while checking that cash, positions, and
// portfolio value always reconcile.
void test_portfolio_invariants_under_random_trades() {
    constexpr double kInitialCash = 100000.0;
    constexpr double kTransactionCost = 0.002;
    constexpr double kSlippage = 0.001;
    constexpr int kSteps = 500;

    Portfolio portfolio(kInitialCash);

    std::mt19937 rng(20240517u);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_real_distribution<double> price_dist(20.0, 500.0);

    const std::vector<std::string> symbols{"AAPL", "MSFT", "GOOG"};

    std::map<std::string, double> expected_quantity;
    std::map<std::string, double> expected_basis;
    std::map<std::string, double> last_price;
    double expected_cash = kInitialCash;
    double total_buy_outflow = 0.0;
    double total_sell_inflow = 0.0;

    for (int step = 0; step < kSteps; ++step) {
        const std::string& symbol =
            symbols[static_cast<std::size_t>(step) % symbols.size()];
        const double price = price_dist(rng);
        last_price[symbol] = price;

        const bool can_buy = expected_cash > 100.0;
        const bool choose_buy =
            can_buy && (unit(rng) < 0.55 ||
                        expected_quantity[symbol] <= 1e-9);

        if (choose_buy) {
            const double fill_price = price * (1.0 + kSlippage);
            const double budget =
                expected_cash * (0.05 + 0.90 * unit(rng));
            const double quantity =
                budget / (fill_price * (1.0 + kTransactionCost));
            const double commission =
                quantity * fill_price * kTransactionCost;
            const double total_cost =
                quantity * fill_price + commission;

            expect(
                quantity > 0.0 && total_cost <= expected_cash + 1e-6,
                "Random buy should fit within available cash"
            );

            // Leverage is unsupported: spending more than cash must fail
            // without changing any accounting state.
            const double unaffordable_quantity =
                (expected_cash / fill_price) * 2.0 + 1.0;
            expect_rejected_without_mutation(
                portfolio,
                [&portfolio, &symbol, fill_price, unaffordable_quantity] {
                    portfolio.buy(
                        symbol,
                        unaffordable_quantity,
                        fill_price
                    );
                },
                "Buy exceeding cash should be rejected"
            );

            portfolio.buy(symbol, quantity, fill_price, commission);

            expected_cash -= total_cost;
            expected_quantity[symbol] += quantity;
            expected_basis[symbol] += total_cost;
            total_buy_outflow += total_cost;
        } else if (expected_quantity[symbol] > 1e-9) {
            const double fill_price = price * (1.0 - kSlippage);

            double quantity =
                expected_quantity[symbol] * (0.2 + 0.6 * unit(rng));
            if (expected_quantity[symbol] - quantity < 1e-6) {
                quantity = expected_quantity[symbol];
            }

            const double gross = quantity * fill_price;
            const double commission = gross * kTransactionCost;
            const double proceeds = gross - commission;

            const double unaffordable_quantity =
                expected_quantity[symbol] + 1.0;
            expect_rejected_without_mutation(
                portfolio,
                [&portfolio, &symbol, fill_price, unaffordable_quantity] {
                    portfolio.sell(
                        symbol,
                        unaffordable_quantity,
                        fill_price
                    );
                },
                "Sell exceeding the held quantity should be rejected"
            );

            const double quantity_before = expected_quantity[symbol];

            portfolio.sell(symbol, quantity, fill_price, commission);

            expected_cash += proceeds;

            if (quantity >= quantity_before - 1e-9) {
                expected_basis[symbol] = 0.0;
            } else {
                expected_basis[symbol] -=
                    (expected_basis[symbol] / quantity_before) * quantity;
            }

            expected_quantity[symbol] -= quantity;
            total_sell_inflow += proceeds;
        }

        // Cash is never negative: leverage is not implicitly available.
        expect(portfolio.cash() >= 0.0, "Cash must never be negative");

        // Cash reconciles with the cumulative cash flows.
        expect(
            approx(portfolio.cash(), expected_cash),
            "Cash does not reconcile with trade cash flows"
        );
        expect(
            approx(
                portfolio.cash(),
                kInitialCash - total_buy_outflow + total_sell_inflow
            ),
            "Cash does not reconcile with total inflows and outflows"
        );

        // The positions map contains exactly the symbols expected to be
        // owned, with matching quantities.
        std::size_t expected_position_count = 0;
        double expected_market_value = 0.0;

        for (const std::string& asset : symbols) {
            const double expected = expected_quantity[asset];

            expect(
                approx(portfolio.quantity(asset), expected),
                "Position quantity does not reconcile"
            );

            if (expected > 1e-9) {
                expect(
                    portfolio.owns(asset),
                    "Owned symbol missing from positions"
                );

                const Position* position =
                    portfolio.find_position(asset);
                expect(
                    position != nullptr,
                    "Owned symbol has no Position entry"
                );
                expect(
                    approx(position->cost_basis, expected_basis[asset]),
                    "Position cost basis does not reconcile"
                );

                ++expected_position_count;
                expected_market_value += expected * last_price[asset];
            } else {
                expect(
                    !portfolio.owns(asset),
                    "Fully sold symbol should not be owned"
                );
                expect(
                    approx(expected_basis[asset], 0.0),
                    "Fully sold symbol should have zero cost basis"
                );
            }
        }

        expect(
            portfolio.positions().size() == expected_position_count,
            "Positions map contains unexpected symbols"
        );

        // Portfolio value is exactly cash plus marked positions.
        expect(
            approx(
                portfolio.market_value(last_price),
                expected_market_value
            ),
            "Market value does not reconcile with positions"
        );
        expect(
            approx(
                portfolio.equity(last_price),
                portfolio.cash() + expected_market_value
            ),
            "Equity does not equal cash plus position value"
        );

        // Cost basis stays non-negative and average cost is well defined.
        for (const auto& [asset, position] : portfolio.positions()) {
            (void)asset;
            expect(
                position.quantity > 0.0,
                "Positions must have positive quantity"
            );
            expect(
                position.cost_basis >= -1e-9,
                "Cost basis must not be negative"
            );
            expect(
                position.average_cost() > 0.0,
                "Average cost must be positive for an open position"
            );
        }
    }
}

// Buys must always be funded by available cash; commissions count against
// that cash. No margin/leverage path exists in the portfolio API.
void test_cash_cannot_be_overspent() {
    Portfolio portfolio(1000.0);

    expect_rejected_without_mutation(
        portfolio,
        [&portfolio] {
            portfolio.buy("AAPL", 10.0000001, 100.0);
        },
        "Buy slightly exceeding cash should be rejected"
    );

    expect_rejected_without_mutation(
        portfolio,
        [&portfolio] {
            portfolio.buy("AAPL", 10.0, 100.0, 0.01);
        },
        "Commission pushing a buy over cash should be rejected"
    );

    // Spending exactly all cash is allowed.
    portfolio.buy("AAPL", 10.0, 100.0);

    expect(approx(portfolio.cash(), 0.0), "All-cash buy should leave zero");
    expect(approx(portfolio.quantity("AAPL"), 10.0), "Position not recorded");

    expect_rejected_without_mutation(
        portfolio,
        [&portfolio] {
            portfolio.buy("AAPL", 0.0001, 100.0);
        },
        "Buy with zero cash should be rejected"
    );
}

class InvariantStrategy final : public Strategy {
public:
    Signal on_bar(
        const std::vector<MarketBar>& history,
        bool has_position
    ) override {
        const std::size_t n = history.size();

        if (!has_position && n % 4 == 1) {
            return Signal::Buy;
        }

        if (has_position && n % 7 == 0) {
            return Signal::Sell;
        }

        return Signal::Hold;
    }
};

std::string make_date(int index) {
    const int month = 1 + index / 28;
    const int day = 1 + index % 28;

    char buffer[11];
    std::snprintf(buffer, sizeof(buffer), "2020-%02d-%02d", month, day);
    return buffer;
}

// Replays every executed trade and checks that the engine's reported return
// matches independently reconstructed cash and positions. The replay also
// proves each buy was funded by cash on hand.
void test_engine_trades_reconcile_with_final_equity() {
    constexpr double kInitialCash = 50000.0;

    std::vector<MarketBar> data;
    data.reserve(120);

    for (int i = 0; i < 120; ++i) {
        const double price =
            100.0 + 15.0 * std::sin(static_cast<double>(i) * 0.35) +
            0.05 * static_cast<double>(i);

        data.push_back(
            MarketBar{
                make_date(i),
                price, price, price, price, price, 1000
            }
        );
    }

    BacktestConfig config{
        "invariant-replay",
        "TEST",
        data.front().date,
        data.back().date,
        kInitialCash,
        {}
    };
    config.transaction_cost_rate = 0.001;
    config.slippage_rate = 0.0005;

    InvariantStrategy strategy;
    const JobResult result = run_backtest(data, strategy, config);

    expect(!result.trades.empty(), "Expected executable trades");

    Portfolio replay(kInitialCash);
    std::size_t buy_fills = 0;
    std::size_t sell_fills = 0;
    double total_costs = 0.0;

    for (const Trade& trade : result.trades) {
        expect(trade.symbol == config.asset, "Trade symbol mismatch");
        expect(trade.quantity > 0.0, "Trade quantity must be positive");
        expect(trade.fill_price > 0.0, "Fill price must be positive");
        expect(trade.commission >= 0.0, "Commission must be non-negative");
        expect(
            trade.slippage_cost >= 0.0,
            "Slippage cost must be non-negative"
        );

        const double cash_before = replay.cash();

        if (trade.side == OrderSide::Buy) {
            const double cash_required =
                trade.quantity * trade.fill_price + trade.commission;

            expect(
                cash_before + 1e-6 >= cash_required,
                "Trade spends more cash than was available"
            );
            ++buy_fills;
        } else {
            ++sell_fills;
        }

        // Throws if a trade violates holdings or cash constraints.
        apply_trade(replay, trade);

        expect(replay.cash() >= 0.0, "Replay cash must never be negative");
        total_costs += trade.commission + trade.slippage_cost;
    }

    expect(buy_fills > 0, "Expected at least one buy fill");
    expect(sell_fills > 0, "Expected at least one sell fill");
    expect(
        result.metrics.number_of_trades == sell_fills,
        "Round-trip count should match sell fills"
    );
    expect(
        approx(result.metrics.total_costs, total_costs),
        "Reported costs should match the sum of trade costs"
    );

    const double final_price = data.back().close;
    const double final_equity =
        replay.cash() + replay.quantity(config.asset) * final_price;
    const double reported_equity =
        kInitialCash * (1.0 + result.metrics.total_return);

    expect(
        approx(
            replay.equity({{config.asset, final_price}}),
            final_equity
        ),
        "Equity should equal cash plus marked positions"
    );
    expect(
        approx(final_equity, reported_equity),
        "Replayed portfolio value does not match reported return"
    );
}

}  // namespace

int main() {
    try {
        test_portfolio_invariants_under_random_trades();
        test_cash_cannot_be_overspent();
        test_engine_trades_reconcile_with_final_equity();

        std::cout << "All accounting invariant tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
