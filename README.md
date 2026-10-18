# Distributed Backtester

A C++17 framework for evaluating trading strategies against historical market data, with plans for scalable experiment execution.

## Features

- **Market data loader:** Parses and validates historical OHLCV data from CSV files.
- **Strategy interface:** Interchangeable strategies through a common `on_bar()` interface.
- **Moving-average crossover:** Example long-only strategy using short- and long-term moving averages.
- **Backtesting engine:** Simulates trades and tracks portfolio equity over a date range, including warm-up bars.
- **Parameter grid:** Cartesian product of parameter combinations, each wired to a `BacktestConfig` and run concurrently through `run_backtest()`, bounded by hardware concurrency.
- **Performance stats:** Throughput, latency (mean/median/min/max/p95), CPU utilization, CPU efficiency, and speedup measured per run.
- **Ranking:** Results ranked by Sharpe ratio (highest first), with a best-configuration summary.
- **Performance metrics:** Total return, annualized Sharpe ratio, maximum drawdown, and completed trades.
- **Automated tests:** Unit, integration, parallel-execution, and end-to-end tests.

## Technical Details

- **Language:** C++17
- **Build system:** CMake
- **Data:** Daily S&P 500 index OHLCV data (1950–2018)
- **Execution:** Signals generated at a bar's close execute at the next bar's open.
- **Trading model:** Long-only, fractional shares, no transaction costs or slippage. Open positions are marked to market at the final close.

## Build and Test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```
