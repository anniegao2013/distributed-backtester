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
- **Performance stats:** Throughput, latency (mean/median/min/max/p95), CPU utilization, CPU efficiency, and speedup measured per run.
- **Ranking:** Results ranked by Sharpe ratio (highest first), with a best-configuration summary.
- **Performance metrics:** Total return, annualized Sharpe ratio, maximum drawdown, completed round trips, executed fills, and total trading costs.
- **Automated tests:** Unit, integration, accounting-invariant, parallel-execution, and end-to-end tests.

## Technical Details

- **Language:** C++17
- **Build system:** CMake
- **Data:** Daily S&P 500 index OHLCV data (1950–2018)
- **Trading model:** Long-only, fractional shares. Strategies emit signals at a bar's close; the engine converts each signal into an order sized against the current portfolio, fills it at the next bar's open with configurable commission (`transaction_cost_rate`) and adverse slippage (`slippage_rate`), and applies the resulting trade. Open positions are marked to market at the final close.

## Project Layout

```text
include/backtester/
  core/          Market data, JSON values, parameter grids, performance stats
  trading/       Strategy interface, portfolio, execution, backtest engine
  strategies/    Built-in trading strategies
  experiment/    Versioned experiment schema and service entry point
src/
  core/          Implementations of the core utilities
  trading/       Implementations of the trading model
  strategies/    Implementations of the built-in strategies
  experiment/    Request parsing, validation, execution, and result JSON
  main.cpp       CLI entry point
tests/
  core/          Unit tests for core utilities
  trading/       Unit and accounting tests for the trading model
  strategies/    Unit tests for each strategy
  experiment/    Schema, service, and parallel-execution tests
  integration/   Real-data, pipeline, and end-to-end tests
```

Public headers are included relative to `include/backtester`, for example
`#include "trading/backtest_engine.h"` or
`#include "experiment/experiment.h"`.

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
