
#include "parameter_grid.h"

#include <cmath>
#include <functional>
#include <stdexcept>

std::vector<ParameterMap> generate_parameter_combinations(
    const ParameterGrid& grid
) {
    // Validate the grid before generating any combinations.
    for (const auto& [name, values] : grid) {
        if (name.empty()) {
            throw std::invalid_argument(
                "Parameter names must not be empty"
            );
        }

        if (values.empty()) {
            throw std::invalid_argument(
                "Parameter value lists must not be empty"
            );
        }

        for (double value : values) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument(
                    "Parameter values must be finite"
                );
            }
        }
    }

    std::vector<ParameterMap> combinations;
    ParameterMap current;

    // Recursively assign one value to each parameter.
    std::function<void(ParameterGrid::const_iterator)> generate =
        [&](ParameterGrid::const_iterator parameter) {
            if (parameter == grid.end()) {
                combinations.push_back(current);
                return;
            }

            const auto& [name, values] = *parameter;

            for (double value : values) {
                current[name] = value;
                generate(std::next(parameter));
            }

            current.erase(name);
        };

    generate(grid.begin());
    return combinations;
}
