# Distributed Backtester

A C++17 engine and distributed runtime for evaluating trading strategies over
historical market data. The same experiment can run locally in parallel or across
coordinator and worker processes over TCP.

## Architecture

```text
experiment JSON
      |
      v
experiment service ──────> local parallel runner
      |
      └───────────────> coordinator  <──TCP──>  stateless workers
                        (queue + state)
```

| Layer | Responsibility | Reference |
| --- | --- | --- |
| Core engine | Market data, strategies, order execution, portfolio accounting, metrics, and ranking. | [`include/backtester/trading`](include/backtester/trading) |
| Experiment service | Versioned JSON requests, grid expansion, local parallel execution, deterministic aggregation, and result JSON. | [`docs/experiments.md`](docs/experiments.md) |
| Coordinator | Worker registration, configurable pending-job scheduling (FIFO by default), retries, job state, timeouts, persistence, and aggregation. | [`docs/coordinator.md`](docs/coordinator.md) |
| Worker | A thin, stateless process that runs one job at a time with the core engine. | [`docs/worker.md`](docs/worker.md) |
| Protocol and transport | Versioned JSON messages over a transport-neutral channel with TCP framing. | [`docs/protocol.md`](docs/protocol.md), [`docs/communication.md`](docs/communication.md) |

### Design principles

- **The coordinator owns job state.** Workers only execute jobs and report one
  result. `pending`, `running`, `completed`, and `failed` never leave the
  coordinator's state table.
- **Dispatch is worker-driven.** A worker takes the next queued job when it
  becomes free, so one slow job does not block idle workers. The default policy
  is FIFO; longest-first, shortest-first, and LIFO can reorder pending work. The
  coordinator can prefetch a small adaptive pipeline per worker to hide the
  dispatch round trip.
- **A job is terminal only when completed or failed.** Retries are bounded
  reassignments with fresh correlation keys.
- **Reliability is coordinator-local.** Transport loss, timeouts, duplicate
  submissions, and restart recovery change scheduling, not the wire protocol.
- **Persistence is opt-in.** In-memory execution is the default; `--state PATH`
  adds a durable checkpoint.

## Capabilities

**Backtesting**

- Loads and validates historical OHLCV CSV data.
- Interchangeable strategies behind a common `on_bar()` interface, with moving
  average crossover, RSI mean reversion, and Donchian channel breakout examples.
- Long-only, fractional-share execution with commission, slippage, warm-up bars,
  and mark-to-market portfolio equity.
- Reports total return, annualized Sharpe ratio, maximum drawdown, trade count,
  costs, latency, throughput, CPU utilization, and ranking.

**Experiments**

- Expands parameter grids into concrete jobs and runs them concurrently with a
  configurable in-flight limit.
- Provides versioned request/response JSON schemas and a service entry point that
  returns structured validation errors.
- Produces the same deterministic response for local and distributed runs.

**Distributed execution**

- Coordinator/worker split over TCP, with an optional stdio transport for local
  debugging.
- FIFO or duration-estimate-based pending-job scheduling, bounded retries,
  reassignment after worker loss, per-job timeouts, and duplicate-result
  suppression.
- Optional durable state with restart recovery; completed runs return without
  waiting for workers.
- Per-job states, attempt counts, retries, timeouts, disconnects, duplicates, and
  recovery counters exposed through `CoordinatorStats`.

## Build and Test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The suite covers core accounting invariants, protocol validation, TCP framing,
parallel execution, coordinator scheduling and recovery, uneven workloads, and
end-to-end executable runs.

## Benchmarks

`benchmarks/benchmark_execution.py` runs the same 240-job sweep sequentially,
across local threads, and across a coordinator plus worker processes, then
writes wall times, throughput, speedups, and latency to
[`benchmarks/results.md`](benchmarks/results.md).

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
python3 benchmarks/benchmark_execution.py
```

For wall-clock, throughput, and speedup across concurrency levels, run
`python3 benchmarks/measure_scaling.py`; its report is
[`benchmarks/scaling.md`](benchmarks/scaling.md). Then run
`python3 benchmarks/analyze_scheduling.py` for scheduling overhead, idle time,
work inflation, and per-worker utilization in
[`benchmarks/scheduling.md`](benchmarks/scheduling.md).

To compare pending-job dispatch orders, run
`python3 benchmarks/compare_scheduling.py --workers 4`. It runs the
heterogeneous 225-job sweep in
[`experiments/heterogeneous_scheduling.json`](experiments/heterogeneous_scheduling.json)
under FIFO, longest-first, shortest-first, and LIFO, and writes makespan,
efficiency, and worker-balance results to
[`benchmarks/scheduling_policies.md`](benchmarks/scheduling_policies.md).

## Running

Local execution is the default:

```bash
./build/backtester --experiment experiments/crossover_sweep.json
./build/backtester --experiment experiments/demo.json --json
cat request.json | ./build/backtester --experiment - --json
```

Distributed execution starts the coordinator first, then the workers:

```bash
# Terminal 1: accept two workers, dispatch, aggregate, and exit.
./build/backtester_coordinator \
  --experiment experiments/crossover_sweep.json \
  --workers 2

# Terminals 2 and 3: connect as clients.
./build/backtester_worker --worker-id worker-a --connect 127.0.0.1:9000
./build/backtester_worker --worker-id worker-b --connect 127.0.0.1:9000

# Retries, persistence, timeouts, and scheduling are coordinator options.
./build/backtester_coordinator \
  --experiment experiments/heterogeneous_scheduling.json --workers 2 \
  --scheduling-policy longest-first
./build/backtester_coordinator \
  --experiment experiments/demo.json --workers 2 \
  --max-attempts 5 --job-timeout 60 --state run-state.json
```

The coordinator prints a text summary by default; add `--json` for the standard
experiment result document. See [`docs/coordinator.md`](docs/coordinator.md) for
all options and failure semantics.

## Project Layout

```text
include/backtester/   Public headers
src/                  Implementations and the three executables
  main.cpp            Local runner
  coordinator_main.cpp
  worker_main.cpp
tests/                Unit, integration, and end-to-end tests
experiments/          Example experiment requests
benchmarks/           Execution-mode benchmark harness and captured results
schema/               Request, response, and protocol JSON Schemas
examples/protocol/    Example protocol messages
docs/                 Architecture and reference documentation
```

Public headers are included relative to `include/backtester`, for example
`#include "trading/backtest_engine.h"` or `#include "coordinator/coordinator.h"`.

## Documentation

- [`docs/experiments.md`](docs/experiments.md) — request/response fields, local
  execution, job IDs, and service integration.
- [`docs/coordinator.md`](docs/coordinator.md) — distributed architecture,
  scheduling, state, retries, timeouts, persistence, and recovery.
- [`docs/worker.md`](docs/worker.md) — worker lifecycle, error mapping, and
  transports.
- [`docs/protocol.md`](docs/protocol.md) — wire messages, envelope, validation,
  and versioning.
- [`docs/communication.md`](docs/communication.md) — channel contract, TCP
  framing, and transport boundaries.
- [`schema/`](schema) — JSON Schemas; [`examples/protocol/`](examples/protocol) —
  example messages.

## Current Boundaries

Version 1 assumes every process sees the dataset at the same path and runs on a
trusted network. See the limitations sections of
[`docs/coordinator.md`](docs/coordinator.md#limitations) and
[`docs/worker.md`](docs/worker.md#what-is-not-here-yet) for the current
constraints.
