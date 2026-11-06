#include "experiment_internal.h"

#include "core/date.h"
#include "core/identifier.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
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
using experiment_detail::validate_strategy_parameters;

// -------------------------------------------------------------------
// Semantic validation helpers
// -------------------------------------------------------------------

void validate_identifier(
    const std::string& value,
    const std::string& path,
    bool allow_dots,
    const std::string& description
) {
    const bool valid = allow_dots
        ? is_dotted_identifier(value)
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
            validate_strategy_parameters(
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
        validate_strategy_parameters(
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

}  // namespace

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

    if (!is_valid_iso_date(request.data.start_date)) {
        fail(
            child_path(child_path(kRootPath, "data"), "start_date"),
            "Expected a date in YYYY-MM-DD format"
        );
    }

    if (!is_valid_iso_date(request.data.end_date)) {
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
