#include "protocol_schema.h"

#include <string>

namespace {

using protocol_detail::allow_schema_reference;
using protocol_detail::expect_object;
using protocol_detail::kRootPath;
using protocol_detail::parse_parameters;
using protocol_detail::reject_unknown_members;
using protocol_detail::require_boolean;
using protocol_detail::require_member;
using protocol_detail::require_message_type;
using protocol_detail::require_number;
using protocol_detail::require_protocol_version;
using protocol_detail::require_string;

BacktestJobData parse_data(
    const JsonValue& value,
    const std::string& path
) {
    expect_object(value, path);
    reject_unknown_members(
        value,
        {"path", "asset", "start_date", "end_date"},
        path
    );

    BacktestJobData data;
    data.path = require_string(
        require_member(value, "path", path),
        path + ".path"
    );
    data.asset = require_string(
        require_member(value, "asset", path),
        path + ".asset"
    );
    data.start_date = require_string(
        require_member(value, "start_date", path),
        path + ".start_date"
    );
    data.end_date = require_string(
        require_member(value, "end_date", path),
        path + ".end_date"
    );

    return data;
}

BacktestJobExecution parse_execution(
    const JsonValue& value,
    const std::string& path
) {
    expect_object(value, path);
    reject_unknown_members(
        value,
        {
            "initial_cash",
            "transaction_cost_rate",
            "slippage_rate"
        },
        path
    );

    BacktestJobExecution execution;

    if (value.contains("initial_cash")) {
        execution.initial_cash = require_number(
            value.at("initial_cash"),
            path + ".initial_cash"
        );
    }

    if (value.contains("transaction_cost_rate")) {
        execution.transaction_cost_rate = require_number(
            value.at("transaction_cost_rate"),
            path + ".transaction_cost_rate"
        );
    }

    if (value.contains("slippage_rate")) {
        execution.slippage_rate = require_number(
            value.at("slippage_rate"),
            path + ".slippage_rate"
        );
    }

    return execution;
}

BacktestJobOutput parse_output(
    const JsonValue& value,
    const std::string& path
) {
    expect_object(value, path);
    reject_unknown_members(value, {"include_trades"}, path);

    BacktestJobOutput output;

    if (value.contains("include_trades")) {
        output.include_trades = require_boolean(
            value.at("include_trades"),
            path + ".include_trades"
        );
    }

    return output;
}

BacktestJob parse_job(
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
            "data",
            "execution",
            "output"
        },
        path
    );

    BacktestJob job;
    job.experiment_id = require_string(
        require_member(value, "experiment_id", path),
        path + ".experiment_id"
    );
    job.job_id = require_string(
        require_member(value, "job_id", path),
        path + ".job_id"
    );
    job.strategy_id = require_string(
        require_member(value, "strategy_id", path),
        path + ".strategy_id"
    );
    job.strategy_type = require_string(
        require_member(value, "strategy_type", path),
        path + ".strategy_type"
    );
    job.parameters = parse_parameters(
        require_member(value, "parameters", path),
        path + ".parameters"
    );
    job.data = parse_data(
        require_member(value, "data", path),
        path + ".data"
    );

    if (value.contains("execution")) {
        job.execution = parse_execution(
            value.at("execution"),
            path + ".execution"
        );
    }

    if (value.contains("output")) {
        job.output = parse_output(
            value.at("output"),
            path + ".output"
        );
    }

    return job;
}

}  // namespace

namespace protocol_detail {

BacktestJobMessage parse_job_message_document(const JsonValue& root) {
    expect_object(root, kRootPath);
    allow_schema_reference(root);

    BacktestJobMessage message;
    message.protocol_version = require_protocol_version(root);
    require_message_type(root, "backtest_job");
    reject_unknown_members(
        root,
        {
            "$schema",
            "protocol_version",
            "type",
            "message_id",
            "job"
        },
        kRootPath
    );
    message.message_id = require_string(
        require_member(root, "message_id", kRootPath),
        std::string(kRootPath) + ".message_id"
    );
    message.job = parse_job(
        require_member(root, "job", kRootPath),
        std::string(kRootPath) + ".job"
    );

    validate_job_message(message);

    return message;
}

}  // namespace protocol_detail

BacktestJobMessage parse_backtest_job_json(const std::string& text) {
    return protocol_detail::parse_job_message_document(parse_json(text));
}
