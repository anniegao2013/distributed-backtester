#pragma once

#include "coordinator/job_state.h"
#include "experiment/experiment.h"
#include "net/tcp.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Coordinator half of the distributed split.
//
// The coordinator is the TCP server. It binds a listening socket, accepts the
// configured number of workers, waits for each worker's worker_ready
// announcement, then dispatches the experiment's expanded jobs. Jobs are queued
// in request order and handed out FIFO to whichever worker finishes first, so a
// slow job never blocks the others. Each worker executes one job at a time and
// replies with exactly one backtest_result. When the queue is empty the
// coordinator sends shutdown to every worker and aggregates the results into the
// same ExperimentResult that a local run produces.
//
// The coordinator owns the authoritative job state table: jobs start pending
// in its queue, move to running when a worker takes them, and end completed or
// failed. A retryable failure moves a running job back to pending and returns
// it to the queue while attempts remain. Workers never publish state directly;
// their replies are interpreted by the coordinator.

// Emitted once when the coordinator detects that a worker connection is gone.
// `job_id` is empty when the worker was lost before it was given work.
// `job_failed` is true when an in-flight job was marked failed; when the job
// still had attempts left it is requeued instead, and
// `job_retry_scheduled` is true.
struct WorkerDisconnectEvent {
    std::string worker_id;
    std::string reason;
    std::string job_id;
    bool job_failed = false;
    bool job_retry_scheduled = false;
};

struct CoordinatorConfig {
    // Bind address. An empty host listens on all interfaces. Port 0 asks the
    // operating system for an ephemeral port; on_listening reports the bound
    // endpoint.
    std::string listen_host = "127.0.0.1";
    std::uint16_t listen_port = 9000;

    // Connections to accept before dispatching starts. The coordinator blocks
    // until every worker has connected and announced worker_ready.
    std::size_t expected_workers = 1;

    // Maximum number of times one job may be dispatched to a worker. The first
    // delivery counts as attempt 1. When an attempt fails in a retryable way
    // and attempts remain, the job returns to the queue and is offered to the
    // next available worker. 1 disables retries. Must be at least 1.
    std::size_t max_job_attempts = 3;

    // A backtest_result with status "failed" reports a runtime failure, which
    // may be specific to one worker or transient. When true it is retried
    // within max_job_attempts; when false it is terminal immediately.
    // Status "invalid" is a deterministic validation failure and is never
    // retried. worker_error replies and transport failures are always
    // retryable while attempts remain.
    bool retry_failed_results = true;

    // Called once, after the listening socket is bound and before the
    // coordinator starts accepting. Useful for tests and for learning the
    // ephemeral port.
    std::function<void(const net::TcpEndpoint&)> on_listening;

    // Optional diagnostics sink. The coordinator never writes protocol data
    // here; the CLI points it at stderr.
    std::function<void(const std::string&)> log;

    // Called by the coordinator whenever a job changes state. The coordinator
    // owns the state table; workers only report results. Calls from multiple
    // worker threads are serialized by the coordinator, but the callback may
    // run on a worker thread.
    std::function<void(const JobStateTransition&)> on_job_state_changed;

    // Called once per worker whose connection is lost during the run. The
    // callback may run on a worker thread and is serialized by the
    // coordinator. Throwing callbacks are suppressed after the loss is
    // recorded, so they cannot interrupt shutdown or leave state inconsistent.
    std::function<void(const WorkerDisconnectEvent&)> on_worker_disconnected;
};

struct CoordinatorStats {
    std::size_t workers_registered = 0;
    std::size_t jobs_dispatched = 0;
    std::size_t results_received = 0;
    std::size_t worker_errors = 0;
    std::size_t transport_failures = 0;

    // Workers whose connection was lost during the run. Such a worker is
    // removed from scheduling; its in-flight job (if any) is requeued when
    // attempts remain, and queued work is offered to the remaining workers.
    std::size_t workers_disconnected = 0;

    // Number of result or error messages ignored because their message_id had
    // already been handled. Duplicates never change a job outcome, advance a
    // state, or count as a received result.
    std::size_t duplicate_results = 0;

    // Number of dispatch attempts after the first, across all jobs. A job that
    // is requeued but never retried because every worker has gone is not
    // counted here.
    std::size_t job_retries = 0;

    // Number of dispatch attempts made for each job, in request order. A job
    // that completed or failed on its first delivery has a count of 1. Jobs
    // that were never dispatched have a count of 0.
    std::vector<std::size_t> job_attempts;

    // Final coordinator-owned state for every job, in request order. Once
    // run_distributed_experiment() returns, every entry is completed or failed.
    std::vector<JobState> job_states;
};

// Binds, accepts workers, dispatches every job in `request`, shuts the workers
// down, and returns the aggregated result. `request` must already be finalized
// (jobs populated). Throws on startup or transport failure; per-job failures
// are represented in the returned result instead.
ExperimentResult run_distributed_experiment(
    const ExperimentRequest& request,
    const CoordinatorConfig& config = {},
    CoordinatorStats* stats = nullptr
);
