#pragma once

#include "market_data.h"

#include <vector>

enum class Signal {
    Hold,
    Buy,
    Sell
};

class Strategy {
public:
    virtual ~Strategy() = default;

    // Called once per completed market bar.
    // The strategy must not access future market data.
    virtual Signal on_bar(
        const std::vector<MarketBar>& history,
        bool has_position
    ) = 0;
};