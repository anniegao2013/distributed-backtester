#pragma once

#include "protocol/protocol.h"
#include "trading/backtest_engine.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

// The worker half of the coordinator/worker split.
//
// A worker owns no scheduling state. It repeatedly receives one JSON message,
// runs at most one backtest, and replies with exactly one message. The
// transport is injected, so the same worker loop can run over an in-memory
// queue in tests, over stdio for local development, or over TCP once that
// adapter exists.
//
// This layer deliberately contains no socket code. See docs/worker.md.

// One message in, one message out. Implementations own framing: receive()
// returns exactly one complete JSON document and send() accepts exactly one.
// Both may throw; run_worker() lets transport failures propagate.
class WorkerTransport {
public:
    virtual ~WorkerTransport() = default;

    // Establishes the connection and performs any handshake. The default is a
    // no-op for transports that are already connected when constructed.
    virtual void connect() {}

    // Blocks until the next message is available. Returns std::nullopt when
    // the coordinator closed the connection and the worker should exit.
    virtual std::optional<std::string> receive() = 0;

    // Sends one complete message. Must not return until the message has been
    // handed to the transport.
    virtual void send(const std::string& message) = 0;
};

struct WorkerConfig {
    // Echoed in every result as `worker_id`. Must satisfy the protocol's
    // identifier rules.
    std::string worker_id = "worker-local";

    // Optional diagnostics sink. The worker never writes logs to its message
    // stream, so the executable points this at stderr.
    std::function<void(const std::string&)> log;
};

// Counters for one run of the loop. A job counted as failed or invalid still
// produced a backtest_result; a protocol error produced a protocol_error.
struct WorkerStats {
    std::size_t messages_received = 0;
    std::size_t jobs_received = 0;
    std::size_t jobs_completed = 0;
    std::size_t jobs_failed = 0;
    std::size_t jobs_invalid = 0;
    std::size_t protocol_errors = 0;
};

// The outcome of running one job. `status` uses the wire vocabulary:
// "completed", "failed", or "invalid". For the two failure statuses,
// `error_type`, `error_path`, and `error_message` describe the problem.
struct WorkerJobOutcome {
    std::string status;
    std::string error_type;
    std::string error_path;
    std::string error_message;
    double duration_seconds = 0.0;
    JobResult result;
};

// Validates the job's strategy parameters, loads its dataset, builds the
// strategy, and runs the existing backtesting engine. Never throws for a bad
// job: a semantic validation failure becomes status "invalid" and any other
// failure, including missing or unusable data, becomes status "failed".
WorkerJobOutcome execute_backtest_job(const BacktestJob& job);

// Builds the reply for a job, echoing its identity and parameters. Metrics and
// trades are attached only for a completed job; an error is attached otherwise.
BacktestResultMessage make_result_message(
    const BacktestJobMessage& request,
    const std::string& worker_id,
    const WorkerJobOutcome& outcome
);

// Connects, then loops until the transport is closed. Every received message
// produces exactly one reply:
//
//   valid job          -> backtest_result (completed, failed, or invalid)
//   malformed bytes    -> protocol_error json_parse_error
//   bad envelope       -> protocol_error validation_error
//   unsupported version-> protocol_error unsupported_version
//   unexpected type    -> protocol_error unknown_message_type
WorkerStats run_worker(
    WorkerTransport& transport,
    const WorkerConfig& config = {}
);
