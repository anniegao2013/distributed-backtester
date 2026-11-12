#pragma once

#include "comm/message_channel.h"
#include "protocol/protocol.h"
#include "trading/backtest_engine.h"
#include "worker/dataset_cache.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

// The worker half of the coordinator/worker split.
//
// A worker owns no scheduling state. After connecting it announces itself with
// one worker_ready message, then repeatedly receives one JSON message, runs at
// most one backtest, and replies with exactly one message. The channel is
// injected, so the same worker loop can run over an in-memory queue in tests,
// over stdio for local development, or over TCP.
//
// This layer deliberately contains no socket code. See docs/worker.md.

// Messages in and out. This is the common communication-layer contract;
// implementations own framing and connection lifecycle. The worker sends an
// unsolicited worker_ready message before it starts receiving. Both calls may
// throw; run_worker() lets transport failures propagate.
using WorkerTransport = comm::MessageChannel;

struct WorkerConfig {
    // Echoed in every result as `worker_id`. Must satisfy the protocol's
    // identifier rules.
    std::string worker_id = "worker-local";

    // Optional diagnostics sink. The worker never writes logs to its message
    // stream, so the executable points this at stderr.
    std::function<void(const std::string&)> log;
};

// Counters for one run of the loop. A job counted as failed or invalid still
// produced a backtest_result; a worker error produced a worker_error.
struct WorkerStats {
    std::size_t ready_messages_sent = 0;
    std::size_t messages_received = 0;
    std::size_t jobs_received = 0;
    std::size_t jobs_completed = 0;
    std::size_t jobs_failed = 0;
    std::size_t jobs_invalid = 0;
    std::size_t worker_errors = 0;
    std::size_t shutdowns_received = 0;
    std::size_t dataset_cache_hits = 0;
    std::size_t dataset_cache_misses = 0;
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
//
// The one-argument form uses a throwaway cache, so it retains its previous
// behavior. The cache-aware form reuses parsed datasets across jobs and is what
// run_worker() uses.
WorkerJobOutcome execute_backtest_job(const BacktestJob& job);
WorkerJobOutcome execute_backtest_job(
    const BacktestJob& job,
    DatasetCache& dataset_cache
);

// Builds the reply for a job, echoing its identity and parameters. Metrics and
// trades are attached only for a completed job; an error is attached otherwise.
BacktestResultMessage make_result_message(
    const BacktestJobMessage& request,
    const std::string& worker_id,
    const WorkerJobOutcome& outcome
);

// Connects, announces readiness with one worker_ready message, then loops
// until a shutdown message arrives or the transport is closed. Every received
// message produces exactly one reply:
//
//   valid job          -> backtest_result (completed, failed, or invalid)
//   shutdown           -> no reply; the loop stops after the current job
//   malformed bytes    -> worker_error json_parse_error
//   bad envelope       -> worker_error validation_error
//   unsupported version-> worker_error unsupported_version
//   unexpected type    -> worker_error unknown_message_type
WorkerStats run_worker(
    WorkerTransport& transport,
    const WorkerConfig& config = {}
);
