#include "core/json_value.h"
#include "protocol/protocol.h"
#include "worker/stdio_transport.h"
#include "worker/worker.h"

#include <cmath>
#include <cstddef>
#include <deque>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
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

BacktestJobMessage make_job(
    const std::string& message_id = "worker-test:ma-1:0001"
) {
    BacktestJobMessage message;
    message.message_id = message_id;
    message.job.experiment_id = "worker-test";
    message.job.job_id = "ma-1";
    message.job.strategy_id = "ma";
    message.job.strategy_type = "moving_average_crossover";
    message.job.parameters = {
        {"short_window", 20.0},
        {"long_window", 100.0}
    };
    message.job.data = {
        "data/market/sp500_1950_2018.csv",
        "SP500",
        "2000-01-01",
        "2010-12-31"
    };
    message.job.execution = {
        10000.0,
        0.001,
        0.0005
    };
    message.job.output.include_trades = false;
    return message;
}

std::string job_json(const BacktestJobMessage& job) {
    return backtest_job_to_json(job, /*pretty=*/false);
}

class MemoryTransport : public WorkerTransport {
public:
    explicit MemoryTransport(std::vector<std::string> messages)
        : pending_(messages.begin(), messages.end()) {}

    void connect() override {
        connected = true;
    }

    std::optional<std::string> receive() override {
        if (pending_.empty()) {
            return std::nullopt;
        }

        std::string message = std::move(pending_.front());
        pending_.pop_front();
        return message;
    }

    void send(const std::string& message) override {
        sent.push_back(message);
    }

    bool connected = false;
    std::vector<std::string> sent;

private:
    std::deque<std::string> pending_;
};

void test_runs_backtest() {
    const WorkerJobOutcome outcome =
        execute_backtest_job(make_job().job);

    expect(outcome.status == "completed", "expected a completed job");
    expect(outcome.error_type.empty(), "completed job must not set an error");
    expect(
        std::isfinite(outcome.result.metrics.total_return) &&
            std::isfinite(outcome.result.metrics.sharpe_ratio) &&
            std::isfinite(outcome.result.metrics.max_drawdown),
        "metrics must be finite"
    );
    expect(
        outcome.duration_seconds >= 0.0,
        "duration must be non-negative"
    );
}

void test_rejects_unknown_strategy() {
    BacktestJobMessage job = make_job();
    job.job.strategy_type = "not_a_strategy";

    const WorkerJobOutcome outcome = execute_backtest_job(job.job);

    expect(outcome.status == "invalid", "expected invalid status");
    expect(
        outcome.error_type == "validation_error",
        "expected validation_error"
    );
    expect(
        outcome.error_message.find("Unknown strategy type") !=
            std::string::npos,
        "expected an unknown-type message"
    );
}

void test_rejects_bad_parameters() {
    BacktestJobMessage job = make_job();
    job.job.parameters["short_window"] = 200.0;

    const WorkerJobOutcome outcome = execute_backtest_job(job.job);

    expect(outcome.status == "invalid", "expected invalid status");
    expect(
        outcome.error_path == "$.job.parameters.short_window",
        "expected the offending parameter path, got " + outcome.error_path
    );
}

void test_reports_missing_data_as_failed() {
    BacktestJobMessage job = make_job();
    job.job.data.path = "data/market/does_not_exist.csv";

    const WorkerJobOutcome outcome = execute_backtest_job(job.job);

    expect(outcome.status == "failed", "expected failed status");
    expect(
        outcome.error_type == "runtime_error",
        "expected runtime_error"
    );
    expect(
        !outcome.error_message.empty(),
        "failed job must explain the failure"
    );
}

void test_builds_completed_result() {
    const BacktestJobMessage job = make_job();
    const WorkerJobOutcome outcome = execute_backtest_job(job.job);

    const BacktestResultMessage message =
        make_result_message(job, "worker-a", outcome);

    expect(message.message_id == job.message_id, "must echo message_id");
    expect(message.worker_id == "worker-a", "must set worker_id");
    expect(
        message.result.experiment_id == job.job.experiment_id &&
            message.result.job_id == job.job.job_id &&
            message.result.strategy_id == job.job.strategy_id &&
            message.result.strategy_type == job.job.strategy_type,
        "must echo the job identity"
    );
    expect(
        message.result.parameters == job.job.parameters,
        "must echo the job parameters"
    );
    expect(
        message.result.status == "completed",
        "expected completed status"
    );
    expect(
        message.result.metrics.has_value(),
        "completed result must carry metrics"
    );
    expect(
        !message.result.error.has_value(),
        "completed result must not carry an error"
    );
    expect(
        !message.result.trades.has_value(),
        "trades must be omitted when include_trades is false"
    );

    // The serialized form must itself satisfy the wire validator.
    const std::string text = backtest_result_to_json(message, false);
    expect(!text.empty(), "result must serialize");
}

void test_builds_result_with_trades() {
    BacktestJobMessage job = make_job();
    job.job.output.include_trades = true;

    const WorkerJobOutcome outcome = execute_backtest_job(job.job);
    const BacktestResultMessage message =
        make_result_message(job, "worker-a", outcome);

    expect(
        message.result.trades.has_value(),
        "trades must be present when include_trades is true"
    );
}

void test_builds_failed_result() {
    BacktestJobMessage job = make_job();
    job.job.data.path = "data/market/does_not_exist.csv";

    const WorkerJobOutcome outcome = execute_backtest_job(job.job);
    const BacktestResultMessage message =
        make_result_message(job, "worker-a", outcome);

    expect(
        message.result.status == "failed",
        "expected failed status"
    );
    expect(
        message.result.error.has_value() &&
            message.result.error->type == "runtime_error",
        "failed result must carry a runtime_error"
    );
    expect(
        !message.result.metrics.has_value() &&
            !message.result.trades.has_value(),
        "failed result must not carry metrics or trades"
    );

    const std::string text = backtest_result_to_json(message, false);
    expect(!text.empty(), "failed result must serialize");
}

void test_loop_completes_job() {
    MemoryTransport transport({job_json(make_job())});
    WorkerConfig config;
    config.worker_id = "worker-a";

    const WorkerStats stats = run_worker(transport, config);

    expect(transport.connected, "run_worker must connect first");
    expect(stats.messages_received == 1, "expected one message");
    expect(stats.jobs_received == 1, "expected one job");
    expect(stats.jobs_completed == 1, "expected one completed job");
    expect(stats.protocol_errors == 0, "expected no protocol errors");
    expect(transport.sent.size() == 1, "expected one reply");

    const BacktestResultMessage reply =
        parse_backtest_result_json(transport.sent.front());

    expect(
        reply.message_id == "worker-test:ma-1:0001",
        "reply must echo message_id"
    );
    expect(reply.worker_id == "worker-a", "reply must identify the worker");
    expect(
        reply.result.status == "completed",
        "reply must report completion"
    );
}

void test_loop_processes_jobs_sequentially() {
    MemoryTransport transport({
        job_json(make_job("worker-test:ma-1:0001")),
        job_json(make_job("worker-test:ma-2:0002"))
    });

    const WorkerStats stats = run_worker(transport);

    expect(stats.messages_received == 2, "expected two messages");
    expect(stats.jobs_completed == 2, "expected two completed jobs");
    expect(transport.sent.size() == 2, "expected two replies");

    const BacktestResultMessage first =
        parse_backtest_result_json(transport.sent[0]);
    const BacktestResultMessage second =
        parse_backtest_result_json(transport.sent[1]);

    expect(
        first.message_id == "worker-test:ma-1:0001" &&
            second.message_id == "worker-test:ma-2:0002",
        "each reply must match its job"
    );
}

void test_loop_reports_malformed_json() {
    MemoryTransport transport({"{not json"});

    const WorkerStats stats = run_worker(transport);

    expect(stats.protocol_errors == 1, "expected one protocol error");
    expect(stats.jobs_received == 0, "no job should be accepted");

    const ProtocolErrorMessage error =
        parse_protocol_error_json(transport.sent.front());

    expect(
        error.error.type == "json_parse_error",
        "expected json_parse_error"
    );
}

void test_loop_reports_invalid_envelope() {
    MemoryTransport transport({R"({"protocol_version":1})"});

    const WorkerStats stats = run_worker(transport);

    expect(stats.protocol_errors == 1, "expected one protocol error");

    const ProtocolErrorMessage error =
        parse_protocol_error_json(transport.sent.front());

    expect(
        error.error.type == "validation_error",
        "expected validation_error"
    );
    expect(
        error.error.path == "$",
        "expected the envelope root path"
    );
}

void test_loop_reports_unsupported_version() {
    const std::string job = replace_once(
        job_json(make_job()),
        "\"protocol_version\":1",
        "\"protocol_version\":99"
    );

    MemoryTransport transport({job});
    const WorkerStats stats = run_worker(transport);

    expect(stats.protocol_errors == 1, "expected one protocol error");

    const ProtocolErrorMessage error =
        parse_protocol_error_json(transport.sent.front());

    expect(
        error.error.type == "unsupported_version",
        "expected unsupported_version"
    );
    expect(
        error.message_id == "worker-test:ma-1:0001",
        "unsupported-version reply must echo message_id"
    );
}

void test_loop_reports_unexpected_message_type() {
    MemoryTransport transport({
        R"({"protocol_version":1,"type":"backtest_result","message_id":"m-1"})"
    });

    const WorkerStats stats = run_worker(transport);

    expect(stats.protocol_errors == 1, "expected one protocol error");

    const ProtocolErrorMessage error =
        parse_protocol_error_json(transport.sent.front());

    expect(
        error.error.type == "unknown_message_type",
        "expected unknown_message_type"
    );
    expect(
        error.message_id == "m-1",
        "reply must echo message_id"
    );
}

void test_loop_reports_invalid_job_payload() {
    MemoryTransport transport({
        R"({"protocol_version":1,"type":"backtest_job",)"
        R"("message_id":"m-2","job":{"experiment_id":"e",)"
        R"("job_id":"j","strategy_id":"s"}})"
    });

    const WorkerStats stats = run_worker(transport);

    expect(stats.protocol_errors == 1, "expected one protocol error");
    expect(stats.jobs_received == 0, "no job should be accepted");

    const ProtocolErrorMessage error =
        parse_protocol_error_json(transport.sent.front());

    expect(
        error.error.type == "validation_error",
        "expected validation_error"
    );
    expect(
        error.message_id == "m-2",
        "payload errors must echo message_id"
    );
    expect(
        error.error.path == "$.job",
        "expected the job path, got " + error.error.path
    );
}

void test_loop_returns_invalid_result_for_bad_strategy() {
    BacktestJobMessage job = make_job();
    job.job.strategy_type = "not_a_strategy";

    MemoryTransport transport({job_json(job)});
    const WorkerStats stats = run_worker(transport);

    expect(stats.jobs_invalid == 1, "expected one invalid job");
    expect(transport.sent.size() == 1, "expected one reply");

    const BacktestResultMessage reply =
        parse_backtest_result_json(transport.sent.front());

    expect(
        reply.result.status == "invalid",
        "semantic failures must use result status invalid"
    );
    expect(
        reply.result.error.has_value() &&
            reply.result.error->type == "validation_error",
        "invalid result must carry a validation_error"
    );
}

void test_loop_stops_at_end_of_input() {
    MemoryTransport transport({});
    const WorkerStats stats = run_worker(transport);

    expect(stats.messages_received == 0, "expected no messages");
    expect(transport.sent.empty(), "expected no replies");
}

void test_stdio_transport_reads_and_writes() {
    std::istringstream input(
        "\n{\"a\":1}\r\n{\"b\":2}\n"
    );
    std::ostringstream output;

    StdioTransport transport(input, output);
    transport.connect();

    const auto first = transport.receive();
    const auto second = transport.receive();
    const auto end = transport.receive();

    expect(
        first.has_value() && *first == "{\"a\":1}",
        "expected the first line without a carriage return"
    );
    expect(
        second.has_value() && *second == "{\"b\":2}",
        "expected the second line"
    );
    expect(!end.has_value(), "expected end of input");

    transport.send("{\"ok\":true}");
    expect(
        output.str() == "{\"ok\":true}\n",
        "expected a newline-framed message"
    );
}

void test_stdio_transport_rejects_multiline_messages() {
    std::istringstream input;
    std::ostringstream output;
    StdioTransport transport(input, output);

    bool threw = false;

    try {
        transport.send("{\n  \"ok\": true\n}");
    } catch (const std::exception&) {
        threw = true;
    }

    expect(threw, "pretty JSON cannot be line framed");
}

}  // namespace

int main() {
    run_test("run backtest through the engine", test_runs_backtest);
    run_test("reject unknown strategy", test_rejects_unknown_strategy);
    run_test("reject invalid parameters", test_rejects_bad_parameters);
    run_test(
        "report missing data as failed",
        test_reports_missing_data_as_failed
    );
    run_test("build completed result", test_builds_completed_result);
    run_test("build result with trades", test_builds_result_with_trades);
    run_test("build failed result", test_builds_failed_result);
    run_test("loop completes one job", test_loop_completes_job);
    run_test(
        "loop processes jobs sequentially",
        test_loop_processes_jobs_sequentially
    );
    run_test("loop reports malformed JSON", test_loop_reports_malformed_json);
    run_test(
        "loop reports invalid envelope",
        test_loop_reports_invalid_envelope
    );
    run_test(
        "loop reports unsupported version",
        test_loop_reports_unsupported_version
    );
    run_test(
        "loop reports unexpected type",
        test_loop_reports_unexpected_message_type
    );
    run_test(
        "loop reports invalid job payload",
        test_loop_reports_invalid_job_payload
    );
    run_test(
        "loop returns invalid result for bad strategy",
        test_loop_returns_invalid_result_for_bad_strategy
    );
    run_test("loop stops at end of input", test_loop_stops_at_end_of_input);
    run_test(
        "stdio transport reads and writes",
        test_stdio_transport_reads_and_writes
    );
    run_test(
        "stdio transport rejects multiline messages",
        test_stdio_transport_rejects_multiline_messages
    );

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
