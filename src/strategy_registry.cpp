#include "experiment_internal.h"

#include "donchian_channel_breakout.h"
#include "moving_average_crossover.h"
#include "rsi_mean_reversion.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using experiment_detail::child_path;
using experiment_detail::fail;

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

}  // namespace

namespace experiment_detail {

void validate_strategy_parameters(
    const std::string& type,
    const ParameterMap& parameters,
    const std::string& path
) {
    validate_parameters(type, parameters, path);
}

}  // namespace experiment_detail

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
