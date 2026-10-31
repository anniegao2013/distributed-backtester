#include "protocol_internal.h"

#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using JsonObject = JsonValue::Object;

namespace {

JsonValue parameters_to_json(const ParameterMap& parameters) {
    JsonObject object;

    for (const auto& [name, value] : parameters) {
        object.emplace(name, value);
    }

    return JsonValue(std::move(object));
}

JsonValue data_to_json(const BacktestJobData& data) {
    JsonObject object;
    object.emplace("path", data.path);
    object.emplace("asset", data.asset);
    object.emplace("start_date", data.start_date);
    object.emplace("end_date", data.end_date);

    return JsonValue(std::move(object));
}

JsonValue execution_to_json(const BacktestJobExecution& execution) {
    JsonObject object;
    object.emplace("initial_cash", execution.initial_cash);
    object.emplace(
        "transaction_cost_rate",
        execution.transaction_cost_rate
    );
    object.emplace("slippage_rate", execution.slippage_rate);

    return JsonValue(std::move(object));
}

JsonValue output_to_json(const BacktestJobOutput& output) {
    JsonObject object;
    object.emplace("include_trades", output.include_trades);

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

JsonValue result_error_to_json(const BacktestResultError& error) {
    JsonObject object;
    object.emplace("type", error.type);

    if (!error.path.empty()) {
        object.emplace("path", error.path);
    }

    object.emplace("message", error.message);

    return JsonValue(std::move(object));
}

JsonValue job_to_json(const BacktestJob& job) {
    JsonObject object;
    object.emplace("experiment_id", job.experiment_id);
    object.emplace("job_id", job.job_id);
    object.emplace("strategy_id", job.strategy_id);
    object.emplace("strategy_type", job.strategy_type);
    object.emplace(
        "parameters",
        parameters_to_json(job.parameters)
    );
    object.emplace("data", data_to_json(job.data));
    object.emplace("execution", execution_to_json(job.execution));
    object.emplace("output", output_to_json(job.output));

    return JsonValue(std::move(object));
}

JsonValue result_to_json(const BacktestResult& result) {
    JsonObject object;
    object.emplace("experiment_id", result.experiment_id);
    object.emplace("job_id", result.job_id);
    object.emplace("strategy_id", result.strategy_id);
    object.emplace("strategy_type", result.strategy_type);
    object.emplace(
        "parameters",
        parameters_to_json(result.parameters)
    );
    object.emplace("status", result.status);
    object.emplace("duration_seconds", result.duration_seconds);

    if (result.metrics.has_value()) {
        object.emplace(
            "metrics",
            metrics_to_json(*result.metrics)
        );
    }

    if (result.trades.has_value()) {
        JsonValue::Array trades;
        trades.reserve(result.trades->size());

        for (const auto& trade : *result.trades) {
            trades.push_back(trade_to_json(trade));
        }

        object.emplace("trades", JsonValue(std::move(trades)));
    }

    if (result.error.has_value()) {
        object.emplace(
            "error",
            result_error_to_json(*result.error)
        );
    }

    return JsonValue(std::move(object));
}

JsonValue protocol_error_detail_to_json(
    const ProtocolErrorDetail& error
) {
    JsonObject object;
    object.emplace("type", error.type);

    if (!error.path.empty()) {
        object.emplace("path", error.path);
    }

    object.emplace("message", error.message);

    return JsonValue(std::move(object));
}

}  // namespace

std::string backtest_job_to_json(
    const BacktestJobMessage& message,
    bool pretty
) {
    protocol_detail::validate_job_message(message);

    JsonObject root;
    root.emplace("protocol_version", message.protocol_version);
    root.emplace("type", "backtest_job");
    root.emplace("message_id", message.message_id);
    root.emplace("job", job_to_json(message.job));

    return serialize_json(JsonValue(std::move(root)), pretty);
}

std::string backtest_result_to_json(
    const BacktestResultMessage& message,
    bool pretty
) {
    protocol_detail::validate_result_message(message);

    JsonObject root;
    root.emplace("protocol_version", message.protocol_version);
    root.emplace("type", "backtest_result");
    root.emplace("message_id", message.message_id);

    if (!message.worker_id.empty()) {
        root.emplace("worker_id", message.worker_id);
    }

    root.emplace("result", result_to_json(message.result));

    return serialize_json(JsonValue(std::move(root)), pretty);
}

std::string protocol_error_to_json(
    const ProtocolErrorMessage& message,
    bool pretty
) {
    protocol_detail::validate_error_message(message);

    JsonObject root;
    root.emplace("protocol_version", message.protocol_version);
    root.emplace("type", "protocol_error");

    if (!message.message_id.empty()) {
        root.emplace("message_id", message.message_id);
    }

    root.emplace(
        "error",
        protocol_error_detail_to_json(message.error)
    );

    return serialize_json(JsonValue(std::move(root)), pretty);
}

std::string protocol_message_to_json(
    const ProtocolMessage& message,
    bool pretty
) {
    if (const auto* job = std::get_if<BacktestJobMessage>(&message)) {
        return backtest_job_to_json(*job, pretty);
    }

    if (const auto* result =
            std::get_if<BacktestResultMessage>(&message)) {
        return backtest_result_to_json(*result, pretty);
    }

    const auto& error = std::get<ProtocolErrorMessage>(message);
    return protocol_error_to_json(error, pretty);
}

ProtocolErrorMessage make_protocol_error(
    std::string type,
    std::string message,
    std::string path,
    std::string message_id
) {
    ProtocolErrorMessage error_message;
    error_message.message_id = std::move(message_id);
    error_message.error.type = std::move(type);
    error_message.error.path = std::move(path);
    error_message.error.message = std::move(message);

    return error_message;
}
