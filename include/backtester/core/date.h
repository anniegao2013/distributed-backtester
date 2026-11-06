#pragma once

#include <string>

// Returns true when `value` is a real calendar date in YYYY-MM-DD form.
bool is_valid_iso_date(const std::string& value);
