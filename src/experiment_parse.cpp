#include "experiment_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using experiment_detail::child_path;
using experiment_detail::fail;
using experiment_detail::index_path;
using experiment_detail::kMaxConcurrentJobs;
using experiment_detail::kRootPath;
using experiment_detail::kSchemaVersion;

// -------------------------------------------------------------------
// Schema helpers
// -------------------------------------------------------------------

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

}  // namespace

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
