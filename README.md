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
- **Parameter grid:** Cartesian product of parameter combinations, each wired to a `BacktestConfig` and run concurrently through `run_backtest()`, bounded by hardware concurrency. The demo runs the moving-average crossover, RSI mean-reversion, and Donchian channel breakout strategies side by side, sweeping each strategy's parameters.
- **Performance stats:** Throughput, latency (mean/median/min/max/p95), CPU utilization, CPU efficiency, and speedup measured per run.
- **Ranking:** Results ranked by Sharpe ratio (highest first), with a best-configuration summary.
- **Performance metrics:** Total return, annualized Sharpe ratio, maximum drawdown, completed round trips, executed fills, and total trading costs.
- **Automated tests:** Unit, integration, accounting-invariant, parallel-execution, and end-to-end tests.

## Technical Details

- **Language:** C++17
- **Build system:** CMake
- **Data:** Daily S&P 500 index OHLCV data (1950–2018)
- **Trading model:** Long-only, fractional shares. Strategies emit signals at a bar's close; the engine converts each signal into an order sized against the current portfolio, fills it at the next bar's open with configurable commission (`transaction_cost_rate`) and adverse slippage (`slippage_rate`), and applies the resulting trade. Open positions are marked to market at the final close.

## Build and Test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```
