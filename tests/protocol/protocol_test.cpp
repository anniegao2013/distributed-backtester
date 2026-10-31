#include "core/json_value.h"
#include "protocol/protocol.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

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

std::string replace_once(
    std::string text,
    const std::string& from,
    const std::string& to
) {
    const std::size_t position = text.find(from);

    if (position == std::string::npos) {
        throw std::runtime_error("Fixture does not contain: " + from);
    }

    text.replace(position, from.size(), to);
    return text;
}

std::string job_document() {
    return R"({
  "protocol_version": 1,
  "type": "backtest_job",
  "message_id": "run-1:job-1",
  "job": {
    "experiment_id": "sweep.v1",
    "job_id": "ma-1",
    "strategy_id": "ma",
    "strategy_type": "moving_average_crossover",
    "parameters": {
      "short_window": 20,
      "long_window": 100
    },
    "data": {
      "path": "data/market/sp500_1950_2018.csv",
      "asset": "SP500",
      "start_date": "2000-01-01",
      "end_date": "2010-12-31"
    },
    "execution": {
      "initial_cash": 10000.0,
      "transaction_cost_rate": 0.001,
      "slippage_rate": 0.0005
    },
    "output": {
      "include_trades": false
    }
  }
})";
}

std::string result_document() {
    return R"({
  "protocol_version": 1,
  "type": "backtest_result",
  "message_id": "run-1:job-1",
  "worker_id": "worker-a",
  "result": {
    "experiment_id": "sweep.v1",
    "job_id": "ma-1",
    "strategy_id": "ma",
    "strategy_type": "moving_average_crossover",
    "parameters": {
      "short_window": 20,
      "long_window": 100
    },
    "status": "completed",
    "duration_seconds": 0.05,
    "metrics": {
      "total_return": 0.1241,
      "sharpe_ratio": 0.1515,
      "max_drawdown": 0.2917,
      "number_of_trades": 18,
      "total_costs": 483.99
    },
    "trades": [
      {
        "symbol": "SP500",
        "side": "buy",
        "quantity": 4.2,
        "fill_price": 100.5,
        "commission": 0.42,
        "slippage_cost": 0.21
      },
      {
        "symbol": "SP500",
        "side": "sell",
        "quantity": 4.2,
        "fill_price": 112.0,
        "commission": 0.47,
        "slippage_cost": 0.24
      }
    ]
  }
})";
}

std::string failed_result_document() {
    return R"({
  "protocol_version": 1,
  "type": "backtest_result",
  "message_id": "run-1:job-1",
  "worker_id": "worker-a",
  "result": {
    "experiment_id": "sweep.v1",
    "job_id": "ma-1",
    "strategy_id": "ma",
    "strategy_type": "moving_average_crossover",
    "parameters": {
      "short_window": 20,
      "long_window": 100
    },
    "status": "failed",
    "duration_seconds": 0.05,
    "error": {
      "type": "runtime_error",
      "message": "market data file not found"
    }
  }
})";
}

std::string invalid_result_document() {
    return R"({
  "protocol_version": 1,
  "type": "backtest_result",
  "message_id": "run-1:job-1",
  "result": {
    "experiment_id": "sweep.v1",
    "job_id": "ma-1",
    "strategy_id": "ma",
    "strategy_type": "moving_average_crossover",
    "parameters": {
      "short_window": 20,
      "long_window": 100
    },
    "status": "invalid",
    "duration_seconds": 0.0,
    "error": {
      "type": "validation_error",
      "path": "$.job.parameters.short_window",
      "message": "Expected a positive integer"
    }
  }
})";
}

std::string error_document() {
    return R"({
  "protocol_version": 1,
  "type": "protocol_error",
  "message_id": "run-1:job-1",
  "error": {
    "type": "unsupported_version",
    "path": "$.protocol_version",
    "message": "Unsupported protocol_version; this build speaks version 1"
  }
})";
}

void expect_validation_with(
    const std::function<void(const std::string&)>& parse,
    const std::string& text,
    const std::string& expected_path,
    const std::string& expected_message
) {
    try {
        parse(text);
    } catch (const ProtocolValidationError& error) {
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

void expect_validation(
    const std::string& text,
    const std::string& expected_path,
    const std::string& expected_message
) {
    expect_validation_with(
        [](const std::string& document) {
            (void)parse_protocol_message_json(document);
        },
        text,
        expected_path,
        expected_message
    );
}

void test_job_parses_with_defaults() {
    const std::string document = replace_once(
        replace_once(
            job_document(),
            R"("execution": {
      "initial_cash": 10000.0,
      "transaction_cost_rate": 0.001,
      "slippage_rate": 0.0005
    },)",
            ""
        ),
        R"("output": {
      "include_trades": false
    })",
        R"("output": {})"
    );

    const BacktestJobMessage message = parse_backtest_job_json(document);

    expect(message.protocol_version == 1, "protocol version");
    expect(message.message_id == "run-1:job-1", "message id");
    expect(message.job.experiment_id == "sweep.v1", "experiment id");
    expect(message.job.job_id == "ma-1", "job id");
    expect(message.job.strategy_id == "ma", "strategy id");
    expect(
        message.job.strategy_type == "moving_average_crossover",
        "strategy type"
    );
    expect(
        message.job.parameters.at("short_window") == 20.0 &&
            message.job.parameters.at("long_window") == 100.0,
        "parameters"
    );
    expect(message.job.data.asset == "SP500", "asset");
    expect(message.job.execution.initial_cash == 10000.0, "default cash");
    expect(
        message.job.execution.transaction_cost_rate == 0.0,
        "default transaction costs"
    );
    expect(message.job.execution.slippage_rate == 0.0, "default slippage");
    expect(!message.job.output.include_trades, "trades excluded by default");

    const ProtocolMessage dispatched =
        parse_protocol_message_json(job_document());
    expect(
        std::holds_alternative<BacktestJobMessage>(dispatched),
        "dispatch should select a job"
    );
}

void test_job_round_trip() {
    const BacktestJobMessage message = parse_backtest_job_json(job_document());
    const std::string serialized = backtest_job_to_json(message, false);
    const BacktestJobMessage reparsed = parse_backtest_job_json(serialized);

    expect(
        serialized.find("\"type\":\"backtest_job\"") != std::string::npos,
        "serialized type"
    );
    expect(
        reparsed.message_id == message.message_id,
        "message id round-trip"
    );
    expect(reparsed.job.job_id == message.job.job_id, "job id round-trip");
    expect(
        reparsed.job.data.path == message.job.data.path &&
            reparsed.job.data.asset == message.job.data.asset &&
            reparsed.job.data.start_date == message.job.data.start_date &&
            reparsed.job.data.end_date == message.job.data.end_date,
        "data round-trip"
    );
    expect(
        reparsed.job.execution.initial_cash ==
            message.job.execution.initial_cash,
        "execution round-trip"
    );
    expect(
        reparsed.job.parameters == message.job.parameters,
        "parameters round-trip"
    );
    expect(
        backtest_job_to_json(reparsed, false) == serialized,
        "serialization should be stable"
    );
}

void test_job_rejects_unknown_fields() {
    expect_validation(
        replace_once(
            job_document(),
            R"("message_id": "run-1:job-1",)",
            R"("message_id": "run-1:job-1", "extra": 1,)"
        ),
        "$.extra",
        "Unknown field"
    );

    expect_validation(
        replace_once(
            job_document(),
            R"("strategy_type": "moving_average_crossover",)",
            R"("strategy_type": "moving_average_crossover", "extra": 1,)"
        ),
        "$.job.extra",
        "Unknown field"
    );

    expect_validation(
        replace_once(
            job_document(),
            R"("slippage_rate": 0.0005)",
            R"("slippage_rate": 0.0005, "extra": 1)"
        ),
        "$.job.execution.extra",
        "Unknown field"
    );
}

void test_job_rejects_bad_envelope() {
    expect_validation(
        replace_once(job_document(), R"("protocol_version": 1)", R"("protocol_version": 2)"),
        "$.protocol_version",
        "Unsupported protocol_version"
    );

    expect_validation_with(
        [](const std::string& document) {
            (void)parse_backtest_job_json(document);
        },
        replace_once(
            job_document(),
            R"("type": "backtest_job")",
            R"("type": "backtest_result")"
        ),
        "$.type",
        "Expected message type 'backtest_job'"
    );

    expect_validation(
        replace_once(
            job_document(),
            R"("message_id": "run-1:job-1",)",
            ""
        ),
        "$",
        "Missing required field 'message_id'"
    );

    expect_validation(
        replace_once(
            job_document(),
            R"("message_id": "run-1:job-1")",
            R"("message_id": "bad id")"
        ),
        "$.message_id",
        "may contain only"
    );

    expect_validation(
        replace_once(
            job_document(),
            R"("message_id": "run-1:job-1")",
            R"("message_id": "-leading")"
        ),
        "$.message_id",
        "must start with a letter or digit"
    );
}

void test_job_rejects_bad_data() {
    expect_validation(
        replace_once(
            job_document(),
            R"("start_date": "2000-01-01")",
            R"("start_date": "2000-13-01")"
        ),
        "$.job.data.start_date",
        "YYYY-MM-DD"
    );

    expect_validation(
        replace_once(
            job_document(),
            R"("start_date": "2000-01-01",)",
            R"("start_date": "2011-01-01",)"
        ),
        "$.job.data.start_date",
        "must not be after"
    );

    expect_validation(
        replace_once(
            job_document(),
            R"("path": "data/market/sp500_1950_2018.csv")",
            R"("path": "")"
        ),
        "$.job.data.path",
        "must not be empty"
    );

    expect_validation(
        replace_once(
            job_document(),
            R"("asset": "SP500")",
            R"("asset": "")"
        ),
        "$.job.data.asset",
        "must not be empty"
    );
}

void test_job_rejects_bad_execution() {
    expect_validation(
        replace_once(
            job_document(),
            R"("initial_cash": 10000.0)",
            R"("initial_cash": 0)"
        ),
        "$.job.execution.initial_cash",
        "must be positive"
    );

    expect_validation(
        replace_once(
            job_document(),
            R"("transaction_cost_rate": 0.001)",
            R"("transaction_cost_rate": 1.0)"
        ),
        "$.job.execution.transaction_cost_rate",
        "[0, 1)"
    );

    expect_validation(
        replace_once(
            job_document(),
            R"("slippage_rate": 0.0005)",
            R"("slippage_rate": -0.1)"
        ),
        "$.job.execution.slippage_rate",
        "[0, 1)"
    );
}

void test_job_rejects_bad_parameters() {
    expect_validation(
        replace_once(
            job_document(),
            R"("parameters": {
      "short_window": 20,
      "long_window": 100
    })",
            R"("parameters": {})"
        ),
        "$.job.parameters",
        "At least one strategy parameter"
    );

    expect_validation(
        replace_once(
            job_document(),
            R"("short_window": 20,)",
            R"("short_window": "20",)"
        ),
        "$.job.parameters.short_window",
        "Expected a number"
    );
}

void test_result_parses_and_round_trips() {
    const BacktestResultMessage message =
        parse_backtest_result_json(result_document());

    expect(message.worker_id == "worker-a", "worker id");
    expect(message.result.status == "completed", "status");
    expect(message.result.duration_seconds == 0.05, "duration");
    expect(message.result.metrics.has_value(), "metrics present");
    expect(
        message.result.metrics->number_of_trades == 18,
        "trade count"
    );
    expect(message.result.trades.has_value(), "trades present");
    expect(message.result.trades->size() == 2, "trade count in array");
    expect(
        message.result.trades->front().side == OrderSide::Buy,
        "buy side parsed"
    );
    expect(!message.result.error.has_value(), "no error");

    const std::string serialized =
        backtest_result_to_json(message, false);
    const BacktestResultMessage reparsed =
        parse_backtest_result_json(serialized);

    expect(
        serialized.find("\"type\":\"backtest_result\"") !=
            std::string::npos,
        "serialized type"
    );
    expect(
        reparsed.result.metrics->sharpe_ratio ==
            message.result.metrics->sharpe_ratio,
        "metrics round-trip"
    );
    expect(
        reparsed.result.trades->size() == 2,
        "trades round-trip"
    );
    expect(
        backtest_result_to_json(reparsed, false) == serialized,
        "serialization should be stable"
    );

    const ProtocolMessage dispatched =
        parse_protocol_message_json(result_document());
    expect(
        std::holds_alternative<BacktestResultMessage>(dispatched),
        "dispatch should select a result"
    );
}

void test_result_requires_consistent_status() {
    expect_validation(
        replace_once(
            result_document(),
            R"("duration_seconds": 0.05,
    "metrics": {
      "total_return": 0.1241,
      "sharpe_ratio": 0.1515,
      "max_drawdown": 0.2917,
      "number_of_trades": 18,
      "total_costs": 483.99
    },)",
            R"("duration_seconds": 0.05,)"
        ),
        "$.result",
        "must include 'metrics'"
    );

    expect_validation(
        replace_once(
            result_document(),
            R"("status": "completed")",
            R"("status": "failed")"
        ),
        "$.result",
        "must include 'error'"
    );

    expect_validation(
        replace_once(
            result_document(),
            R"("status": "completed")",
            R"("status": "invalid")"
        ),
        "$.result",
        "must include 'error'"
    );

    expect_validation(
        replace_once(
            result_document(),
            R"("status": "completed")",
            R"("status": "partial")"
        ),
        "$.result.status",
        "Expected 'completed', 'failed', or 'invalid'"
    );

    expect_validation(
        replace_once(
            result_document(),
            R"("duration_seconds": 0.05)",
            R"("duration_seconds": -0.05)"
        ),
        "$.result.duration_seconds",
        "non-negative"
    );
}

void test_result_error_matrix() {
    const BacktestResultMessage failed =
        parse_backtest_result_json(failed_result_document());
    expect(failed.result.status == "failed", "failed status");
    expect(failed.result.error.has_value(), "failed error");
    expect(
        failed.result.error->type == "runtime_error",
        "runtime error type"
    );
    expect(!failed.result.metrics.has_value(), "no failed metrics");
    expect(!failed.result.trades.has_value(), "no failed trades");

    expect_validation(
        replace_once(
            failed_result_document(),
            R"("type": "runtime_error")",
            R"("type": "validation_error")"
        ),
        "$.result.error.type",
        "must use error type 'runtime_error'"
    );

    const BacktestResultMessage invalid =
        parse_backtest_result_json(invalid_result_document());
    expect(invalid.result.status == "invalid", "invalid status");
    expect(
        invalid.result.error->type == "validation_error",
        "validation error type"
    );
    expect(
        invalid.result.error->path ==
            "$.job.parameters.short_window",
        "validation path"
    );

    expect_validation(
        replace_once(
            invalid_result_document(),
            R"("type": "validation_error")",
            R"("type": "runtime_error")"
        ),
        "$.result.error.type",
        "must use error type 'validation_error'"
    );

    expect_validation(
        replace_once(
            failed_result_document(),
            R"("error": {
      "type": "runtime_error",
      "message": "market data file not found"
    })",
            R"("metrics": {
      "total_return": 0.0,
      "sharpe_ratio": 0.0,
      "max_drawdown": 0.0,
      "number_of_trades": 0,
      "total_costs": 0.0
    })"
        ),
        "$.result",
        "must include 'error'"
    );

    expect_validation(
        replace_once(
            failed_result_document(),
            R"("duration_seconds": 0.05,)",
            R"("duration_seconds": 0.05,
    "metrics": {
      "total_return": 0.0,
      "sharpe_ratio": 0.0,
      "max_drawdown": 0.0,
      "number_of_trades": 0,
      "total_costs": 0.0
    },)"
        ),
        "$.result.metrics",
        "Only a completed result may include 'metrics'"
    );

    expect_validation(
        replace_once(
            failed_result_document(),
            R"("duration_seconds": 0.05,)",
            R"("duration_seconds": 0.05,
    "trades": [],)"
        ),
        "$.result.trades",
        "Only a completed result may include 'trades'"
    );
}

void test_optional_fields() {
    const std::string job_with_schema = replace_once(
        job_document(),
        "{\n  \"protocol_version\"",
        "{\n  \"$schema\": \"../../schema/backtest_job.schema.json\",\n"
        "  \"protocol_version\""
    );
    expect(
        parse_backtest_job_json(job_with_schema).job.job_id == "ma-1",
        "job accepts an editor schema reference"
    );

    const std::string result_with_schema = replace_once(
        result_document(),
        "{\n  \"protocol_version\"",
        "{\n  \"$schema\": \"../../schema/backtest_result.schema.json\",\n"
        "  \"protocol_version\""
    );
    expect(
        parse_backtest_result_json(result_with_schema).result.job_id ==
            "ma-1",
        "result accepts an editor schema reference"
    );

    expect_validation(
        replace_once(
            result_document(),
            R"("worker_id": "worker-a",)",
            R"("worker_id": "",)"
        ),
        "$.worker_id",
        "must not be empty when present"
    );

    expect_validation(
        replace_once(
            job_document(),
            R"("message_id": "run-1:job-1")",
            R"("message_id": "")"
        ),
        "$.message_id",
        "must not be empty"
    );
}

void test_result_rejects_error_on_completed() {
    expect_validation(
        replace_once(
            result_document(),
            R"("duration_seconds": 0.05,)",
            R"("duration_seconds": 0.05,
    "error": {
      "type": "runtime_error",
      "message": "boom"
    },)"
        ),
        "$.result.error",
        "must not include 'error'"
    );
}

void test_result_rejects_bad_trade() {
    expect_validation(
        replace_once(
            result_document(),
            R"("side": "buy")",
            R"("side": "hold")"
        ),
        "$.result.trades[0].side",
        "Expected 'buy' or 'sell'"
    );

    expect_validation(
        replace_once(
            result_document(),
            R"("quantity": 4.2)",
            R"("quantity": 0)"
        ),
        "$.result.trades[0].quantity",
        "must be positive"
    );

    expect_validation(
        replace_once(
            result_document(),
            R"("commission": 0.42)",
            R"("commission": -0.42)"
        ),
        "$.result.trades[0].commission",
        "non-negative"
    );
}

void test_protocol_error_round_trip() {
    const ProtocolErrorMessage message =
        parse_protocol_error_json(error_document());

    expect(message.message_id == "run-1:job-1", "error message id");
    expect(
        message.error.type == "unsupported_version",
        "error type"
    );
    expect(
        message.error.path == "$.protocol_version",
        "error path"
    );

    const std::string serialized =
        protocol_error_to_json(message, false);
    const ProtocolErrorMessage reparsed =
        parse_protocol_error_json(serialized);

    expect(
        serialized.find("\"type\":\"protocol_error\"") !=
            std::string::npos,
        "serialized type"
    );
    expect(
        reparsed.error.message == message.error.message,
        "error message round-trip"
    );

    const ProtocolMessage dispatched =
        parse_protocol_message_json(error_document());
    expect(
        std::holds_alternative<ProtocolErrorMessage>(dispatched),
        "dispatch should select a protocol error"
    );

    const ProtocolErrorMessage made = make_protocol_error(
        "unknown_message_type",
        "Unknown message type 'heartbeat'",
        "$.type",
        "run-1:job-1"
    );
    expect(
        made.error.type == "unknown_message_type",
        "helper type"
    );
    expect(
        protocol_error_to_json(made, false).find("run-1:job-1") !=
            std::string::npos,
        "helper message id"
    );

    // An omitted message_id is allowed; an explicit empty one is not.
    const ProtocolErrorMessage without_id = parse_protocol_error_json(
        replace_once(
            error_document(),
            R"("message_id": "run-1:job-1",)",
            ""
        )
    );
    expect(
        without_id.message_id.empty(),
        "omitted message id stays empty"
    );

    expect_validation(
        replace_once(
            error_document(),
            R"("message_id": "run-1:job-1")",
            R"("message_id": "")"
        ),
        "$.message_id",
        "must not be empty when present"
    );
}

void test_protocol_error_rejects_bad_type() {
    expect_validation(
        replace_once(
            error_document(),
            R"("type": "unsupported_version")",
            R"("type": "boom")"
        ),
        "$.error.type",
        "Expected 'json_parse_error'"
    );

    expect_validation(
        replace_once(
            error_document(),
            R"("message": "Unsupported protocol_version; this build speaks version 1")",
            R"("message": "")"
        ),
        "$.error.message",
        "must not be empty"
    );
}

void test_envelope() {
    const ProtocolEnvelope envelope =
        parse_protocol_envelope(job_document());

    expect(envelope.protocol_version == 1, "envelope version");
    expect(envelope.type == "backtest_job", "envelope type");
    expect(envelope.message_id == "run-1:job-1", "envelope message id");

    const ProtocolEnvelope unsupported = parse_protocol_envelope(
        replace_once(
            job_document(),
            R"("protocol_version": 1)",
            R"("protocol_version": 3)"
        )
    );
    expect(
        unsupported.protocol_version == 3,
        "envelope reader reports the actual version"
    );
    expect(
        unsupported.message_id == "run-1:job-1",
        "envelope reader keeps message id for an unsupported version"
    );
}

void test_unknown_message_type() {
    expect_validation(
        replace_once(
            job_document(),
            R"("type": "backtest_job")",
            R"("type": "heartbeat")"
        ),
        "$.type",
        "Unknown message type 'heartbeat'"
    );
}

void test_serializer_rejects_inconsistent_result() {
    BacktestResultMessage message;
    message.message_id = "run-1:job-1";
    message.result.experiment_id = "sweep.v1";
    message.result.job_id = "ma-1";
    message.result.strategy_id = "ma";
    message.result.strategy_type = "moving_average_crossover";
    message.result.parameters = {{"short_window", 20.0}, {"long_window", 100.0}};
    message.result.status = "completed";
    message.result.duration_seconds = 0.05;

    try {
        (void)backtest_result_to_json(message, false);
    } catch (const ProtocolValidationError& error) {
        expect(error.path() == "$.result", "serializer error path");
        expect(
            std::string(error.what()).find("must include 'metrics'") !=
                std::string::npos,
            "serializer error message"
        );
        return;
    }

    throw std::runtime_error(
        "Expected serializer to reject a completed result without metrics"
    );
}

void test_malformed_json() {
    try {
        (void)parse_backtest_job_json("{not json");
    } catch (const JsonParseError&) {
        return;
    }

    throw std::runtime_error("Expected JsonParseError");
}

}  // namespace

int main() {
    run_test("job parses with defaults", test_job_parses_with_defaults);
    run_test("job round-trips", test_job_round_trip);
    run_test("job rejects unknown fields", test_job_rejects_unknown_fields);
    run_test("job rejects bad envelope", test_job_rejects_bad_envelope);
    run_test("job rejects bad data", test_job_rejects_bad_data);
    run_test("job rejects bad execution", test_job_rejects_bad_execution);
    run_test("job rejects bad parameters", test_job_rejects_bad_parameters);
    run_test(
        "result parses and round-trips",
        test_result_parses_and_round_trips
    );
    run_test(
        "result requires consistent status",
        test_result_requires_consistent_status
    );
    run_test("result error matrix", test_result_error_matrix);
    run_test("optional fields", test_optional_fields);
    run_test(
        "result rejects error on completed",
        test_result_rejects_error_on_completed
    );
    run_test("result rejects bad trade", test_result_rejects_bad_trade);
    run_test(
        "protocol error round-trips",
        test_protocol_error_round_trip
    );
    run_test(
        "protocol error rejects bad type",
        test_protocol_error_rejects_bad_type
    );
    run_test("envelope parses", test_envelope);
    run_test("unknown message type rejected", test_unknown_message_type);
    run_test(
        "serializer rejects inconsistent result",
        test_serializer_rejects_inconsistent_result
    );
    run_test("malformed JSON rejected", test_malformed_json);

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
