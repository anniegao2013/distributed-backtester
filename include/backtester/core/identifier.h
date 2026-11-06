#pragma once

#include <string>

// True for IDs that start with an ASCII letter or digit and continue with
// letters, digits, underscores, or hyphens: "crossover-1".
bool is_identifier(const std::string& value);

// Same as is_identifier(), but dots are also allowed: "sweep.v2".
bool is_dotted_identifier(const std::string& value);
