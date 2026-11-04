#pragma once

#include "experiment/experiment.h"
#include "net/tcp.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

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

struct CoordinatorConfig {
    // Bind address. An empty host listens on all interfaces. Port 0 asks the
    // operating system for an ephemeral port; on_listening reports the bound
    // endpoint.
    std::string listen_host = "127.0.0.1";
    std::uint16_t listen_port = 9000;

    // Connections to accept before dispatching starts. The coordinator blocks
    // until every worker has connected and announced worker_ready.
    std::size_t expected_workers = 1;

    // Called once, after the listening socket is bound and before the
    // coordinator starts accepting. Useful for tests and for learning the
    // ephemeral port.
    std::function<void(const net::TcpEndpoint&)> on_listening;

    // Optional diagnostics sink. The coordinator never writes protocol data
    // here; the CLI points it at stderr.
    std::function<void(const std::string&)> log;
};

struct CoordinatorStats {
    std::size_t workers_registered = 0;
    std::size_t jobs_dispatched = 0;
    std::size_t results_received = 0;
    std::size_t worker_errors = 0;
    std::size_t transport_failures = 0;
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
