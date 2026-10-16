#pragma once

#include "strategy.h"

#include <cstddef>

class MovingAverageCrossover final : public Strategy {
public:
    MovingAverageCrossover(
        std::size_t short_window,
        std::size_t long_window
    );

    Signal on_bar(
        const std::vector<MarketBar>& history,
        bool has_position
    ) override;

private:
    std::size_t short_window_;
    std::size_t long_window_;
};