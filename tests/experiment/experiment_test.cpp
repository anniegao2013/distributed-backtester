#include "experiment/experiment.h"
#include "core/json_value.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

int passed = 0;
int failed = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void run_test(
    const std::string& name,
    const std::function<void()>& test
) {
    try {
        test();
        ++passed;
        std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "[FAIL] " << name << ": " << error.what() << '\n';
    }
}

std::string document(
    const std::string& strategies,
    const std::string& experiment_id = "test-experiment",
    const std::string& path = "market.csv"
) {
    return
        R"({"schema_version":1,"experiment_id":")" + experiment_id +
        R"(","data":{"path":")" + path +
        R"(","asset":"TEST","start_date":"2020-01-01",)"
        R"("end_date":"2020-12-31"},"strategies":)" +
        strategies + "}";
}

void expect_validation(
    const std::string& text,
    const std::string& expected_path,
    const std::string& expected_message
) {
    try {
        (void)parse_experiment_json(text);
    } catch (const ExperimentValidationError& error) {
        expect(
            error.path() == expected_path,
            "Expected error path '" + expected_path + "', got '" +
                error.path() + "'"
        );
        expect(
            std::string(error.what()).find(expected_message) !=
                std::string::npos,
            "Expected message containing '" + expected_message +
                "', got '" + error.what() + "'"
        );
        return;
    }

    throw std::runtime_error(
        "Expected validation error at '" + expected_path + "'"
    );
}

const char* kCrossoverGrid =
    R"([{"id":"ma","type":"moving_average_crossover",)"
    R"("grid":{"short_window":[2,3],"long_window":[4]}}])";

void test_minimal_request_expands_jobs() {
    const ExperimentRequest request = parse_experiment_json(
        document(kCrossoverGrid)
    );

    expect(request.schema_version == 1, "schema version should be 1");
    expect(
        request.experiment_id == "test-experiment",
        "experiment id should round-trip"
    );
    expect(request.jobs.size() == 2, "grid should expand to two jobs");
    expect(request.jobs[0].job_id == "ma-1", "first job id should be ma-1");
    expect(request.jobs[1].job_id == "ma-2", "second job id should be ma-2");
    expect(
        request.jobs[0].strategy_type == "moving_average_crossover",
        "strategy type should be carried into jobs"
    );
    expect(
        request.jobs[0].parameters.at("short_window") == 2.0 &&
            request.jobs[0].parameters.at("long_window") == 4.0,
        "first combination should be expanded correctly"
    );

    // Defaults.
    expect(request.execution.initial_cash == 10000.0, "default cash");
    expect(
        request.execution.transaction_cost_rate == 0.0,
        "default transaction costs"
    );
    expect(request.execution.slippage_rate == 0.0, "default slippage");
    expect(
        request.execution.max_concurrent_jobs == 0,
        "default concurrency sentinel"
    );
    expect(!request.output.include_trades, "trades excluded by default");
}

void test_schema_reference_is_accepted() {
    const std::string text =
        R"({"$schema":"../schema/experiment.schema.json",)"
        R"("schema_version":1,"experiment_id":"with-schema",)"
        R"("data":{"path":"market.csv","asset":"TEST",)"
        R"("start_date":"2020-01-01","end_date":"2020-12-31"},)"
        R"("strategies":)" + std::string(kCrossoverGrid) + "}";

    const ExperimentRequest request = parse_experiment_json(text);
    expect(request.jobs.size() == 2, "schema reference should be ignored");
}

void test_explicit_parameters() {
    const auto request = parse_experiment_json(document(
        R"([{"id":"don","type":"donchian_channel_breakout",)"
        R"("parameters":{"entry_period":55,"exit_period":20}}])"
    ));

    expect(request.jobs.size() == 1, "explicit parameters make one job");
    expect(request.jobs[0].job_id == "don-1", "job id should be don-1");
    expect(
        request.jobs[0].parameters.at("entry_period") == 55.0,
        "entry period should be preserved"
    );
}

void test_strategies_interleave_round_robin() {
    const auto request = parse_experiment_json(document(
        R"([)"
        R"({"id":"ma","type":"moving_average_crossover",)"
        R"("grid":{"short_window":[2,3],"long_window":[4]}},)"
        R"({"id":"rsi","type":"rsi_mean_reversion",)"
        R"("grid":{"rsi_period":[2,3],"oversold_threshold":[20],)"
        R"("exit_threshold":[70]}},)"
        R"({"id":"don","type":"donchian_channel_breakout",)"
        R"("grid":{"entry_period":[5,10],"exit_period":[2]}})"
        R"(])"
    ));

    expect(request.jobs.size() == 6, "three strategies of two jobs each");

    const std::vector<std::string> expected{
        "ma-1", "rsi-1", "don-1", "ma-2", "rsi-2", "don-2"
    };

    for (std::size_t i = 0; i < expected.size(); ++i) {
        expect(
            request.jobs[i].job_id == expected[i],
            "job " + std::to_string(i) + " should be " + expected[i]
        );
    }
}

void test_execution_overrides() {
    const std::string text =
        R"({"schema_version":1,"experiment_id":"exec",)"
        R"("data":{"path":"market.csv","asset":"TEST",)"
        R"("start_date":"2020-01-01","end_date":"2020-12-31"},)"
        R"("execution":{"initial_cash":5000,"transaction_cost_rate":0.002,)"
        R"("slippage_rate":0.001,"max_concurrent_jobs":3},)"
        R"("output":{"include_trades":true},)"
        R"("strategies":)" + std::string(kCrossoverGrid) + "}";

    const auto request = parse_experiment_json(text);

    expect(request.execution.initial_cash == 5000.0, "cash override");
    expect(
        request.execution.transaction_cost_rate == 0.002,
        "cost override"
    );
    expect(request.execution.slippage_rate == 0.001, "slippage override");
    expect(
        request.execution.max_concurrent_jobs == 3,
        "concurrency override"
    );
    expect(request.output.include_trades, "trade output override");
}

void test_malformed_json_throws_parse_error() {
    bool threw = false;

    try {
        (void)parse_experiment_json("{");
    } catch (const JsonParseError&) {
        threw = true;
    }

    expect(threw, "malformed JSON should throw JsonParseError");
}

void test_structural_validation() {
    expect_validation(
        "[]",
        "$",
        "Expected an object"
    );

    expect_validation(
        R"({"experiment_id":"x","data":{},"strategies":[]})",
        "$",
        "Missing required field 'schema_version'"
    );

    expect_validation(
        R"({"schema_version":2,"experiment_id":"x","data":{},)"
        R"("strategies":[]})",
        "$.schema_version",
        "Unsupported schema_version"
    );

    expect_validation(
        R"({"schema_version":1,"experiment_id":"x",)"
        R"("data":{"path":"m.csv","asset":"T",)"
        R"("start_date":"2020-01-01","end_date":"2020-12-31"},)"
        R"("strategies":[],"extra":1})",
        "$.extra",
        "Unknown field"
    );

    expect_validation(
        document(
            R"([{"id":"ma","type":"moving_average_crossover",)"
            R"("grid":{"short_window":[2],"long_window":[4]},)"
            R"("parameters":{"short_window":2,"long_window":4}}])"
        ),
        "$.strategies[0]",
        "exactly one of 'grid' or 'parameters'"
    );

    expect_validation(
        document(
            R"([{"id":"ma","type":"moving_average_crossover"}])"
        ),
        "$.strategies[0]",
        "exactly one of 'grid' or 'parameters'"
    );

    expect_validation(
        document(R"([])"),
        "$.strategies",
        "at least one strategy"
    );
}

void test_strategy_validation() {
    expect_validation(
        document(
            R"([{"id":"x","type":"does_not_exist",)"
            R"("grid":{"foo":[1]}}])"
        ),
        "$.strategies[0].grid",
        "Unknown strategy type"
    );

    expect_validation(
        document(
            R"([{"id":"ma","type":"moving_average_crossover",)"
            R"("grid":{"short_window":[2],"long_window":[4],)"
            R"("bogus":[1]}}])"
        ),
        "$.strategies[0].grid.bogus",
        "Unknown parameter"
    );

    expect_validation(
        document(
            R"([{"id":"ma","type":"moving_average_crossover",)"
            R"("grid":{"short_window":[2]}}])"
        ),
        "$.strategies[0].grid",
        "Missing required parameter 'long_window'"
    );

    expect_validation(
        document(
            R"([{"id":"ma","type":"moving_average_crossover",)"
            R"("grid":{"short_window":[2.5],"long_window":[4]}}])"
        ),
        "$.strategies[0].grid.short_window",
        "positive integer"
    );

    expect_validation(
        document(
            R"([{"id":"ma","type":"moving_average_crossover",)"
            R"("grid":{"short_window":[0],"long_window":[4]}}])"
        ),
        "$.strategies[0].grid.short_window",
        "positive integer"
    );

    expect_validation(
        document(
            R"([{"id":"ma","type":"moving_average_crossover",)"
            R"("grid":{"short_window":[5],"long_window":[4]}}])"
        ),
        "$.strategies[0].grid.short_window",
        "must be less than long_window"
    );

    expect_validation(
        document(
            R"([{"id":"rsi","type":"rsi_mean_reversion",)"
            R"("grid":{"rsi_period":[14],"oversold_threshold":[70],)"
            R"("exit_threshold":[30]}}])"
        ),
        "$.strategies[0].grid.oversold_threshold",
        "must be less than exit_threshold"
    );

    expect_validation(
        document(
            R"([{"id":"rsi","type":"rsi_mean_reversion",)"
            R"("parameters":{"rsi_period":14,)"
            R"("oversold_threshold":20,"exit_threshold":150}}])"
        ),
        "$.strategies[0].parameters.exit_threshold",
        "between 0 and 100"
    );

    expect_validation(
        document(
            R"([{"id":"bad id","type":"donchian_channel_breakout",)"
            R"("parameters":{"entry_period":20,"exit_period":5}}])"
        ),
        "$.strategies[0].id",
        "Strategy id"
    );

    expect_validation(
        document(
            R"([{"id":"dup","type":"donchian_channel_breakout",)"
            R"("parameters":{"entry_period":20,"exit_period":5}},)"
            R"({"id":"dup","type":"donchian_channel_breakout",)"
            R"("parameters":{"entry_period":40,"exit_period":10}}])"
        ),
        "$.strategies[1].id",
        "Duplicate strategy id"
    );
}

void test_experiment_validation() {
    expect_validation(
        document(kCrossoverGrid, "bad id"),
        "$.experiment_id",
        "Experiment id"
    );

    expect_validation(
        R"({"schema_version":1,"experiment_id":"x",)"
        R"("data":{"path":"m.csv","asset":"T",)"
        R"("start_date":"2020-13-01","end_date":"2020-12-31"},)"
        R"("strategies":)" + std::string(kCrossoverGrid) + "}",
        "$.data.start_date",
        "YYYY-MM-DD"
    );

    expect_validation(
        R"({"schema_version":1,"experiment_id":"x",)"
        R"("data":{"path":"m.csv","asset":"T",)"
        R"("start_date":"2021-01-01","end_date":"2020-12-31"},)"
        R"("strategies":)" + std::string(kCrossoverGrid) + "}",
        "$.data.start_date",
        "must not be after end_date"
    );

    expect_validation(
        R"({"schema_version":1,"experiment_id":"x",)"
        R"("data":{"path":"m.csv","asset":"T",)"
        R"("start_date":"2020-01-01","end_date":"2020-12-31"},)"
        R"("execution":{"initial_cash":0},)"
        R"("strategies":)" + std::string(kCrossoverGrid) + "}",
        "$.execution.initial_cash",
        "positive and finite"
    );

    expect_validation(
        R"({"schema_version":1,"experiment_id":"x",)"
        R"("data":{"path":"m.csv","asset":"T",)"
        R"("start_date":"2020-01-01","end_date":"2020-12-31"},)"
        R"("execution":{"transaction_cost_rate":1.0},)"
        R"("strategies":)" + std::string(kCrossoverGrid) + "}",
        "$.execution.transaction_cost_rate",
        "[0, 1)"
    );

    expect_validation(
        R"({"schema_version":1,"experiment_id":"x",)"
        R"("data":{"path":"m.csv","asset":"T",)"
        R"("start_date":"2020-01-01","end_date":"2020-12-31"},)"
        R"("execution":{"max_concurrent_jobs":1.5},)"
        R"("strategies":)" + std::string(kCrossoverGrid) + "}",
        "$.execution.max_concurrent_jobs",
        "non-negative integer"
    );

    expect_validation(
        R"({"schema_version":1,"experiment_id":"x",)"
        R"("data":{"path":"m.csv","asset":"T",)"
        R"("start_date":"2020-01-01","end_date":"2020-12-31"},)"
        R"("execution":{"max_concurrent_jobs":1025},)"
        R"("strategies":)" + std::string(kCrossoverGrid) + "}",
        "$.execution.max_concurrent_jobs",
        "at most 1024"
    );

    expect_validation(
        R"({"schema_version":1,"experiment_id":"x",)"
        R"("data":{"path":"","asset":"T",)"
        R"("start_date":"2020-01-01","end_date":"2020-12-31"},)"
        R"("strategies":)" + std::string(kCrossoverGrid) + "}",
        "$.data.path",
        "must not be empty"
    );
}

std::filesystem::path write_market_csv() {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const auto path = std::filesystem::temp_directory_path() /
        ("backtester_experiment_" + unique + ".csv");

    std::ofstream stream(path);
    if (!stream) {
        throw std::runtime_error("Could not create temporary CSV");
    }

    stream << "Date,Open,High,Low,Close,Adj.Close,Volume\n";

    const int days_per_month[] = {31, 29, 31, 30, 31, 30, 31, 31};
    std::size_t index = 0;

    for (int month = 0; month < 8; ++month) {
        for (int day = 1; day <= days_per_month[month]; ++day) {
            const double close =
                100.0 + 10.0 * std::sin(static_cast<double>(index) * 0.7);
            const double open = close;
            const double high = close + 1.0;
            const double low = close - 1.0;

            char date[11];
            std::snprintf(
                date,
                sizeof(date),
                "2020-%02d-%02d",
                month + 1,
                day
            );

            stream << date << ','
                   << open << ','
                   << high << ','
                   << low << ','
                   << close << ','
                   << close << ','
                   << 1000 << '\n';

            ++index;
        }
    }

    return path;
}

const char* kEndToEndStrategy =
    R"([{"id":"ma","type":"moving_average_crossover",)"
    R"("parameters":{"short_window":2,"long_window":3}}])";

std::string end_to_end_document(
    const std::filesystem::path& data_path,
    bool include_trades
) {
    return
        R"({"schema_version":1,"experiment_id":"end-to-end",)"
        R"("data":{"path":")" + data_path.string() +
        R"(","asset":"TEST","start_date":"2020-01-01",)"
        R"("end_date":"2020-04-30"},)"
        R"("execution":{"initial_cash":100000,)"
        R"("transaction_cost_rate":0.001,"slippage_rate":0.0005,)"
        R"("max_concurrent_jobs":2},)"
        R"("output":{"include_trades":)" +
        (include_trades ? "true" : "false") +
        R"(},"strategies":)" + std::string(kEndToEndStrategy) + "}";
}

void test_run_experiment_json() {
    const auto path = write_market_csv();

    try {
        const JsonValue response = parse_json(
            run_experiment_json(end_to_end_document(path, false), false)
        );

        expect(
            response.at("status").as_string() == "completed",
            "run should complete"
        );
        expect(
            response.at("experiment_id").as_string() == "end-to-end",
            "experiment id should round-trip"
        );
        expect(
            response.at("summary").at("jobs_total").as_number() == 1.0,
            "one job should run"
        );
        expect(
            response.at("summary").at("jobs_completed").as_number() == 1.0,
            "one job should complete"
        );
        expect(
            response.at("summary").at("jobs_failed").as_number() == 0.0,
            "no job should fail"
        );
        expect(
            response.at("summary").at("best_job_id").as_string() == "ma-1",
            "best job should be ma-1"
        );
        expect(
            response.at("data").at("trading_days").as_number() ==
                static_cast<double>(31 + 29 + 31 + 30 + 31 + 30 + 31 + 31),
            "all CSV rows should load"
        );

        const JsonValue& result = response.at("results").at(0);
        expect(
            result.at("job_id").as_string() == "ma-1",
            "result job id"
        );
        expect(
            result.at("strategy_type").as_string() ==
                "moving_average_crossover",
            "result strategy type"
        );
        expect(
            result.at("status").as_string() == "completed",
            "result status"
        );
        expect(
            result.at("metrics").at("total_return").is_number(),
            "metrics should be present"
        );
        expect(
            !result.contains("trades"),
            "trades should be omitted by default"
        );

        const JsonValue with_trades = parse_json(
            run_experiment_json(end_to_end_document(path, true), false)
        );
        const JsonValue& trades =
            with_trades.at("results").at(0).at("trades");

        expect(trades.is_array(), "trades should be an array");
        expect(!trades.empty(), "the sample series should produce fills");
        expect(
            trades.at(0).at("side").as_string() == "buy",
            "the first fill should be a buy"
        );
        expect(
            trades.at(0).at("symbol").as_string() == "TEST",
            "trades should use the requested asset"
        );
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_service_error_responses() {
    const JsonValue parse_error = parse_json(
        run_experiment_json("{", false)
    );

    expect(
        parse_error.at("status").as_string() == "invalid",
        "parse errors should be invalid"
    );
    expect(
        parse_error.at("error").at("type").as_string() ==
            "json_parse_error",
        "error type should be json_parse_error"
    );

    const JsonValue validation_error = parse_json(
        run_experiment_json(
            document(
                R"([{"id":"ma","type":"moving_average_crossover",)"
                R"("parameters":{"short_window":0,"long_window":3}}])"
            ),
            false
        )
    );

    expect(
        validation_error.at("status").as_string() == "invalid",
        "schema violations should be invalid"
    );
    expect(
        validation_error.at("error").at("type").as_string() ==
            "validation_error",
        "error type should be validation_error"
    );
    expect(
        validation_error.at("error").at("path").as_string() ==
            "$.strategies[0].parameters.short_window",
        "validation errors should carry a path"
    );

    const JsonValue runtime_error = parse_json(
        run_experiment_json(
            document(kCrossoverGrid, "missing-data", "/no/such/file.csv"),
            false
        )
    );

    expect(
        runtime_error.at("status").as_string() == "failed",
        "data failures should be failed"
    );
    expect(
        runtime_error.at("error").at("type").as_string() ==
            "runtime_error",
        "error type should be runtime_error"
    );
}

void test_strategy_factory() {
    const std::vector<std::string> types = supported_strategy_types();
    expect(types.size() == 3, "three strategy types should be registered");

    const auto ma = create_strategy(
        "moving_average_crossover",
        {{"short_window", 2.0}, {"long_window", 3.0}}
    );
    expect(ma != nullptr, "factory should build a crossover strategy");

    bool threw = false;
    try {
        (void)create_strategy("unknown", {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "unknown strategy types should throw");
}

}  // namespace

int main() {
    run_test("minimal request expands jobs", test_minimal_request_expands_jobs);
    run_test("schema reference accepted", test_schema_reference_is_accepted);
    run_test("explicit parameters", test_explicit_parameters);
    run_test("strategies interleave round-robin", test_strategies_interleave_round_robin);
    run_test("execution overrides", test_execution_overrides);
    run_test("malformed JSON throws", test_malformed_json_throws_parse_error);
    run_test("structural validation", test_structural_validation);
    run_test("strategy validation", test_strategy_validation);
    run_test("experiment validation", test_experiment_validation);
    run_test("run experiment JSON", test_run_experiment_json);
    run_test("service error responses", test_service_error_responses);
    run_test("strategy factory", test_strategy_factory);

    std::cout << "\nResults: " << passed << " passed, "
              << failed << " failed\n";

    return failed == 0 ? 0 : 1;
}
