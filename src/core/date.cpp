#include "core/date.h"

#include <cstddef>
#include <string>

namespace {

bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

}  // namespace

bool is_valid_iso_date(const std::string& value) {
    if (value.size() != 10 ||
        value[4] != '-' ||
        value[7] != '-') {
        return false;
    }

    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 4 || i == 7) {
            continue;
        }
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
    }

    const int year =
        std::stoi(value.substr(0, 4));
    const int month =
        std::stoi(value.substr(5, 2));
    const int day =
        std::stoi(value.substr(8, 2));

    if (month < 1 || month > 12) {
        return false;
    }

    static const int kDaysPerMonth[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    int max_day = kDaysPerMonth[month - 1];
    if (month == 2 && is_leap_year(year)) {
        max_day = 29;
    }

    return day >= 1 && day <= max_day;
}
