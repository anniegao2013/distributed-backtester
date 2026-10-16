#include "market_data.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

const std::string HEADER =
    "Date,Open,High,Low,Close,Adj.Close,Volume\n";

const fs::path TEST_DIR =
    fs::temp_directory_path() / "distributed_backtester_tests";

int passed = 0;
int failed = 0;

void write_csv(const std::string& filename, const std::string& contents) {
    std::ofstream file(TEST_DIR / filename);

    if (!file) {
        throw std::runtime_error("Could not create test CSV");
    }

    file << contents;
}

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_load_failure(const std::string& filename) {
    bool threw = false;

    try {
        (void)load_market_data((TEST_DIR / filename).string());
    } catch (const std::exception&) {
        threw = true;
    }

    expect(threw, "Expected loader to reject " + filename);
}

void run_test(
    const std::string& name,
    const std::function<void()>& test) {

    try {
        test();
        ++passed;
        std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "[FAIL] " << name << ": "
                  << error.what() << '\n';
    }
}

void test_valid_csv() {
    write_csv("valid.csv",
        HEADER +
        "2018-01-02,10,12,9,11,11,1000\n"
        "2018-01-03,11,13,10,12,12,1500\n");

    const auto bars =
        load_market_data((TEST_DIR / "valid.csv").string());

    expect(bars.size() == 2, "Expected two bars");
    expect(bars[0].date == "2018-01-02", "Wrong first date");
    expect(bars[1].date == "2018-01-03", "Wrong second date");
    expect(bars[0].open == 10.0, "Wrong open price");
    expect(bars[0].high == 12.0, "Wrong high price");
    expect(bars[0].low == 9.0, "Wrong low price");
    expect(bars[0].close == 11.0, "Wrong close price");
    expect(bars[0].adjusted_close == 11.0, "Wrong adjusted close");
    expect(bars[1].volume == 1500, "Wrong volume");
}

void test_header_only_csv() {
    write_csv("header_only.csv", HEADER);
    expect_load_failure("header_only.csv");
}

void test_empty_csv() {
    write_csv("empty.csv", "");
    expect_load_failure("empty.csv");
}

void test_missing_file() {
    expect_load_failure("does_not_exist.csv");
}

void test_invalid_header() {
    write_csv("bad_header.csv",
        "Date,Open,High,Low,Close,Volume\n"
        "2018-01-02,10,12,9,11,1000\n");

    expect_load_failure("bad_header.csv");
}

void test_wrong_column_count() {
    write_csv("wrong_columns.csv",
        HEADER +
        "2018-01-02,10,12,9,11,11\n");

    expect_load_failure("wrong_columns.csv");
}

void test_invalid_date() {
    write_csv("invalid_date.csv",
        HEADER +
        "2018-02-30,10,12,9,11,11,1000\n");

    expect_load_failure("invalid_date.csv");
}

void test_invalid_numeric_value() {
    write_csv("invalid_number.csv",
        HEADER +
        "2018-01-02,abc,12,9,11,11,1000\n");

    expect_load_failure("invalid_number.csv");
}

void test_nonpositive_price() {
    write_csv("zero_price.csv",
        HEADER +
        "2018-01-02,0,12,0,11,11,1000\n");

    expect_load_failure("zero_price.csv");
}

void test_inconsistent_ohlc() {
    write_csv("bad_ohlc.csv",
        HEADER +
        "2018-01-02,10,12,11,10,10,1000\n");

    expect_load_failure("bad_ohlc.csv");
}

void test_negative_volume() {
    write_csv("negative_volume.csv",
        HEADER +
        "2018-01-02,10,12,9,11,11,-100\n");

    expect_load_failure("negative_volume.csv");
}

void test_duplicate_dates() {
    write_csv("duplicate_dates.csv",
        HEADER +
        "2018-01-02,10,12,9,11,11,1000\n"
        "2018-01-02,11,13,10,12,12,1500\n");

    expect_load_failure("duplicate_dates.csv");
}

void test_out_of_order_dates() {
    write_csv("out_of_order.csv",
        HEADER +
        "2018-01-03,11,13,10,12,12,1500\n"
        "2018-01-02,10,12,9,11,11,1000\n");

    expect_load_failure("out_of_order.csv");
}

} // namespace

int main() {
    fs::create_directories(TEST_DIR);

    run_test("valid CSV loads correctly", test_valid_csv);
    run_test("header-only CSV rejected", test_header_only_csv);
    run_test("empty CSV rejected", test_empty_csv);
    run_test("missing file rejected", test_missing_file);
    run_test("invalid header rejected", test_invalid_header);
    run_test("wrong column count rejected", test_wrong_column_count);
    run_test("invalid date rejected", test_invalid_date);
    run_test("invalid numeric value rejected", test_invalid_numeric_value);
    run_test("nonpositive price rejected", test_nonpositive_price);
    run_test("inconsistent OHLC rejected", test_inconsistent_ohlc);
    run_test("negative volume rejected", test_negative_volume);
    run_test("duplicate dates rejected", test_duplicate_dates);
    run_test("out-of-order dates rejected", test_out_of_order_dates);

    fs::remove_all(TEST_DIR);

    std::cout << "\nResults: " << passed << " passed, "
              << failed << " failed\n";

    return failed == 0 ? 0 : 1;
}