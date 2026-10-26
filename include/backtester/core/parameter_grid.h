
#ifndef PARAMETER_GRID_H
#define PARAMETER_GRID_H

#include "core/parameter_types.h"

#include <map>
#include <string>
#include <vector>

using ParameterGrid = std::map<std::string, std::vector<double>>;

std::vector<ParameterMap> generate_parameter_combinations(
    const ParameterGrid& grid
);

#endif
