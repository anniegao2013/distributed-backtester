#include "protocol_internal.h"

#include "core/date.h"
#include "core/identifier.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace {

using protocol_detail::fail;
using protocol_detail::kMaxIdentifierLength;

std::string child_path(
    const std::string& parent,
    const std::string& key
) {
    return parent + "." + key;
}

std::string index_path(
    const std::string& parent,
    std::size_t index
) {
    return parent + "[" + std::to_string(index) + "]";
}

bool is_opaque_id_char(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-' || c == ':';
}

void validate_opaque_id(
    const std::string& value,
    const std::string& path,
    const std::string& label
) {
    if (value.empty()) {
        fail(path, label + " must not be empty");
    }

    if (value.size() > kMaxIdentifierLength) {
        fail(
            path,
            label + " must be at most " +
                std::to_string(kMaxIdentifierLength) + " characters"
        );
    }

    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];

        if (!is_opaque_id_char(c)) {
            fail(
                path,
                label + " may contain only letters, digits, "
                "underscores, hyphens, dots, and colons"
            );
        }

        const bool first = i == 0;
        const bool alphanumeric =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9');

        if (first && !alphanumeric) {
            fail(path, label + " must start with a letter or digit");
        }
    }
}

void validate_identifier_field(
    const std::string& value,
    const std::string& path,
    bool allow_dots,
    const std::string& label
) {
    if (value.empty()) {
        fail(path, label + " must not be empty");
    }

    if (value.size() > kMaxIdentifierLength) {
        fail(
            path,
            label + " must be at most " +
                std::to_string(kMaxIdentifierLength) + " characters"
        );
    }

    const bool valid = allow_dots
        ? is_dotted_identifier(value)
        : is_identifier(value);

    if (!valid) {
        fail(
            path,
            label +
                " must start with a letter or digit and contain only "
                "letters, digits, underscores, and hyphens" +
                std::string(allow_dots ? " (dots are also allowed)" : "")
        );
    }
}

void validate_parameters(
    const ParameterMap& parameters,
    const std::string& path
) {
    if (parameters.empty()) {
        fail(path, "At least one strategy parameter is required");
    }

    for (const auto& [name, value] : parameters) {
        const std::string parameter_path = child_path(path, name);

        if (name.empty()) {
            fail(path, "Parameter names must not be empty");
        }

        if (name.size() > kMaxIdentifierLength) {
            fail(
                parameter_path,
                "Parameter name must be at most " +
                    std::to_string(kMaxIdentifierLength) + " characters"
            );
        }

        if (!std::isfinite(value)) {
            fail(parameter_path, "Expected a finite number");
        }
    }
}

void validate_data(
    const BacktestJobData& data,
    const std::string& path
) {
    const std::string path_path = child_path(path, "path");
    const std::string asset_path = child_path(path, "asset");
    const std::string start_path = child_path(path, "start_date");
    const std::string end_path = child_path(path, "end_date");

    if (data.path.empty()) {
        fail(path_path, "Data path must not be empty");
    }

    if (data.asset.empty()) {
        fail(asset_path, "Asset must not be empty");
    }

    if (!is_valid_iso_date(data.start_date)) {
        fail(start_path, "Expected a date in YYYY-MM-DD format");
    }

    if (!is_valid_iso_date(data.end_date)) {
        fail(end_path, "Expected a date in YYYY-MM-DD format");
    }

    if (data.start_date > data.end_date) {
        fail(start_path, "start_date must not be after end_date");
    }
}

void validate_execution(
    const BacktestJobExecution& execution,
    const std::string& path
) {
    const std::string cash_path = child_path(path, "initial_cash");
    const std::string cost_path =
        child_path(path, "transaction_cost_rate");
    const std::string slippage_path =
        child_path(path, "slippage_rate");

    if (!std::isfinite(execution.initial_cash) ||
        execution.initial_cash <= 0.0) {
        fail(cash_path, "initial_cash must be positive and finite");
    }

    if (!std::isfinite(execution.transaction_cost_rate) ||
        execution.transaction_cost_rate < 0.0 ||
        execution.transaction_cost_rate >= 1.0) {
        fail(cost_path, "transaction_cost_rate must be in [0, 1)");
    }

    if (!std::isfinite(execution.slippage_rate) ||
        execution.slippage_rate < 0.0 ||
        execution.slippage_rate >= 1.0) {
        fail(slippage_path, "slippage_rate must be in [0, 1)");
    }
}

void validate_identity(
    const std::string& experiment_id,
    const std::string& job_id,
    const std::string& strategy_id,
    const std::string& strategy_type,
    const ParameterMap& parameters,
    const std::string& path
) {
    validate_identifier_field(
        experiment_id,
        child_path(path, "experiment_id"),
        true,
        "Experiment id"
    );
    validate_identifier_field(
        job_id,
        child_path(path, "job_id"),
        false,
        "Job id"
    );
    validate_identifier_field(
        strategy_id,
        child_path(path, "strategy_id"),
        false,
        "Strategy id"
    );

    const std::string type_path = child_path(path, "strategy_type");
    if (strategy_type.empty()) {
        fail(type_path, "Strategy type must not be empty");
    }
    if (strategy_type.size() > kMaxIdentifierLength) {
        fail(
            type_path,
            "Strategy type must be at most " +
                std::to_string(kMaxIdentifierLength) + " characters"
        );
    }

    validate_parameters(parameters, child_path(path, "parameters"));
}

void validate_metrics(
    const BacktestMetrics& metrics,
    const std::string& path
) {
    const auto require_finite = [&path](double value, const char* field) {
        if (!std::isfinite(value)) {
            fail(
                child_path(path, field),
                "Expected a finite number"
            );
        }
    };

    require_finite(metrics.total_return, "total_return");
    require_finite(metrics.sharpe_ratio, "sharpe_ratio");
    require_finite(metrics.max_drawdown, "max_drawdown");
    require_finite(metrics.total_costs, "total_costs");
}

void validate_trades(
    const std::vector<Trade>& trades,
    const std::string& path
) {
    for (std::size_t i = 0; i < trades.size(); ++i) {
        const Trade& trade = trades[i];
        const std::string trade_path = index_path(path, i);

        if (trade.symbol.empty()) {
            fail(
                child_path(trade_path, "symbol"),
                "Trade symbol must not be empty"
            );
        }

        if (trade.side != OrderSide::Buy &&
            trade.side != OrderSide::Sell) {
            fail(
                child_path(trade_path, "side"),
                "Expected 'buy' or 'sell'"
            );
        }

        if (!std::isfinite(trade.quantity) || trade.quantity <= 0.0) {
            fail(
                child_path(trade_path, "quantity"),
                "quantity must be positive and finite"
            );
        }

        if (!std::isfinite(trade.fill_price) ||
            trade.fill_price <= 0.0) {
            fail(
                child_path(trade_path, "fill_price"),
                "fill_price must be positive and finite"
            );
        }

        if (!std::isfinite(trade.commission) ||
            trade.commission < 0.0) {
            fail(
                child_path(trade_path, "commission"),
                "commission must be non-negative and finite"
            );
        }

        if (!std::isfinite(trade.slippage_cost) ||
            trade.slippage_cost < 0.0) {
            fail(
                child_path(trade_path, "slippage_cost"),
                "slippage_cost must be non-negative and finite"
            );
        }
    }
}

void validate_result_error(
    const BacktestResultError& error,
    const std::string& path
) {
    const std::string type_path = child_path(path, "type");
    const std::string message_path = child_path(path, "message");

    if (error.type != "validation_error" &&
        error.type != "runtime_error") {
        fail(
            type_path,
            "Expected 'validation_error' or 'runtime_error'"
        );
    }

    if (error.message.empty()) {
        fail(message_path, "Error message must not be empty");
    }
}

}  // namespace

namespace protocol_detail {

void fail(
    const std::string& path,
    const std::string& message
) {
    throw ProtocolValidationError(path, message);
}

void validate_message_id(
    const std::string& value,
    const std::string& path
) {
    validate_opaque_id(value, path, "message_id");
}

void validate_worker_id(
    const std::string& value,
    const std::string& path
) {
    validate_opaque_id(value, path, "worker_id");
}

void validate_job_message(const BacktestJobMessage& message) {
    const std::string root = kRootPath;

    if (message.protocol_version != kBacktestProtocolVersion) {
        fail(
            child_path(root, "protocol_version"),
            "Unsupported protocol_version; this build speaks version " +
                std::to_string(kBacktestProtocolVersion)
        );
    }

    validate_message_id(
        message.message_id,
        child_path(root, "message_id")
    );

    const std::string job_path = child_path(root, "job");
    validate_identity(
        message.job.experiment_id,
        message.job.job_id,
        message.job.strategy_id,
        message.job.strategy_type,
        message.job.parameters,
        job_path
    );
    validate_data(message.job.data, child_path(job_path, "data"));
    validate_execution(
        message.job.execution,
        child_path(job_path, "execution")
    );
}

void validate_result_message(const BacktestResultMessage& message) {
    const std::string root = kRootPath;

    if (message.protocol_version != kBacktestProtocolVersion) {
        fail(
            child_path(root, "protocol_version"),
            "Unsupported protocol_version; this build speaks version " +
                std::to_string(kBacktestProtocolVersion)
        );
    }

    validate_message_id(
        message.message_id,
        child_path(root, "message_id")
    );

    if (!message.worker_id.empty()) {
        validate_worker_id(
            message.worker_id,
            child_path(root, "worker_id")
        );
    }

    const std::string result_path = child_path(root, "result");
    const BacktestResult& result = message.result;

    validate_identity(
        result.experiment_id,
        result.job_id,
        result.strategy_id,
        result.strategy_type,
        result.parameters,
        result_path
    );

    const std::string status_path = child_path(result_path, "status");
    if (result.status != "completed" &&
        result.status != "failed" &&
        result.status != "invalid") {
        fail(
            status_path,
            "Expected 'completed', 'failed', or 'invalid'"
        );
    }

    const std::string duration_path =
        child_path(result_path, "duration_seconds");
    if (!std::isfinite(result.duration_seconds) ||
        result.duration_seconds < 0.0) {
        fail(
            duration_path,
            "duration_seconds must be non-negative and finite"
        );
    }

    if (result.metrics.has_value()) {
        validate_metrics(
            *result.metrics,
            child_path(result_path, "metrics")
        );
    }

    if (result.trades.has_value()) {
        validate_trades(
            *result.trades,
            child_path(result_path, "trades")
        );
    }

    if (result.error.has_value()) {
        validate_result_error(
            *result.error,
            child_path(result_path, "error")
        );
    }

    if (result.status == "completed") {
        if (!result.metrics.has_value()) {
            fail(
                result_path,
                "A completed result must include 'metrics'"
            );
        }

        if (result.error.has_value()) {
            fail(
                child_path(result_path, "error"),
                "A completed result must not include 'error'"
            );
        }

        return;
    }

    if (!result.error.has_value()) {
        fail(
            result_path,
            "A " + result.status + " result must include 'error'"
        );
    }

    if (result.metrics.has_value()) {
        fail(
            child_path(result_path, "metrics"),
            "Only a completed result may include 'metrics'"
        );
    }

    if (result.trades.has_value()) {
        fail(
            child_path(result_path, "trades"),
            "Only a completed result may include 'trades'"
        );
    }

    const std::string expected_type =
        result.status == "invalid"
            ? "validation_error"
            : "runtime_error";

    if (result.error->type != expected_type) {
        fail(
            child_path(child_path(result_path, "error"), "type"),
            "A " + result.status + " result must use error type '" +
                expected_type + "'"
        );
    }
}

void validate_error_message(const ProtocolErrorMessage& message) {
    const std::string root = kRootPath;

    if (message.protocol_version != kBacktestProtocolVersion) {
        fail(
            child_path(root, "protocol_version"),
            "Unsupported protocol_version; this build speaks version " +
                std::to_string(kBacktestProtocolVersion)
        );
    }

    if (!message.message_id.empty()) {
        validate_message_id(
            message.message_id,
            child_path(root, "message_id")
        );
    }

    const std::string error_path = child_path(root, "error");
    const std::string type_path = child_path(error_path, "type");
    const std::string message_path = child_path(error_path, "message");

    if (message.error.type != "json_parse_error" &&
        message.error.type != "unsupported_version" &&
        message.error.type != "unknown_message_type" &&
        message.error.type != "validation_error") {
        fail(
            type_path,
            "Expected 'json_parse_error', 'unsupported_version', "
            "'unknown_message_type', or 'validation_error'"
        );
    }

    if (message.error.message.empty()) {
        fail(message_path, "Error message must not be empty");
    }
}

}  // namespace protocol_detail

ProtocolValidationError::ProtocolValidationError(
    std::string path,
    std::string message
)
    : std::runtime_error(std::move(message)),
      path_(std::move(path)) {}

const std::string& ProtocolValidationError::path() const noexcept {
    return path_;
}
