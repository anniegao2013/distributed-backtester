#include "core/market_data.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace {

constexpr int EXPECTED_COLUMNS = 7;

std::vector<std::string> split_csv_line(const std::string& line) {
    // Sufficient for this dataset if fields contain no quoted commas.
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;

    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }

    // Preserve an empty final field, if present.
    if (!line.empty() && line.back() == ',') {
        fields.emplace_back();
    }

    return fields;
}

double parse_double(const std::string& value, std::size_t line_number) {
    std::size_t parsed = 0;
    double result;

    try {
        result = std::stod(value, &parsed);
    } catch (const std::exception&) {
        throw std::runtime_error(
            "Invalid number on CSV line " + std::to_string(line_number));
    }

    if (parsed != value.size() || !std::isfinite(result)) {
        throw std::runtime_error(
            "Invalid or non-finite number on CSV line "
            + std::to_string(line_number));
    }

    return result;
}

std::int64_t parse_volume(
    const std::string& value, std::size_t line_number) {

    std::size_t parsed = 0;
    long long result;

    try {
        result = std::stoll(value, &parsed);
    } catch (const std::exception&) {
        throw std::runtime_error(
            "Invalid volume on CSV line " + std::to_string(line_number));
    }

    if (parsed != value.size() || result < 0) {
        throw std::runtime_error(
            "Invalid volume on CSV line " + std::to_string(line_number));
    }

    return result;
}

bool valid_date(const std::string& date) {
    // Validate the expected YYYY-MM-DD shape and basic month/day ranges.
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') {
        return false;
    }

    for (std::size_t i = 0; i < date.size(); ++i) {
        if (i != 4 && i != 7 &&
            (date[i] < '0' || date[i] > '9')) {
            return false;
        }
    }

    const int year = std::stoi(date.substr(0, 4));
    const int month = std::stoi(date.substr(5, 2));
    const int day = std::stoi(date.substr(8, 2));

    if (year < 1 || month < 1 || month > 12 || day < 1) {
        return false;
    }

    const bool leap_year =
        year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);

    const int days_in_month[] = {
        31, leap_year ? 29 : 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    return day <= days_in_month[month - 1];
}

void validate_bar(const MarketBar& bar, std::size_t line_number) {
    if (!valid_date(bar.date)) {
        throw std::runtime_error(
            "Invalid date on CSV line " + std::to_string(line_number));
    }

    if (bar.open <= 0 || bar.high <= 0 || bar.low <= 0 ||
        bar.close <= 0 || bar.adjusted_close <= 0) {
        throw std::runtime_error(
            "Prices must be positive on CSV line "
            + std::to_string(line_number));
    }

    if (bar.low > bar.high ||
        bar.low > bar.open || bar.low > bar.close ||
        bar.high < bar.open || bar.high < bar.close) {
        throw std::runtime_error(
            "Inconsistent OHLC prices on CSV line "
            + std::to_string(line_number));
    }
}

} // namespace

std::vector<MarketBar> load_market_data(const std::string& filepath) {
    std::ifstream file(filepath);

    if (!file.is_open()) {
        throw std::runtime_error("Could not open CSV: " + filepath);
    }

    std::string line;
    if (!std::getline(file, line)) {
        throw std::runtime_error("CSV is empty: " + filepath);
    }

    // Accept Windows-style CSV files with a UTF-8 BOM.
    if (line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }

    const std::vector<std::string> expected_header = {
        "Date", "Open", "High", "Low",
        "Close", "Adj.Close", "Volume"
    };

    if (split_csv_line(line) != expected_header) {
        throw std::runtime_error("Unexpected CSV header");
    }

    std::vector<MarketBar> bars;
    std::unordered_set<std::string> seen_dates;
    std::size_t line_number = 1;

    while (std::getline(file, line)) {
        ++line_number;

        if (line.empty()) {
            throw std::runtime_error(
                "Empty row on CSV line " + std::to_string(line_number));
        }

        const auto fields = split_csv_line(line);

        if (fields.size() != EXPECTED_COLUMNS) {
            throw std::runtime_error(
                "Expected 7 columns on CSV line "
                + std::to_string(line_number));
        }

        MarketBar bar{
            fields[0],
            parse_double(fields[1], line_number),
            parse_double(fields[2], line_number),
            parse_double(fields[3], line_number),
            parse_double(fields[4], line_number),
            parse_double(fields[5], line_number),
            parse_volume(fields[6], line_number)
        };

        validate_bar(bar, line_number);

        if (!seen_dates.insert(bar.date).second) {
            throw std::runtime_error("Duplicate date: " + bar.date);
        }

        if (!bars.empty() && bar.date <= bars.back().date) {
            throw std::runtime_error(
                "Dates must be strictly ascending; problem on line "
                + std::to_string(line_number));
        }

        bars.push_back(bar);
    }

    if (file.bad()) {
        throw std::runtime_error("Error reading CSV: " + filepath);
    }

    if (bars.empty()) {
        throw std::runtime_error("CSV contains no market data");
    }

    return bars;
}