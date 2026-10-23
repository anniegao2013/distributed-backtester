#pragma once

#include <map>
#include <string>

// A single holding in the portfolio, keyed by symbol in Portfolio.
struct Position {
    double quantity = 0.0;

    // Total cash paid for the remaining shares, including commissions.
    double cost_basis = 0.0;

    double average_cost() const;
};

// Tracks cash, per-symbol positions, and mark-to-market equity.
//
// Buys and sells update both cash and the position identified by symbol, so
// the portfolio always knows which assets are currently owned.
class Portfolio {
public:
    explicit Portfolio(double initial_cash = 0.0);

    double cash() const noexcept;

    // All currently owned positions, including those with partial size.
    const std::map<std::string, Position>& positions() const noexcept;

    bool owns(const std::string& symbol) const;

    // Shares held for a symbol, or 0 when the symbol is not owned.
    double quantity(const std::string& symbol) const;

    // Null when the symbol is not owned.
    const Position* find_position(const std::string& symbol) const;

    // Buys quantity shares at fill_price, paying an additional commission.
    // Throws std::invalid_argument for invalid inputs or insufficient cash.
    void buy(
        const std::string& symbol,
        double quantity,
        double fill_price,
        double commission = 0.0
    );

    // Sells quantity shares at fill_price, deducting commission from the
    // proceeds. Throws std::invalid_argument for invalid inputs, selling
    // more than is held, or a commission larger than the proceeds.
    void sell(
        const std::string& symbol,
        double quantity,
        double fill_price,
        double commission = 0.0
    );

    // Sum of quantity * price for every owned position. Throws
    // std::invalid_argument if an owned symbol has no valid price.
    double market_value(
        const std::map<std::string, double>& prices
    ) const;

    // Cash plus the market value of all positions.
    double equity(
        const std::map<std::string, double>& prices
    ) const;

private:
    double cash_;
    std::map<std::string, Position> positions_;
};
