#include "experiment.h"

#include "donchian_channel_breakout.h"
#include "json_value.h"
#include "market_data.h"
#include "moving_average_crossover.h"
#include "rsi_mean_reversion.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <utility>
#include <vector>

namespace {

using JsonObject = JsonValue::Object;

constexpr const char* kRootPath = "$";
constexpr const char* kSchemaVersion = "1";

// A single request may not spawn an unbounded number of worker threads.
constexpr double kMaxConcurrentJobs = 1024.0;

// -------------------------------------------------------------------
// Schema helpers
// -------------------------------------------------------------------

[[noreturn]] void fail(
    const std::string& path,
    const std::string& message
) {
    throw ExperimentValidationError(path, message);
}

std::string child_path(
    const std::string& parent,
    const std::string& key
) {
    return parent + "." + key;
}

std::string index_path(const std::string& parent, std::size_t index) {
    return parent + "[" + std::to_string(index) + "]";
}

void expect_object(const JsonValue& value, const std::string& path) {
    if (!value.is_object()) {
        fail(
            path,
            std::string("Expected an object, got ") + value.type_name()
        );
    }
}

void expect_array(const JsonValue& value, const std::string& path) {
    if (!value.is_array()) {
        fail(
            path,
            std::string("Expected an array, got ") + value.type_name()
        );
    }
}

const JsonValue& require_member(
    const JsonValue& object,
    const std::string& key,
    const std::string& path
) {
    if (!object.contains(key)) {
        fail(path, "Missing required field '" + key + "'");
    }

    return object.at(key);
}

void reject_unknown_members(
    const JsonValue& object,
    const std::vector<std::string>& allowed,
    const std::string& path
) {
    for (const auto& [key, value] : object.as_object()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) ==
            allowed.end()) {
            fail(
                child_path(path, key),
                "Unknown field '" + key + "'"
            );
        }
    }
}

std::string require_string(
    const JsonValue& value,
    const std::string& path
) {
    if (!value.is_string()) {
        fail(
            path,
            std::string("Expected a string, got ") + value.type_name()
        );
    }

    return value.as_string();
}

double require_number(
    const JsonValue& value,
    const std::string& path
) {
    if (!value.is_number()) {
        fail(
            path,
            std::string("Expected a number, got ") + value.type_name()
        );
    }

    const double result = value.as_number();
    if (!std::isfinite(result)) {
        fail(path, "Expected a finite number");
    }

    return result;
}

bool require_boolean(
    const JsonValue& value,
    const std::string& path
) {
    if (!value.is_boolean()) {
        fail(
            path,
            std::string("Expected a boolean, got ") + value.type_name()
        );
    }

    return value.as_boolean();
}

// -------------------------------------------------------------------
// Semantic validation helpers
// -------------------------------------------------------------------

bool is_ascii_alphanumeric(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

// URL-safe identifier used for strategy IDs: starts alphanumeric, then
// alphanumerics, underscores, or hyphens.
bool is_identifier(const std::string& value) {
    if (value.empty() || !is_ascii_alphanumeric(value.front())) {
        return false;
    }

    return std::all_of(
        value.begin(),
        value.end(),
        [](char c) {
            return is_ascii_alphanumeric(c) || c == '_' || c == '-';
        }
    );
}

// Experiment IDs additionally allow dots (for versions such as "sweep.v2").
bool is_experiment_id(const std::string& value) {
    if (value.empty() || !is_ascii_alphanumeric(value.front())) {
        return false;
    }

    return std::all_of(
        value.begin(),
        value.end(),
        [](char c) {
            return is_ascii_alphanumeric(c) ||
                   c == '_' || c == '-' || c == '.';
        }
    );
}

bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

bool valid_date(const std::string& value) {
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

void validate_identifier(
    const std::string& value,
    const std::string& path,
    bool allow_dots,
    const std::string& description
) {
    const bool valid = allow_dots
        ? is_experiment_id(value)
        : is_identifier(value);

    if (!valid) {
        fail(
            path,
            description +
                " must start with a letter or digit and contain only "
                "letters, digits, underscores, and hyphens" +
                std::string(allow_dots ? " (dots are also allowed)" : "")
        );
    }
}

void validate_positive_integer(
    double value,
    const std::string& path
) {
    if (std::floor(value) != value || value < 1.0) {
        fail(path, "Expected a positive integer");
    }

    if (value > 1.0e6) {
        fail(path, "Value must be at most 1000000");
    }
}

// -------------------------------------------------------------------
// Strategy parameter rules
// -------------------------------------------------------------------

enum class ParameterKind {
    PositiveInteger,
    Percentage
};

struct ParameterRule {
    std::string name;
    ParameterKind kind;
};

struct StrategyRule {
    std::string type;
    std::vector<ParameterRule> parameters;
};

const std::vector<StrategyRule>& strategy_rules() {
    static const std::vector<StrategyRule> rules{
        {
            "moving_average_crossover",
            {
                {"short_window", ParameterKind::PositiveInteger},
                {"long_window", ParameterKind::PositiveInteger}
            }
        },
        {
            "rsi_mean_reversion",
            {
                {"rsi_period", ParameterKind::PositiveInteger},
                {"oversold_threshold", ParameterKind::Percentage},
                {"exit_threshold", ParameterKind::Percentage}
            }
        },
        {
            "donchian_channel_breakout",
            {
                {"entry_period", ParameterKind::PositiveInteger},
                {"exit_period", ParameterKind::PositiveInteger}
            }
        }
    };

    return rules;
}

const StrategyRule* find_strategy_rule(const std::string& type) {
    const auto& rules = strategy_rules();
    const auto it = std::find_if(
        rules.begin(),
        rules.end(),
        [&type](const StrategyRule& rule) {
            return rule.type == type;
        }
    );

    return it == rules.end() ? nullptr : &*it;
}

std::string join_quoted(const std::vector<std::string>& values) {
    std::string result;

    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            result += ", ";
        }
        result += "'" + values[i] + "'";
    }

    return result;
}

void validate_parameters(
    const std::string& type,
    const ParameterMap& parameters,
    const std::string& path
) {
    const StrategyRule* rule = find_strategy_rule(type);
    if (rule == nullptr) {
        const auto types = supported_strategy_types();
        fail(
            path,
            "Unknown strategy type '" + type +
                "'; supported types are " + join_quoted(types)
        );
    }

    std::set<std::string> expected;
    for (const auto& parameter : rule->parameters) {
        expected.insert(parameter.name);
    }

    for (const auto& [name, value] : parameters) {
        (void)value;

        if (expected.find(name) == expected.end()) {
            std::vector<std::string> names;
            names.reserve(rule->parameters.size());
            for (const auto& parameter : rule->parameters) {
                names.push_back(parameter.name);
            }

            fail(
                child_path(path, name),
                "Unknown parameter '" + name + "' for strategy '" +
                    type + "'; expected " + join_quoted(names)
            );
        }
    }

    for (const auto& parameter : rule->parameters) {
        const auto it = parameters.find(parameter.name);
        const std::string parameter_path =
            child_path(path, parameter.name);

        if (it == parameters.end()) {
            fail(
                path,
                "Missing required parameter '" + parameter.name +
                    "' for strategy '" + type + "'"
            );
        }

        const double value = it->second;
        if (!std::isfinite(value)) {
            fail(parameter_path, "Expected a finite number");
        }

        switch (parameter.kind) {
            case ParameterKind::PositiveInteger:
                validate_positive_integer(value, parameter_path);
                break;
            case ParameterKind::Percentage:
                if (value < 0.0 || value > 100.0) {
                    fail(
                        parameter_path,
                        "Expected a value between 0 and 100"
                    );
                }
                break;
        }
    }

    // Cross-parameter rules.
    if (type == "moving_average_crossover") {
        if (parameters.at("short_window") >=
            parameters.at("long_window")) {
            fail(
                child_path(path, "short_window"),
                "short_window must be less than long_window"
            );
        }
    }

    if (type == "rsi_mean_reversion") {
        if (parameters.at("oversold_threshold") >=
            parameters.at("exit_threshold")) {
            fail(
                child_path(path, "oversold_threshold"),
                "oversold_threshold must be less than exit_threshold"
            );
        }
    }
}

// -------------------------------------------------------------------
// Request parsing
// -------------------------------------------------------------------

ExperimentData parse_data(const JsonValue& value) {
    const std::string path = child_path(kRootPath, "data");
    expect_object(value, path);
    reject_unknown_members(
        value,
        {"path", "asset", "start_date", "end_date"},
        path
    );

    ExperimentData data;
    data.path = require_string(
        require_member(value, "path", path),
        child_path(path, "path")
    );
    data.asset = require_string(
        require_member(value, "asset", path),
        child_path(path, "asset")
    );
    data.start_date = require_string(
        require_member(value, "start_date", path),
        child_path(path, "start_date")
    );
    data.end_date = require_string(
        require_member(value, "end_date", path),
        child_path(path, "end_date")
    );

    return data;
}

ExperimentExecution parse_execution(const JsonValue& value) {
    const std::string path = child_path(kRootPath, "execution");
    expect_object(value, path);
    reject_unknown_members(
        value,
        {
            "initial_cash",
            "transaction_cost_rate",
            "slippage_rate",
            "max_concurrent_jobs"
        },
        path
    );

    ExperimentExecution execution;

    if (value.contains("initial_cash")) {
        execution.initial_cash = require_number(
            value.at("initial_cash"),
            child_path(path, "initial_cash")
        );
    }

    if (value.contains("transaction_cost_rate")) {
        execution.transaction_cost_rate = require_number(
            value.at("transaction_cost_rate"),
            child_path(path, "transaction_cost_rate")
        );
    }

    if (value.contains("slippage_rate")) {
        execution.slippage_rate = require_number(
            value.at("slippage_rate"),
            child_path(path, "slippage_rate")
        );
    }

    if (value.contains("max_concurrent_jobs")) {
        const std::string member_path =
            child_path(path, "max_concurrent_jobs");
        const double jobs = require_number(
            value.at("max_concurrent_jobs"),
            member_path
        );

        if (std::floor(jobs) != jobs || jobs < 0.0) {
            fail(member_path, "Expected a non-negative integer");
        }

        if (jobs > kMaxConcurrentJobs) {
            fail(member_path, "Value must be at most 1024");
        }

        execution.max_concurrent_jobs =
            static_cast<std::size_t>(jobs);
    }

    return execution;
}

ExperimentOutput parse_output(const JsonValue& value) {
    const std::string path = child_path(kRootPath, "output");
    expect_object(value, path);
    reject_unknown_members(value, {"include_trades"}, path);

    ExperimentOutput output;

    if (value.contains("include_trades")) {
        output.include_trades = require_boolean(
            value.at("include_trades"),
            child_path(path, "include_trades")
        );
    }

    return output;
}

ExperimentStrategy parse_strategy(
    const JsonValue& value,
    std::size_t index
) {
    const std::string path =
        index_path(child_path(kRootPath, "strategies"), index);

    expect_object(value, path);
    reject_unknown_members(
        value,
        {"id", "type", "grid", "parameters"},
        path
    );

    ExperimentStrategy strategy;
    strategy.id = require_string(
        require_member(value, "id", path),
        child_path(path, "id")
    );
    strategy.type = require_string(
        require_member(value, "type", path),
        child_path(path, "type")
    );

    const bool has_grid = value.contains("grid");
    const bool has_parameters = value.contains("parameters");

    if (has_grid == has_parameters) {
        fail(
            path,
            "Each strategy must define exactly one of 'grid' or "
            "'parameters'"
        );
    }

    if (has_grid) {
        const JsonValue& grid = value.at("grid");
        const std::string grid_path = child_path(path, "grid");
        expect_object(grid, grid_path);

        if (grid.empty()) {
            fail(grid_path, "A grid must contain at least one parameter");
        }

        for (const auto& [name, values] : grid.as_object()) {
            const std::string values_path =
                child_path(grid_path, name);

            expect_array(values, values_path);

            if (values.empty()) {
                fail(
                    values_path,
                    "A grid value list must contain at least one value"
                );
            }

            std::vector<double> parsed;
            parsed.reserve(values.size());

            for (std::size_t i = 0; i < values.size(); ++i) {
                const std::string value_path = index_path(values_path, i);
                const double number = require_number(
                    values.as_array()[i],
                    value_path
                );

                if (!std::isfinite(number)) {
                    fail(value_path, "Expected a finite number");
                }

                parsed.push_back(number);
            }

            strategy.grid.emplace(name, std::move(parsed));
        }
    } else {
        const JsonValue& parameters = value.at("parameters");
        const std::string parameters_path =
            child_path(path, "parameters");
        expect_object(parameters, parameters_path);

        if (parameters.empty()) {
            fail(
                parameters_path,
                "Explicit parameters must contain at least one parameter"
            );
        }

        ParameterMap parsed;
        for (const auto& [name, parameter] : parameters.as_object()) {
            parsed.emplace(
                name,
                require_number(
                    parameter,
                    child_path(parameters_path, name)
                )
            );
        }

        strategy.parameter_sets.push_back(std::move(parsed));
    }

    return strategy;
}

ExperimentRequest parse_document(const JsonValue& root) {
    expect_object(root, kRootPath);
    reject_unknown_members(
        root,
        {
            "$schema",
            "schema_version",
            "experiment_id",
            "description",
            "data",
            "execution",
            "strategies",
            "output"
        },
        kRootPath
    );

    ExperimentRequest request;

    // Optional editor/schema reference. Validation is performed by this
    // parser, not by resolving the URI.
    if (root.contains("$schema")) {
        (void)require_string(
            root.at("$schema"),
            child_path(kRootPath, "$schema")
        );
    }

    const std::string version_path =
        child_path(kRootPath, "schema_version");
    const double version = require_number(
        require_member(root, "schema_version", kRootPath),
        version_path
    );

    if (std::floor(version) != version ||
        version != 1.0) {
        fail(
            version_path,
            "Unsupported schema_version; this backtester supports " +
                std::string(kSchemaVersion)
        );
    }

    request.schema_version = 1;

    request.experiment_id = require_string(
        require_member(root, "experiment_id", kRootPath),
        child_path(kRootPath, "experiment_id")
    );

    if (root.contains("description")) {
        request.description = require_string(
            root.at("description"),
            child_path(kRootPath, "description")
        );
    }

    request.data = parse_data(
        require_member(root, "data", kRootPath)
    );

    if (root.contains("execution")) {
        request.execution = parse_execution(root.at("execution"));
    }

    if (root.contains("output")) {
        request.output = parse_output(root.at("output"));
    }

    const JsonValue& strategies = require_member(
        root,
        "strategies",
        kRootPath
    );
    const std::string strategies_path =
        child_path(kRootPath, "strategies");
    expect_array(strategies, strategies_path);

    if (strategies.empty()) {
        fail(
            strategies_path,
            "An experiment must define at least one strategy"
        );
    }

    request.strategies.reserve(strategies.size());
    for (std::size_t i = 0; i < strategies.size(); ++i) {
        request.strategies.push_back(
            parse_strategy(strategies.at(i), i)
        );
    }

    return request;
}

// -------------------------------------------------------------------
// Job expansion and execution
// -------------------------------------------------------------------

void validate_and_expand_strategy(
    const ExperimentStrategy& strategy,
    std::size_t index,
    std::vector<ExperimentJob>& jobs
) {
    const std::string path =
        index_path(child_path(kRootPath, "strategies"), index);

    validate_identifier(
        strategy.id,
        child_path(path, "id"),
        false,
        "Strategy id"
    );

    if (strategy.type.empty()) {
        fail(child_path(path, "type"), "Strategy type must not be empty");
    }

    const bool has_grid = !strategy.grid.empty();
    const bool has_parameters = !strategy.parameter_sets.empty();

    if (has_grid == has_parameters) {
        fail(
            path,
            "Each strategy must define exactly one of 'grid' or "
            "'parameters'"
        );
    }

    std::vector<ParameterMap> parameter_sets;

    if (has_grid) {
        const std::string grid_path = child_path(path, "grid");

        for (const auto& [name, values] : strategy.grid) {
            if (name.empty()) {
                fail(grid_path, "Parameter names must not be empty");
            }

            if (values.empty()) {
                fail(
                    child_path(grid_path, name),
                    "A grid value list must contain at least one value"
                );
            }
        }

        try {
            parameter_sets =
                generate_parameter_combinations(strategy.grid);
        } catch (const std::exception& error) {
            fail(grid_path, error.what());
        }

        for (const auto& parameters : parameter_sets) {
            validate_parameters(
                strategy.type,
                parameters,
                grid_path
            );
        }
    } else {
        if (strategy.parameter_sets.size() != 1) {
            fail(
                child_path(path, "parameters"),
                "Explicit parameter mode accepts exactly one combination"
            );
        }

        parameter_sets = strategy.parameter_sets;
        validate_parameters(
            strategy.type,
            parameter_sets.front(),
            child_path(path, "parameters")
        );
    }

    for (std::size_t i = 0; i < parameter_sets.size(); ++i) {
        jobs.push_back({
            strategy.id + "-" + std::to_string(i + 1),
            strategy.id,
            strategy.type,
            parameter_sets[i]
        });
    }
}

ExperimentJobOutcome run_job(
    const std::vector<MarketBar>& dataset,
    const ExperimentData& data,
    const ExperimentExecution& execution,
    const ExperimentJob& job
) {
    ExperimentJobOutcome outcome;
    outcome.job = job;

    const auto start = std::chrono::steady_clock::now();

    try {
        const BacktestConfig config{
            job.job_id,
            data.asset,
            data.start_date,
            data.end_date,
            execution.initial_cash,
            job.parameters,
            execution.transaction_cost_rate,
            execution.slippage_rate
        };

        const auto strategy =
            create_strategy(job.strategy_type, job.parameters);

        outcome.result = run_backtest(dataset, *strategy, config);
        outcome.ok = true;
    } catch (const std::exception& error) {
        outcome.error = error.what();
    }

    const auto end = std::chrono::steady_clock::now();
    outcome.duration_seconds =
        std::chrono::duration<double>(end - start).count();

    return outcome;
}

double process_cpu_seconds() {
    struct rusage usage;

    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }

    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
           usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}

JsonValue parameters_to_json(const ParameterMap& parameters) {
    JsonObject object;

    for (const auto& [name, value] : parameters) {
        object.emplace(name, value);
    }

    return JsonValue(std::move(object));
}

JsonValue metrics_to_json(const BacktestMetrics& metrics) {
    JsonObject object;
    object.emplace("total_return", metrics.total_return);
    object.emplace("sharpe_ratio", metrics.sharpe_ratio);
    object.emplace("max_drawdown", metrics.max_drawdown);
    object.emplace(
        "number_of_trades",
        static_cast<double>(metrics.number_of_trades)
    );
    object.emplace("total_costs", metrics.total_costs);

    return JsonValue(std::move(object));
}

JsonValue trade_to_json(const Trade& trade) {
    JsonObject object;
    object.emplace("symbol", trade.symbol);
    object.emplace(
        "side",
        trade.side == OrderSide::Buy ? "buy" : "sell"
    );
    object.emplace("quantity", trade.quantity);
    object.emplace("fill_price", trade.fill_price);
    object.emplace("commission", trade.commission);
    object.emplace("slippage_cost", trade.slippage_cost);

    return JsonValue(std::move(object));
}

JsonValue performance_to_json(const PerformanceStats& stats) {
    JsonObject object;
    object.emplace(
        "max_concurrent",
        static_cast<double>(stats.max_concurrent)
    );
    object.emplace("throughput", stats.throughput);
    object.emplace("mean_latency", stats.mean_latency);
    object.emplace("median_latency", stats.median_latency);
    object.emplace("min_latency", stats.min_latency);
    object.emplace("max_latency", stats.max_latency);
    object.emplace("p95_latency", stats.p95_latency);
    object.emplace("total_work_seconds", stats.total_work_seconds);
    object.emplace("speedup", stats.speedup);
    object.emplace("cpu_seconds", stats.cpu_seconds);
    object.emplace("cpu_utilization", stats.cpu_utilization);
    object.emplace("cpu_efficiency", stats.cpu_efficiency);

    return JsonValue(std::move(object));
}

JsonValue error_document(
    const std::string& status,
    const std::string& type,
    const std::string& path,
    const std::string& message
) {
    JsonObject root;
    root.emplace("schema_version", 1);
    root.emplace("status", status);

    JsonObject error;
    error.emplace("type", type);
    if (!path.empty()) {
        error.emplace("path", path);
    }
    error.emplace("message", message);
    root.emplace("error", JsonValue(std::move(error)));

    return JsonValue(std::move(root));
}

}  // namespace

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

ExperimentValidationError::ExperimentValidationError(
    std::string path,
    std::string message
) : std::runtime_error(std::move(message)), path_(std::move(path)) {}

const std::string& ExperimentValidationError::path() const noexcept {
    return path_;
}

ExperimentRequest parse_experiment_json(const std::string& text) {
    ExperimentRequest request = parse_document(parse_json(text));
    finalize_experiment(request);
    return request;
}

ExperimentRequest load_experiment_file(const std::string& path) {
    std::ifstream stream(path);

    if (!stream) {
        throw std::runtime_error(
            "Could not open experiment file '" + path + "'"
        );
    }

    std::ostringstream contents;
    contents << stream.rdbuf();

    if (!stream.good() && !stream.eof()) {
        throw std::runtime_error(
            "Could not read experiment file '" + path + "'"
        );
    }

    return parse_experiment_json(contents.str());
}

void finalize_experiment(ExperimentRequest& request) {
    if (request.schema_version != 1) {
        fail(
            child_path(kRootPath, "schema_version"),
            "Unsupported schema_version; this backtester supports " +
                std::string(kSchemaVersion)
        );
    }

    validate_identifier(
        request.experiment_id,
        child_path(kRootPath, "experiment_id"),
        true,
        "Experiment id"
    );

    if (request.data.path.empty()) {
        fail(
            child_path(child_path(kRootPath, "data"), "path"),
            "Data path must not be empty"
        );
    }

    if (request.data.asset.empty()) {
        fail(
            child_path(child_path(kRootPath, "data"), "asset"),
            "Asset must not be empty"
        );
    }

    if (!valid_date(request.data.start_date)) {
        fail(
            child_path(child_path(kRootPath, "data"), "start_date"),
            "Expected a date in YYYY-MM-DD format"
        );
    }

    if (!valid_date(request.data.end_date)) {
        fail(
            child_path(child_path(kRootPath, "data"), "end_date"),
            "Expected a date in YYYY-MM-DD format"
        );
    }

    if (request.data.start_date > request.data.end_date) {
        fail(
            child_path(child_path(kRootPath, "data"), "start_date"),
            "start_date must not be after end_date"
        );
    }

    const std::string execution_path =
        child_path(kRootPath, "execution");

    if (!std::isfinite(request.execution.initial_cash) ||
        request.execution.initial_cash <= 0.0) {
        fail(
            child_path(execution_path, "initial_cash"),
            "initial_cash must be positive and finite"
        );
    }

    if (!std::isfinite(request.execution.transaction_cost_rate) ||
        request.execution.transaction_cost_rate < 0.0 ||
        request.execution.transaction_cost_rate >= 1.0) {
        fail(
            child_path(execution_path, "transaction_cost_rate"),
            "transaction_cost_rate must be in [0, 1)"
        );
    }

    if (!std::isfinite(request.execution.slippage_rate) ||
        request.execution.slippage_rate < 0.0 ||
        request.execution.slippage_rate >= 1.0) {
        fail(
            child_path(execution_path, "slippage_rate"),
            "slippage_rate must be in [0, 1)"
        );
    }

    if (static_cast<double>(request.execution.max_concurrent_jobs) >
        kMaxConcurrentJobs) {
        fail(
            child_path(execution_path, "max_concurrent_jobs"),
            "max_concurrent_jobs must be at most 1024"
        );
    }

    const std::string strategies_path =
        child_path(kRootPath, "strategies");

    if (request.strategies.empty()) {
        fail(
            strategies_path,
            "An experiment must define at least one strategy"
        );
    }

    std::set<std::string> strategy_ids;
    std::vector<std::vector<ExperimentJob>> per_strategy;
    per_strategy.reserve(request.strategies.size());

    for (std::size_t i = 0; i < request.strategies.size(); ++i) {
        const ExperimentStrategy& strategy = request.strategies[i];

        if (!strategy_ids.insert(strategy.id).second) {
            fail(
                child_path(
                    index_path(strategies_path, i),
                    "id"
                ),
                "Duplicate strategy id '" + strategy.id + "'"
            );
        }

        std::vector<ExperimentJob> jobs;
        validate_and_expand_strategy(strategy, i, jobs);
        per_strategy.push_back(std::move(jobs));
    }

    // Interleave strategies round-robin so a bounded execution window always
    // contains a mix of strategies rather than draining one strategy first.
    std::vector<ExperimentJob> jobs;
    std::size_t total = 0;
    for (const auto& strategy_jobs : per_strategy) {
        total += strategy_jobs.size();
    }
    jobs.reserve(total);

    std::size_t max_jobs = 0;
    for (const auto& strategy_jobs : per_strategy) {
        max_jobs = std::max(max_jobs, strategy_jobs.size());
    }

    for (std::size_t i = 0; i < max_jobs; ++i) {
        for (const auto& strategy_jobs : per_strategy) {
            if (i < strategy_jobs.size()) {
                jobs.push_back(strategy_jobs[i]);
            }
        }
    }

    request.jobs = std::move(jobs);
}

std::unique_ptr<Strategy> create_strategy(
    const std::string& type,
    const ParameterMap& parameters
) {
    if (type == "moving_average_crossover") {
        return std::make_unique<MovingAverageCrossover>(
            static_cast<std::size_t>(parameters.at("short_window")),
            static_cast<std::size_t>(parameters.at("long_window"))
        );
    }

    if (type == "rsi_mean_reversion") {
        return std::make_unique<RsiMeanReversion>(
            static_cast<std::size_t>(parameters.at("rsi_period")),
            parameters.at("oversold_threshold"),
            parameters.at("exit_threshold")
        );
    }

    if (type == "donchian_channel_breakout") {
        return std::make_unique<DonchianChannelBreakout>(
            static_cast<std::size_t>(parameters.at("entry_period")),
            static_cast<std::size_t>(parameters.at("exit_period"))
        );
    }

    throw std::invalid_argument("Unknown strategy type '" + type + "'");
}

std::vector<std::string> supported_strategy_types() {
    std::vector<std::string> types;
    types.reserve(strategy_rules().size());

    for (const auto& rule : strategy_rules()) {
        types.push_back(rule.type);
    }

    return types;
}

ExperimentResult run_experiment(
    const ExperimentRequest& request,
    const ExperimentProgressCallback& on_started
) {
    if (request.jobs.empty()) {
        throw std::invalid_argument(
            "Experiment has no jobs; call finalize_experiment() first"
        );
    }

    const std::vector<MarketBar> dataset =
        load_market_data(request.data.path);

    if (dataset.empty()) {
        throw std::runtime_error(
            "Market data file '" + request.data.path + "' is empty"
        );
    }

    const ExperimentDataSummary data_summary{
        dataset.size(),
        dataset.front().date,
        dataset.back().date
    };

    const std::size_t hardware_threads = std::max<std::size_t>(
        1,
        std::thread::hardware_concurrency()
    );
    const std::size_t max_concurrent =
        request.execution.max_concurrent_jobs > 0
            ? request.execution.max_concurrent_jobs
            : hardware_threads;

    if (on_started) {
        on_started(data_summary, request.jobs.size(), max_concurrent);
    }

    const double cpu_start = process_cpu_seconds();
    const auto wall_start = std::chrono::steady_clock::now();

    std::vector<ExperimentJobOutcome> outcomes;
    outcomes.reserve(request.jobs.size());

    std::vector<std::future<ExperimentJobOutcome>> in_flight;
    in_flight.reserve(max_concurrent);

    std::size_t next_job = 0;

    while (next_job < request.jobs.size() || !in_flight.empty()) {
        while (next_job < request.jobs.size() &&
               in_flight.size() < max_concurrent) {
            in_flight.push_back(
                std::async(
                    std::launch::async,
                    run_job,
                    std::cref(dataset),
                    std::cref(request.data),
                    std::cref(request.execution),
                    request.jobs[next_job]
                )
            );
            ++next_job;
        }

        // Retire the oldest job before launching another one.
        outcomes.push_back(in_flight.front().get());
        in_flight.erase(in_flight.begin());
    }

    const auto wall_end = std::chrono::steady_clock::now();
    const double cpu_end = process_cpu_seconds();

    ExperimentResult result;
    result.schema_version = request.schema_version;
    result.experiment_id = request.experiment_id;
    result.data = request.data;
    result.data_summary = data_summary;
    result.execution = request.execution;
    result.output = request.output;
    result.execution.max_concurrent_jobs = max_concurrent;
    result.max_concurrent_jobs = max_concurrent;
    result.results = std::move(outcomes);
    result.summary.jobs_total = result.results.size();

    std::vector<CombinationOutcome> stat_outcomes;
    stat_outcomes.reserve(result.results.size());

    const ExperimentJobOutcome* best = nullptr;

    for (std::size_t i = 0; i < result.results.size(); ++i) {
        const ExperimentJobOutcome& outcome = result.results[i];

        stat_outcomes.push_back({
            i,
            outcome.ok,
            outcome.duration_seconds
        });

        if (!outcome.ok) {
            ++result.summary.jobs_failed;
            continue;
        }

        ++result.summary.jobs_completed;

        if (best == nullptr ||
            outcome.result.metrics.sharpe_ratio >
                best->result.metrics.sharpe_ratio ||
            (outcome.result.metrics.sharpe_ratio ==
                 best->result.metrics.sharpe_ratio &&
             outcome.result.metrics.total_return >
                 best->result.metrics.total_return)) {
            best = &outcome;
        }
    }

    if (result.summary.jobs_failed == 0) {
        result.status = "completed";
    } else if (result.summary.jobs_completed > 0) {
        result.status = "partial";
    } else {
        result.status = "failed";
    }

    const double wall_seconds =
        std::chrono::duration<double>(wall_end - wall_start).count();

    result.summary.wall_seconds = wall_seconds;
    if (best != nullptr) {
        result.summary.best_job_id = best->result.job_id;
        result.summary.best_sharpe_ratio =
            best->result.metrics.sharpe_ratio;
        result.summary.best_total_return =
            best->result.metrics.total_return;
    }

    result.summary.performance = compute_performance_stats(
        stat_outcomes,
        wall_seconds,
        cpu_end - cpu_start,
        max_concurrent
    );

    return result;
}

std::string experiment_result_to_json(
    const ExperimentResult& result,
    bool pretty
) {
    JsonObject root;
    root.emplace("schema_version", result.schema_version);
    root.emplace("status", result.status);
    root.emplace("experiment_id", result.experiment_id);

    JsonObject data;
    data.emplace("path", result.data.path);
    data.emplace("asset", result.data.asset);
    data.emplace("start_date", result.data.start_date);
    data.emplace("end_date", result.data.end_date);
    data.emplace(
        "trading_days",
        static_cast<double>(result.data_summary.trading_days)
    );
    data.emplace("first_date", result.data_summary.first_date);
    data.emplace("last_date", result.data_summary.last_date);
    root.emplace("data", JsonValue(std::move(data)));

    JsonObject execution;
    execution.emplace("initial_cash", result.execution.initial_cash);
    execution.emplace(
        "transaction_cost_rate",
        result.execution.transaction_cost_rate
    );
    execution.emplace("slippage_rate", result.execution.slippage_rate);
    execution.emplace(
        "max_concurrent_jobs",
        static_cast<double>(result.max_concurrent_jobs)
    );
    root.emplace("execution", JsonValue(std::move(execution)));

    JsonObject summary;
    summary.emplace(
        "jobs_total",
        static_cast<double>(result.summary.jobs_total)
    );
    summary.emplace(
        "jobs_completed",
        static_cast<double>(result.summary.jobs_completed)
    );
    summary.emplace(
        "jobs_failed",
        static_cast<double>(result.summary.jobs_failed)
    );
    summary.emplace("wall_seconds", result.summary.wall_seconds);
    summary.emplace("best_job_id", result.summary.best_job_id);
    summary.emplace(
        "best_sharpe_ratio",
        result.summary.best_sharpe_ratio
    );
    summary.emplace(
        "best_total_return",
        result.summary.best_total_return
    );
    summary.emplace(
        "performance",
        performance_to_json(result.summary.performance)
    );
    root.emplace("summary", JsonValue(std::move(summary)));

    JsonValue::Array results;
    results.reserve(result.results.size());

    for (const auto& outcome : result.results) {
        JsonObject entry;
        entry.emplace("job_id", outcome.job.job_id);
        entry.emplace("strategy_id", outcome.job.strategy_id);
        entry.emplace("strategy_type", outcome.job.strategy_type);
        entry.emplace("status", outcome.ok ? "completed" : "failed");
        entry.emplace("duration_seconds", outcome.duration_seconds);
        entry.emplace(
            "parameters",
            parameters_to_json(outcome.job.parameters)
        );

        if (outcome.ok) {
            entry.emplace(
                "metrics",
                metrics_to_json(outcome.result.metrics)
            );

            // Attach the full fill history only when the request opts in;
            // a large sweep can otherwise produce a very large response.
            if (result.output.include_trades) {
                JsonValue::Array trades;
                trades.reserve(outcome.result.trades.size());

                for (const auto& trade : outcome.result.trades) {
                    trades.push_back(trade_to_json(trade));
                }

                entry.emplace("trades", JsonValue(std::move(trades)));
            }
        } else {
            entry.emplace("error", outcome.error);
        }

        results.push_back(JsonValue(std::move(entry)));
    }

    root.emplace("results", JsonValue(std::move(results)));

    return serialize_json(JsonValue(std::move(root)), pretty);
}

std::string run_experiment_json(
    const std::string& request_json,
    bool pretty
) {
    try {
        const ExperimentRequest request =
            parse_experiment_json(request_json);

        const ExperimentResult result = run_experiment(request);
        return experiment_result_to_json(result, pretty);
    } catch (const JsonParseError& error) {
        return serialize_json(
            error_document(
                "invalid",
                "json_parse_error",
                "",
                error.what()
            ),
            pretty
        );
    } catch (const ExperimentValidationError& error) {
        return serialize_json(
            error_document(
                "invalid",
                "validation_error",
                error.path(),
                error.what()
            ),
            pretty
        );
    } catch (const std::exception& error) {
        return serialize_json(
            error_document(
                "failed",
                "runtime_error",
                "",
                error.what()
            ),
            pretty
        );
    }
}
