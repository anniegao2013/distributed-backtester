#include "core/market_data.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    try {
        const std::string filepath =
            "data/market/sp500_1950_2018.csv";

        if (!std::filesystem::exists(filepath)) {
            throw std::runtime_error(
                "Dataset not found: " + filepath);
        }

        const auto bars = load_market_data(filepath);

        if (bars.empty()) {
            throw std::runtime_error(
                "Dataset contains no market data");
        }

        // Verify the dataset spans multiple years.
        if (bars.front().date.substr(0, 4) >=
            bars.back().date.substr(0, 4)) {
            throw std::runtime_error(
                "Dataset does not span multiple years");
        }

        // Verify that every loaded bar contains valid values.
        for (const auto& bar : bars) {
            if (!std::isfinite(bar.open) ||
                !std::isfinite(bar.high) ||
                !std::isfinite(bar.low) ||
                !std::isfinite(bar.close) ||
                !std::isfinite(bar.adjusted_close)) {
                throw std::runtime_error(
                    "Non-finite price on " + bar.date);
            }

            if (bar.open <= 0 || bar.high <= 0 ||
                bar.low <= 0 || bar.close <= 0 ||
                bar.adjusted_close <= 0) {
                throw std::runtime_error(
                    "Nonpositive price on " + bar.date);
            }
        }

        std::cout << "[PASS] OpenIntro dataset integration test\n";
        std::cout << "  Trading days: " << bars.size() << '\n';
        std::cout << "  First date:   " << bars.front().date << '\n';
        std::cout << "  Last date:    " << bars.back().date << '\n';

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] Integration test: "
                  << error.what() << '\n';
        return 1;
    }
}