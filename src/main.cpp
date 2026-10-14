#include "market_data.h"

#include <exception>
#include <iostream>

int main() {
    try {
        const auto bars =
            load_market_data("data/market/sp500_1950_2018.csv");

        std::cout << "Successfully loaded " << bars.size()
                  << " trading days.\n";

        std::cout << "First date: " << bars.front().date << '\n';
        std::cout << "Last date:  " << bars.back().date << '\n';

        const auto& first = bars.front();
        std::cout << "First close: " << first.close << '\n';

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}