#pragma once

#include "core/json_value.h"
#include "core/parameter_types.h"
#include "trading/backtest_engine.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// Wire protocol between the experiment coordinator and backtest workers.
//
// This layer defines message shapes and JSON (de)serialization only. It is
// deliberately transport-neutral: TCP framing, connection management, retries,
// and scheduling are not part of it. A transport adapter carries exactly one
// UTF-8 JSON document per message.
//
// Three message types share a small envelope:
//
//   backtest_job      coordinator -> worker  (one fully-expanded backtest)
//   backtest_result   worker -> coordinator  (outcome of one job)
//   protocol_error    worker -> coordinator  (envelope could not be processed)
//
// `message_id` is the correlation key. It is chosen by the sender, must be
// unique for the lifetime of a run, and is echoed by the receiver so a reply
// can be matched even when several jobs are in flight.

inline constexpr int kBacktestProtocolVersion = 1;

// Raised when a protocol document is valid JSON but violates the wire schema.
// `path()` points at the offending field using a JSONPath-like expression such
// as "$.job.data.start_date".
class ProtocolValidationError : public std::runtime_error {
public:
    ProtocolValidationError(std::string path, std::string message);

    const std::string& path() const noexcept;

private:
    std::string path_;
};

// Where the worker can find the dataset and which slice to backtest. Paths are
// resolved in the worker's environment, not the coordinator's.
struct BacktestJobData {
    std::string path;
    std::string asset;
    std::string start_date;
    std::string end_date;
};

// Per-job execution settings. Concurrency is a coordinator concern and is
// therefore not part of a job message.
struct BacktestJobExecution {
    double initial_cash = 10000.0;
    double transaction_cost_rate = 0.0;
    double slippage_rate = 0.0;
};

struct BacktestJobOutput {
    bool include_trades = false;
};

// A self-contained unit of work. Everything needed to reproduce the backtest
// travels with the job so a worker never has to consult coordinator state.
struct BacktestJob {
    std::string experiment_id;
    std::string job_id;
    std::string strategy_id;
    std::string strategy_type;
    ParameterMap parameters;
    BacktestJobData data;
    BacktestJobExecution execution;
    BacktestJobOutput output;
};

struct BacktestJobMessage {
    int protocol_version = kBacktestProtocolVersion;
    std::string message_id;
    BacktestJob job;
};

// A structured failure attached to a result. `type` is "validation_error" for
// status "invalid" and "runtime_error" for status "failed".
struct BacktestResultError {
    std::string type;
    std::string path;
    std::string message;
};

// The outcome of one job. `status` is one of:
//
//   completed  metrics are present; error is absent
//   failed     error.type is "runtime_error"; metrics are absent
//   invalid    error.type is "validation_error"; metrics are absent
//
// `trades` is engaged only when the job requested include_trades and the job
// completed. The result echoes the job's strategy identity so it is
// self-describing and can be aggregated without retaining the request.
struct BacktestResult {
    std::string experiment_id;
    std::string job_id;
    std::string strategy_id;
    std::string strategy_type;
    ParameterMap parameters;
    std::string status;
    double duration_seconds = 0.0;
    std::optional<BacktestMetrics> metrics;
    std::optional<std::vector<Trade>> trades;
    std::optional<BacktestResultError> error;
};

struct BacktestResultMessage {
    int protocol_version = kBacktestProtocolVersion;
    std::string message_id;
    std::string worker_id;
    BacktestResult result;
};

// Returned when the envelope itself cannot be processed: malformed JSON, an
// unsupported protocol version, or an unknown message type. A job whose
// envelope is valid but whose payload is invalid produces a backtest_result
// with status "invalid" instead.
struct ProtocolErrorDetail {
    std::string type;
    std::string path;
    std::string message;
};

struct ProtocolErrorMessage {
    int protocol_version = kBacktestProtocolVersion;
    std::string message_id;
    ProtocolErrorDetail error;
};

using ProtocolMessage = std::variant<
    BacktestJobMessage,
    BacktestResultMessage,
    ProtocolErrorMessage
>;

// Envelope metadata, readable before the payload is parsed. This lets a
// transport route a message or reject an unsupported version cheaply.
struct ProtocolEnvelope {
    int protocol_version = 0;
    std::string type;
    std::string message_id;
};

// Parses just the envelope. Throws JsonParseError for malformed JSON and
// ProtocolValidationError when the envelope fields are invalid. The reported
// version is not required to be supported, so a server can reject an
// unsupported version with a protocol_error while still echoing message_id.
ProtocolEnvelope parse_protocol_envelope(const std::string& text);

// Parses and fully validates one message. Each type-specific parser also
// verifies that the envelope `type` matches. All throw JsonParseError for
// malformed JSON and ProtocolValidationError for schema violations.
BacktestJobMessage parse_backtest_job_json(const std::string& text);
BacktestResultMessage parse_backtest_result_json(const std::string& text);
ProtocolErrorMessage parse_protocol_error_json(const std::string& text);

// Dispatches on the envelope `type` and parses the corresponding payload.
// Throws ProtocolValidationError for an unknown message type.
ProtocolMessage parse_protocol_message_json(const std::string& text);

// Serializers. Object keys are emitted in deterministic (sorted) order. They
// validate the message first, so an inconsistent result (for example,
// "completed" without metrics) is rejected rather than written to the wire.
std::string backtest_job_to_json(
    const BacktestJobMessage& message,
    bool pretty = true
);

std::string backtest_result_to_json(
    const BacktestResultMessage& message,
    bool pretty = true
);

std::string protocol_error_to_json(
    const ProtocolErrorMessage& message,
    bool pretty = true
);

std::string protocol_message_to_json(
    const ProtocolMessage& message,
    bool pretty = true
);

// Convenience constructor for a protocol_error reply. `path` and `message_id`
// may be empty; they are omitted from the serialized document.
ProtocolErrorMessage make_protocol_error(
    std::string type,
    std::string message,
    std::string path = {},
    std::string message_id = {}
);
