#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MarketBar {
    std::string date;
    double open;
    double high;
    double low;
    double close;
    double adjusted_close;
    std::int64_t volume;
};

std::vector<MarketBar> load_market_data(const std::string& filepath);