
#include "core/parameter_grid.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_cartesian_product() {
    const ParameterGrid grid{
        {"short_window", {5.0, 10.0}},
        {"long_window", {20.0, 50.0}}
    };

    const auto combinations = generate_parameter_combinations(grid);

    expect(combinations.size() == 4, "Expected four combinations");

    // std::map orders parameter names alphabetically:
    // long_window is enumerated first, then short_window.
    expect(combinations[0].at("long_window") == 20.0,
           "Unexpected first long window");
    expect(combinations[0].at("short_window") == 5.0,
           "Unexpected first short window");

    expect(combinations[1].at("long_window") == 20.0,
           "Unexpected second long window");
    expect(combinations[1].at("short_window") == 10.0,
           "Unexpected second short window");

    expect(combinations[2].at("long_window") == 50.0,
           "Unexpected third long window");
    expect(combinations[2].at("short_window") == 5.0,
           "Unexpected third short window");
}

void test_single_parameter() {
    const ParameterGrid grid{
        {"threshold", {0.1, 0.2, 0.3}}
    };

    const auto combinations = generate_parameter_combinations(grid);

    expect(combinations.size() == 3,
           "Expected three combinations");

    expect(combinations[0].at("threshold") == 0.1,
           "Unexpected first value");
    expect(combinations[2].at("threshold") == 0.3,
           "Unexpected last value");
}

void test_empty_grid() {
    const ParameterGrid grid;

    const auto combinations = generate_parameter_combinations(grid);

    expect(combinations.size() == 1,
           "Empty grid should produce one combination");
    expect(combinations[0].empty(),
           "Empty grid should produce an empty parameter map");
}

void test_empty_value_list_throws() {
    const ParameterGrid grid{
        {"threshold", {}}
    };

    bool threw = false;

    try {
        generate_parameter_combinations(grid);
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    expect(threw, "Empty value list should throw");
}

int main() {
    try {
        test_cartesian_product();
        test_single_parameter();
        test_empty_grid();
        test_empty_value_list_throws();

        std::cout << "All parameter grid tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
