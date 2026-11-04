# Distributed Backtester

A C++17 framework for evaluating trading strategies against historical market data, with plans for scalable experiment execution.

## Features

- **Market data loader:** Parses and validates historical OHLCV data from CSV files.
- **Strategy interface:** Interchangeable strategies through a common `on_bar()` interface.
- **Execution pipeline:** Strategies emit `Signal` intent; the engine sizes `Order`s, fills them into `Trade`s (applying slippage and commission), and applies trades to the portfolio.
- **Moving-average crossover:** Example long-only strategy using short- and long-term moving averages.
- **RSI mean reversion:** Example long-only strategy that buys when the Relative Strength Index (RSI) falls below an oversold threshold and sells when it rises above an exit threshold.
- **Donchian channel breakout:** Example long-only strategy that buys when the close breaks above the highest high of the previous `entry_period` bars and sells when it falls below the lowest low of the previous `exit_period` bars.
- **Backtesting engine:** Simulates trades and tracks portfolio equity over a date range, including warm-up bars.
- **Portfolio accounting:** Cash, per-symbol positions with average cost, and mark-to-market equity; buys and sells update the holding identified by symbol.
- **Parameter grid:** Cartesian product of parameter combinations, each wired to a `BacktestConfig` and run concurrently through `run_backtest()`, bounded by configurable concurrency. The demo runs the moving-average crossover, RSI mean-reversion, and Donchian channel breakout strategies side by side, sweeping each strategy's parameters.
- **JSON experiment schema:** Versioned request documents define the dataset, date range, execution costs, strategy grids, and output options. A matching response schema describes the machine-readable result.
- **Service-ready entry point:** `run_experiment_json()` accepts a request document and always returns a schema-shaped response, including structured validation errors. The CLI exposes the same path via `--experiment` and stdin.
- **Worker protocol:** Versioned JSON messages for availability (`worker_ready`), coordinator-to-worker jobs (`backtest_job`), worker-to-coordinator results (`backtest_result`), shutdown (`shutdown`), and errors (`worker_error`), with strict parsing, validation, and JSON Schemas. Messages travel over TCP using length-prefixed frames.
- **Standalone worker:** `backtester_worker` connects to a coordinator with `--connect HOST:PORT`, announces `worker_ready`, then receives one job at a time, runs the existing engine, and replies with one result. It keeps serving until it receives a `shutdown` message or its connection closes. The same loop also runs over a line-oriented stdio transport for local debugging.
- **Distributed coordinator:** `backtester_coordinator` is a TCP server that accepts workers, expands an experiment into jobs, assigns each job to an available worker, collects results, and aggregates them into the normal experiment response. Slow jobs do not block other workers.
- **Performance stats:** Throughput, latency (mean/median/min/max/p95), CPU utilization, CPU efficiency, and speedup measured per run.
- **Ranking:** Results ranked by Sharpe ratio (highest first), with a best-configuration summary.
- **Performance metrics:** Total return, annualized Sharpe ratio, maximum drawdown, completed round trips, executed fills, and total trading costs.
- **Automated tests:** Unit, integration, accounting-invariant, parallel-execution, TCP framing, distributed coordinator/worker, and end-to-end tests.

## Technical Details

- **Language:** C++17
- **Build system:** CMake
- **Data:** Daily S&P 500 index OHLCV data (1950–2018)
- **Trading model:** Long-only, fractional shares. Strategies emit signals at a bar's close; the engine converts each signal into an order sized against the current portfolio, fills it at the next bar's open with configurable commission (`transaction_cost_rate`) and adverse slippage (`slippage_rate`), and applies the resulting trade. Open positions are marked to market at the final close.

## Project Layout

```text
include/backtester/
  core/          Market data, JSON values, identifiers, dates, parameter grids, stats
  trading/       Strategy interface, portfolio, execution, backtest engine
  strategies/    Built-in trading strategies
  experiment/    Versioned experiment schema and service entry point
  protocol/      Coordinator <-> worker wire message types
  net/           TCP listening, connecting, and length-prefixed framing
  comm/          Transport-neutral one-document-per-call channel contract
  worker/        Worker transport interface, TCP/stdio transports, job execution
  coordinator/   Worker registration, job dispatch, and result aggregation
src/
  core/          Implementations of the core utilities
  trading/       Implementations of the trading model
  strategies/    Implementations of the built-in strategies
  experiment/    Request parsing, validation, execution, and result JSON
  protocol/      Job/result parsing, validation, and serialization
  net/           TCP socket and framing implementation
  comm/          TCP channel implementations
  worker/        Worker loop, job execution, and stdio/TCP transports
  coordinator/   TCP server and distributed scheduling loop
  main.cpp       Local CLI entry point
  worker_main.cpp        Worker executable entry point
  coordinator_main.cpp   Coordinator executable entry point
tests/
  core/          Unit tests for core utilities
  trading/       Unit and accounting tests for the trading model
  strategies/    Unit tests for each strategy
  experiment/    Schema, service, and parallel-execution tests
  protocol/      Wire message parsing, validation, and round-trip tests
  net/           TCP framing and endpoint tests
  comm/          Communication-layer contract and channel tests
  worker/        Worker loop, job execution, and transport tests
  integration/   Real-data, protocol-example, worker, coordinator, pipeline, and end-to-end tests
```

Public headers are included relative to `include/backtester`, for example
`#include "trading/backtest_engine.h"`,
`#include "experiment/experiment.h"`,
`#include "protocol/protocol.h"`, or
`#include "worker/worker.h"`.

## Build and Test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Running Experiments

The built-in demo runs with no arguments. To run a JSON-defined experiment:

```bash
./build/backtester --experiment experiments/crossover_sweep.json
./build/backtester --experiment experiments/demo.json --json
cat request.json | ./build/backtester --experiment - --json
```

The request and response contracts, field reference, job-ID rules, and
service-integration notes are in [`docs/experiments.md`](docs/experiments.md).
The JSON Schemas live in [`schema/`](schema) and example requests in
[`experiments/`](experiments).

## Distributed Execution

The coordinator is a TCP server and each worker is a TCP client. Both use the
same transport-neutral `comm::MessageChannel` contract: one JSON document per
send/receive, with framing and connection lifecycle owned by the channel. The
coordinator expands an experiment into `backtest_job` messages and queues them in
request order for FIFO distribution: a worker is available exactly when it has
finished its previous job and takes the oldest queued job. The worker runs the
existing engine and replies with a `backtest_result`. When the queue is empty the
coordinator sends `shutdown` to every worker and aggregates the results into the
same experiment response produced by a local run.

```bash
# Terminal 1: coordinator. It waits for two workers before dispatching.
./build/backtester_coordinator \
  --experiment experiments/crossover_sweep.json \
  --workers 2

# Terminal 2 and 3: workers connect as clients.
./build/backtester_worker --worker-id worker-a --connect 127.0.0.1:9000
./build/backtester_worker --worker-id worker-b --connect 127.0.0.1:9000

# Machine-readable output uses the standard experiment result schema.
./build/backtester_coordinator \
  --experiment experiments/demo.json --workers 2 --json
```

TCP frames are a 4-byte big-endian payload length followed by one UTF-8 JSON
document, with an 8 MiB limit. On a local machine the default listen address is
`127.0.0.1:9000`; use `--listen 0.0.0.0:9000` to accept remote workers. Version 1
assumes every process sees the dataset at the same path.

The worker is a thin wrapper around the existing engine: it deserializes a job,
validates the strategy and parameters, loads the data, calls `run_backtest()`,
and serializes the result. It loops for another job until it receives a
`shutdown` message or loses its connection.

See [`docs/coordinator.md`](docs/coordinator.md) for scheduling and failure
handling, [`docs/communication.md`](docs/communication.md) for the channel
contract and framing, [`docs/worker.md`](docs/worker.md) for the worker
lifecycle, and [`docs/protocol.md`](docs/protocol.md) for the wire contract.
Message schemas live in [`schema/`](schema) and protocol examples in
[`examples/protocol/`](examples/protocol).
