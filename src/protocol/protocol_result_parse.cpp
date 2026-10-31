#include "protocol_schema.h"

#include <cstddef>
#include <string>
#include <vector>

namespace {

using protocol_detail::allow_schema_reference;
using protocol_detail::expect_array;
using protocol_detail::expect_object;
using protocol_detail::fail;
using protocol_detail::kRootPath;
using protocol_detail::parse_parameters;
using protocol_detail::reject_unknown_members;
using protocol_detail::require_member;
using protocol_detail::require_message_type;
using protocol_detail::require_non_negative_integer;
using protocol_detail::require_number;
using protocol_detail::require_protocol_version;
using protocol_detail::require_string;

BacktestMetrics parse_metrics(
    const JsonValue& value,
    const std::string& path
) {
    expect_object(value, path);
    reject_unknown_members(
        value,
        {
            "total_return",
            "sharpe_ratio",
            "max_drawdown",
            "number_of_trades",
            "total_costs"
        },
        path
    );

    BacktestMetrics metrics;
    metrics.total_return = require_number(
        require_member(value, "total_return", path),
        path + ".total_return"
    );
    metrics.sharpe_ratio = require_number(
        require_member(value, "sharpe_ratio", path),
        path + ".sharpe_ratio"
    );
    metrics.max_drawdown = require_number(
        require_member(value, "max_drawdown", path),
        path + ".max_drawdown"
    );
    metrics.number_of_trades = require_non_negative_integer(
        require_member(value, "number_of_trades", path),
        path + ".number_of_trades"
    );
    metrics.total_costs = require_number(
        require_member(value, "total_costs", path),
        path + ".total_costs"
    );

    return metrics;
}

Trade parse_trade(const JsonValue& value, const std::string& path) {
    expect_object(value, path);
    reject_unknown_members(
        value,
        {
            "symbol",
            "side",
            "quantity",
            "fill_price",
            "commission",
            "slippage_cost"
        },
        path
    );

    Trade trade;
    trade.symbol = require_string(
        require_member(value, "symbol", path),
        path + ".symbol"
    );

    const std::string side = require_string(
        require_member(value, "side", path),
        path + ".side"
    );

    if (side == "buy") {
        trade.side = OrderSide::Buy;
    } else if (side == "sell") {
        trade.side = OrderSide::Sell;
    } else {
        fail(path + ".side", "Expected 'buy' or 'sell'");
    }

    trade.quantity = require_number(
        require_member(value, "quantity", path),
        path + ".quantity"
    );
    trade.fill_price = require_number(
        require_member(value, "fill_price", path),
        path + ".fill_price"
    );
    trade.commission = require_number(
        require_member(value, "commission", path),
        path + ".commission"
    );
    trade.slippage_cost = require_number(
        require_member(value, "slippage_cost", path),
        path + ".slippage_cost"
    );

    return trade;
}

std::vector<Trade> parse_trades(
    const JsonValue& value,
    const std::string& path
) {
    expect_array(value, path);

    std::vector<Trade> trades;
    trades.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
        trades.push_back(
            parse_trade(
                value.at(i),
                path + "[" + std::to_string(i) + "]"
            )
        );
    }

    return trades;
}

BacktestResultError parse_result_error(
    const JsonValue& value,
    const std::string& path
) {
    expect_object(value, path);
    reject_unknown_members(value, {"type", "path", "message"}, path);

    BacktestResultError error;
    error.type = require_string(
        require_member(value, "type", path),
        path + ".type"
    );
    error.message = require_string(
        require_member(value, "message", path),
        path + ".message"
    );

    if (value.contains("path")) {
        error.path = require_string(
            value.at("path"),
            path + ".path"
        );
    }

    return error;
}

BacktestResult parse_result(
    const JsonValue& value,
    const std::string& path
) {
    expect_object(value, path);
    reject_unknown_members(
        value,
        {
            "experiment_id",
            "job_id",
            "strategy_id",
            "strategy_type",
            "parameters",
            "status",
            "duration_seconds",
            "metrics",
            "trades",
            "error"
        },
        path
    );

    BacktestResult result;
    result.experiment_id = require_string(
        require_member(value, "experiment_id", path),
        path + ".experiment_id"
    );
    result.job_id = require_string(
        require_member(value, "job_id", path),
        path + ".job_id"
    );
    result.strategy_id = require_string(
        require_member(value, "strategy_id", path),
        path + ".strategy_id"
    );
    result.strategy_type = require_string(
        require_member(value, "strategy_type", path),
        path + ".strategy_type"
    );
    result.parameters = parse_parameters(
        require_member(value, "parameters", path),
        path + ".parameters"
    );
    result.status = require_string(
        require_member(value, "status", path),
        path + ".status"
    );
    result.duration_seconds = require_number(
        require_member(value, "duration_seconds", path),
        path + ".duration_seconds"
    );

    if (value.contains("metrics")) {
        result.metrics = parse_metrics(
            value.at("metrics"),
            path + ".metrics"
        );
    }

    if (value.contains("trades")) {
        result.trades = parse_trades(
            value.at("trades"),
            path + ".trades"
        );
    }

    if (value.contains("error")) {
        result.error = parse_result_error(
            value.at("error"),
            path + ".error"
        );
    }

    return result;
}

}  // namespace

namespace protocol_detail {

BacktestResultMessage parse_result_message_document(
    const JsonValue& root
) {
    expect_object(root, kRootPath);
    allow_schema_reference(root);

    BacktestResultMessage message;
    message.protocol_version = require_protocol_version(root);
    require_message_type(root, "backtest_result");
    reject_unknown_members(
        root,
        {
            "$schema",
            "protocol_version",
            "type",
            "message_id",
            "worker_id",
            "result"
        },
        kRootPath
    );
    message.message_id = require_string(
        require_member(root, "message_id", kRootPath),
        std::string(kRootPath) + ".message_id"
    );

    if (root.contains("worker_id")) {
        message.worker_id = require_string(
            root.at("worker_id"),
            std::string(kRootPath) + ".worker_id"
        );

        if (message.worker_id.empty()) {
            fail(
                std::string(kRootPath) + ".worker_id",
                "worker_id must not be empty when present"
            );
        }
    }

    message.result = parse_result(
        require_member(root, "result", kRootPath),
        std::string(kRootPath) + ".result"
    );

    validate_result_message(message);

    return message;
}

ProtocolErrorMessage parse_error_message_document(const JsonValue& root) {
    expect_object(root, kRootPath);
    allow_schema_reference(root);

    ProtocolErrorMessage message;
    message.protocol_version = require_protocol_version(root);
    require_message_type(root, "protocol_error");
    reject_unknown_members(
        root,
        {
            "$schema",
            "protocol_version",
            "type",
            "message_id",
            "error"
        },
        kRootPath
    );

    if (root.contains("message_id")) {
        message.message_id = require_string(
            root.at("message_id"),
            std::string(kRootPath) + ".message_id"
        );

        if (message.message_id.empty()) {
            fail(
                std::string(kRootPath) + ".message_id",
                "message_id must not be empty when present"
            );
        }
    }

    const std::string error_path =
        std::string(kRootPath) + ".error";
    const BacktestResultError error = parse_result_error(
        require_member(root, "error", kRootPath),
        error_path
    );

    message.error.type = error.type;
    message.error.path = error.path;
    message.error.message = error.message;

    validate_error_message(message);

    return message;
}

}  // namespace protocol_detail

BacktestResultMessage parse_backtest_result_json(const std::string& text) {
    return protocol_detail::parse_result_message_document(parse_json(text));
}

ProtocolErrorMessage parse_protocol_error_json(const std::string& text) {
    return protocol_detail::parse_error_message_document(parse_json(text));
}
