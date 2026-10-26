#include "trading/portfolio.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace {

// Absolute tolerance used when comparing cash and share quantities, so
// floating-point residue from commission math does not reject valid trades.
constexpr double kTolerance = 1e-9;

bool valid_cash(double value) {
    return std::isfinite(value) && value >= 0.0;
}

bool valid_trade(double quantity, double price, double commission) {
    return std::isfinite(quantity) && quantity > 0.0 &&
           std::isfinite(price) && price > 0.0 &&
           std::isfinite(commission) && commission >= 0.0;
}

void validate_symbol(const std::string& symbol) {
    if (symbol.empty()) {
        throw std::invalid_argument("Symbol cannot be empty");
    }
}

}  // namespace

double Position::average_cost() const {
    if (quantity <= 0.0) {
        return 0.0;
    }

    return cost_basis / quantity;
}

Portfolio::Portfolio(double initial_cash) : cash_(initial_cash) {
    if (!valid_cash(initial_cash)) {
        throw std::invalid_argument(
            "Initial cash must be non-negative and finite"
        );
    }
}

double Portfolio::cash() const noexcept {
    return cash_;
}

const std::map<std::string, Position>& Portfolio::positions() const noexcept {
    return positions_;
}

bool Portfolio::owns(const std::string& symbol) const {
    const auto it = positions_.find(symbol);
    return it != positions_.end() && it->second.quantity > 0.0;
}

double Portfolio::quantity(const std::string& symbol) const {
    const auto it = positions_.find(symbol);
    return it == positions_.end() ? 0.0 : it->second.quantity;
}

const Position* Portfolio::find_position(const std::string& symbol) const {
    const auto it = positions_.find(symbol);
    return it == positions_.end() ? nullptr : &it->second;
}

void Portfolio::buy(
    const std::string& symbol,
    double quantity,
    double fill_price,
    double commission
) {
    validate_symbol(symbol);

    if (!valid_trade(quantity, fill_price, commission)) {
        throw std::invalid_argument(
            "Buy requires positive quantity and price, and a "
            "non-negative finite commission"
        );
    }

    const double total_cost = quantity * fill_price + commission;

    if (!std::isfinite(total_cost) || total_cost > cash_ + kTolerance) {
        throw std::invalid_argument("Insufficient cash for buy");
    }

    cash_ -= total_cost;

    // Clamp floating-point residue when the buy spends all available cash.
    if (cash_ < 0.0) {
        cash_ = 0.0;
    }

    Position& position = positions_[symbol];
    position.quantity += quantity;
    position.cost_basis += total_cost;
}

void Portfolio::sell(
    const std::string& symbol,
    double quantity,
    double fill_price,
    double commission
) {
    validate_symbol(symbol);

    if (!valid_trade(quantity, fill_price, commission)) {
        throw std::invalid_argument(
            "Sell requires positive quantity and price, and a "
            "non-negative finite commission"
        );
    }

    const auto it = positions_.find(symbol);

    if (it == positions_.end() ||
        quantity > it->second.quantity + kTolerance) {
        throw std::invalid_argument(
            "Cannot sell more shares than are held"
        );
    }

    Position& position = it->second;

    // Treat a sell within tolerance of the full position as a full exit.
    const bool closing_position =
        quantity >= position.quantity - kTolerance;
    const double sold_quantity =
        closing_position ? position.quantity : quantity;

    const double gross_proceeds = sold_quantity * fill_price;

    if (!std::isfinite(gross_proceeds) || commission > gross_proceeds) {
        throw std::invalid_argument(
            "Commission cannot exceed sale proceeds"
        );
    }

    // Remove cost basis at the average cost of the shares being sold.
    const double cost_removed =
        closing_position
            ? position.cost_basis
            : position.average_cost() * sold_quantity;

    cash_ += gross_proceeds - commission;

    position.quantity -= sold_quantity;
    position.cost_basis -= cost_removed;

    if (closing_position || position.quantity <= kTolerance) {
        positions_.erase(it);
    }
}

double Portfolio::market_value(
    const std::map<std::string, double>& prices
) const {
    double value = 0.0;

    for (const auto& [symbol, position] : positions_) {
        const auto price_it = prices.find(symbol);

        if (price_it == prices.end() ||
            !std::isfinite(price_it->second) ||
            price_it->second <= 0.0) {
            throw std::invalid_argument(
                "Missing or invalid price for held symbol: " + symbol
            );
        }

        value += position.quantity * price_it->second;
    }

    return value;
}

double Portfolio::equity(
    const std::map<std::string, double>& prices
) const {
    return cash_ + market_value(prices);
}
