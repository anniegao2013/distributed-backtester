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
// Five message types share a small envelope:
//
//   worker_ready      worker -> coordinator  (worker is available)
//   backtest_job      coordinator -> worker  (one fully-expanded backtest)
//   backtest_result   worker -> coordinator  (outcome of one job)
//   shutdown          coordinator -> worker  (graceful stop after current job)
//   worker_error      worker -> coordinator  (execution or protocol failure)
//
// `protocol_error` is accepted as a deprecated input alias of `worker_error`.
// Serializers always emit the canonical `worker_error` type.
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

// Worker -> coordinator availability announcement, sent once after the
// transport is connected and before the worker waits for work. It tells the
// coordinator which worker_id owns the connection; it is not a request for a
// job and has no reply.
struct WorkerReadyMessage {
    int protocol_version = kBacktestProtocolVersion;
    std::string message_id;
    std::string worker_id;
};

// Coordinator -> worker control message. The worker finishes the job it is
// already running, if any, then stops accepting messages and closes its side
// of the connection. No reply is defined: the coordinator observes the close.
// `reason` is optional and exists for logs and diagnostics.
struct ShutdownMessage {
    int protocol_version = kBacktestProtocolVersion;
    std::string message_id;
    std::string reason;
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

// Worker -> coordinator failure report. It covers two situations:
//
//   protocol failures  the envelope or payload could not be processed at all
//                      (json_parse_error, unsupported_version,
//                      unknown_message_type, validation_error)
//   execution failure  the worker could not produce a result for a readable
//                      job (execution_error)
//
// A job whose envelope and payload are readable but whose strategy or data is
// unusable still produces a backtest_result with status "invalid" or "failed".
// That keeps the one-result-per-job guarantee intact. worker_error is for
// failures that cannot be represented by a result.
struct WorkerErrorDetail {
    std::string type;
    std::string path;
    std::string message;
};

struct WorkerErrorMessage {
    int protocol_version = kBacktestProtocolVersion;
    std::string message_id;
    std::string worker_id;
    WorkerErrorDetail error;
};

// Deprecated names retained for source compatibility. The wire type is
// "worker_error"; "protocol_error" remains readable on input.
using ProtocolErrorDetail = WorkerErrorDetail;
using ProtocolErrorMessage = WorkerErrorMessage;

using ProtocolMessage = std::variant<
    WorkerReadyMessage,
    BacktestJobMessage,
    BacktestResultMessage,
    ShutdownMessage,
    WorkerErrorMessage
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
// unsupported version with a worker_error while still echoing message_id.
ProtocolEnvelope parse_protocol_envelope(const std::string& text);

// Parses and fully validates one message. Each type-specific parser also
// verifies that the envelope `type` matches. All throw JsonParseError for
// malformed JSON and ProtocolValidationError for schema violations.
WorkerReadyMessage parse_worker_ready_json(const std::string& text);
BacktestJobMessage parse_backtest_job_json(const std::string& text);
BacktestResultMessage parse_backtest_result_json(const std::string& text);
ShutdownMessage parse_shutdown_json(const std::string& text);
WorkerErrorMessage parse_worker_error_json(const std::string& text);

// Deprecated alias that accepts either "worker_error" or "protocol_error".
ProtocolErrorMessage parse_protocol_error_json(const std::string& text);

// Dispatches on the envelope `type` and parses the corresponding payload.
// Throws ProtocolValidationError for an unknown message type.
ProtocolMessage parse_protocol_message_json(const std::string& text);

// Serializers. Object keys are emitted in deterministic (sorted) order. They
// validate the message first, so an inconsistent result (for example,
// "completed" without metrics) is rejected rather than written to the wire.
std::string worker_ready_to_json(
    const WorkerReadyMessage& message,
    bool pretty = true
);

std::string backtest_job_to_json(
    const BacktestJobMessage& message,
    bool pretty = true
);

std::string backtest_result_to_json(
    const BacktestResultMessage& message,
    bool pretty = true
);

std::string shutdown_to_json(
    const ShutdownMessage& message,
    bool pretty = true
);

// Always emits the canonical "worker_error" type.
std::string worker_error_to_json(
    const WorkerErrorMessage& message,
    bool pretty = true
);

// Deprecated alias; emits the canonical "worker_error" type.
std::string protocol_error_to_json(
    const ProtocolErrorMessage& message,
    bool pretty = true
);

std::string protocol_message_to_json(
    const ProtocolMessage& message,
    bool pretty = true
);

// Convenience constructor for a worker_error reply. `path`, `message_id`, and
// `worker_id` may be empty; they are omitted from the serialized document.
WorkerErrorMessage make_worker_error(
    std::string type,
    std::string message,
    std::string path = {},
    std::string message_id = {},
    std::string worker_id = {}
);

// Deprecated alias.
ProtocolErrorMessage make_protocol_error(
    std::string type,
    std::string message,
    std::string path = {},
    std::string message_id = {},
    std::string worker_id = {}
);
