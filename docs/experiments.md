# JSON experiment schema

Experiments are defined as JSON documents. The same document is used by the
command-line runner and by the `run_experiment_json()` service entry point, so
an experiment can be reproduced locally or submitted to a future HTTP service
without translation.

- Request schema: [`schema/experiment.schema.json`](../schema/experiment.schema.json)
- Response schema: [`schema/experiment_result.schema.json`](../schema/experiment_result.schema.json)
- Examples: [`experiments/`](../experiments)

The C++ parser is the source of truth for validation. The JSON Schema files
provide editor support and describe the wire contract; they do not replace the
runtime checks.

## Versioning

Every document declares `"schema_version": 1`. The backtester rejects any
other value. Additive, backward-compatible fields may be added to version 1;
breaking changes will bump the version.

Experiment documents use `schema_version`. Worker protocol messages use a
separate `protocol_version`; see [`docs/protocol.md`](protocol.md).

## Request

```json
{
  "$schema": "../schema/experiment.schema.json",
  "schema_version": 1,
  "experiment_id": "crossover-sweep",
  "description": "Optional free-form note.",
  "data": {
    "path": "data/market/sp500_1950_2018.csv",
    "asset": "SP500",
    "start_date": "2000-01-01",
    "end_date": "2010-12-31"
  },
  "execution": {
    "initial_cash": 10000.0,
    "transaction_cost_rate": 0.001,
    "slippage_rate": 0.0005,
    "max_concurrent_jobs": 0
  },
  "strategies": [
    {
      "id": "crossover",
      "type": "moving_average_crossover",
      "grid": {
        "short_window": [20, 50],
        "long_window": [100, 200]
      }
    }
  ],
  "output": {
    "include_trades": false
  }
}
```

### Top-level fields

| Field | Required | Description |
| --- | --- | --- |
| `$schema` | No | Optional URI for editor tooling. Ignored by the runner. |
| `schema_version` | Yes | Must be `1`. |
| `experiment_id` | Yes | Caller-supplied ID, echoed in the response. Must match `^[A-Za-z0-9][A-Za-z0-9._-]*$`. |
| `description` | No | Free-form description. |
| `data` | Yes | Market data source and backtest date range. |
| `execution` | No | Cash, costs, and scheduling. Defaults shown below. |
| `strategies` | Yes | One or more strategy declarations. Must be non-empty. |
| `output` | No | Response-shaping options. |

### `data`

| Field | Required | Description |
| --- | --- | --- |
| `path` | Yes | OHLCV CSV path. Resolved relative to the backtester process working directory. |
| `asset` | Yes | Symbol used for orders and portfolio accounting. |
| `start_date` | Yes | First bar in the backtest, `YYYY-MM-DD`. Earlier bars are still visible to strategies as warm-up. |
| `end_date` | Yes | Last bar in the backtest, `YYYY-MM-DD`. Must not precede `start_date`. |

### `execution`

| Field | Default | Description |
| --- | --- | --- |
| `initial_cash` | `10000.0` | Starting cash. Must be positive. |
| `transaction_cost_rate` | `0.0` | Commission as a fraction of traded notional (`0.001` = 10 bps). Must be in `[0, 1)`. |
| `slippage_rate` | `0.0` | Adverse price movement per fill as a fraction of the bar open (`0.0005` = 5 bps). Must be in `[0, 1)`. |
| `max_concurrent_jobs` | `0` | Maximum in-flight jobs. `0` means hardware concurrency. Values above 1024 are rejected. |

### `output`

| Field | Default | Description |
| --- | --- | --- |
| `include_trades` | `false` | Include every filled trade per result. Off by default because sweeps can produce large responses. |

### `strategies`

Each entry has:

| Field | Required | Description |
| --- | --- | --- |
| `id` | Yes | Strategy identifier. Must match `^[A-Za-z0-9][A-Za-z0-9_-]*$` and be unique within the experiment. |
| `type` | Yes | One of `moving_average_crossover`, `rsi_mean_reversion`, `donchian_channel_breakout`. |
| `grid` | One of `grid` / `parameters` | Map of parameter name to a non-empty list of values. The Cartesian product becomes the jobs for this strategy. |
| `parameters` | One of `grid` / `parameters` | A single explicit parameter map. |

Exactly one of `grid` and `parameters` must be supplied. Unknown parameter
names are rejected so typos do not silently run the wrong sweep.

#### Strategy parameters

| Type | Parameters | Rules |
| --- | --- | --- |
| `moving_average_crossover` | `short_window`, `long_window` | Positive integers; `short_window < long_window`. |
| `rsi_mean_reversion` | `rsi_period`, `oversold_threshold`, `exit_threshold` | `rsi_period` positive integer; thresholds in `[0, 100]`; `oversold_threshold < exit_threshold`. |
| `donchian_channel_breakout` | `entry_period`, `exit_period` | Positive integers. |

Periods may be written as `20` or `20.0`; non-integral values are rejected.

### Job IDs and ordering

A strategy with `id` `"rsi"` and four parameter combinations produces jobs
`rsi-1` through `rsi-4`. IDs are deterministic and 1-based within each
strategy.

Strategies are expanded independently and then interleaved round-robin:
`ma-1`, `rsi-1`, `don-1`, `ma-2`, `rsi-2`, `don-2`, ... This ensures a bounded
execution window contains a mix of strategies.

## Response

```json
{
  "schema_version": 1,
  "status": "completed",
  "experiment_id": "crossover-sweep",
  "data": {
    "path": "data/market/sp500_1950_2018.csv",
    "asset": "SP500",
    "start_date": "2000-01-01",
    "end_date": "2010-12-31",
    "trading_days": 17346,
    "first_date": "1950-01-03",
    "last_date": "2018-12-07"
  },
  "execution": {
    "initial_cash": 10000.0,
    "transaction_cost_rate": 0.001,
    "slippage_rate": 0.0005,
    "max_concurrent_jobs": 8
  },
  "summary": {
    "jobs_total": 4,
    "jobs_completed": 4,
    "jobs_failed": 0,
    "wall_seconds": 0.11,
    "best_job_id": "crossover-3",
    "best_sharpe_ratio": 0.21,
    "best_total_return": 0.19,
    "performance": {
      "max_concurrent": 8,
      "throughput": 36.4,
      "mean_latency": 0.02,
      "median_latency": 0.02,
      "min_latency": 0.01,
      "max_latency": 0.03,
      "p95_latency": 0.03,
      "total_work_seconds": 0.08,
      "speedup": 0.73,
      "cpu_seconds": 0.09,
      "cpu_utilization": 81.8,
      "cpu_efficiency": 10.2
    }
  },
  "results": [
    {
      "job_id": "crossover-1",
      "strategy_id": "crossover",
      "strategy_type": "moving_average_crossover",
      "status": "completed",
      "duration_seconds": 0.02,
      "parameters": {
        "long_window": 100,
        "short_window": 20
      },
      "metrics": {
        "total_return": 0.1241,
        "sharpe_ratio": 0.1515,
        "max_drawdown": 0.2917,
        "number_of_trades": 18,
        "total_costs": 483.99
      }
    }
  ]
}
```

Results are returned in execution-plan order (the interleaved job order), not
ranked. `summary.best_*` identifies the highest Sharpe ratio, with total return
as the tie-breaker.

`status` values:

| Status | Meaning |
| --- | --- |
| `completed` | Every job completed. |
| `partial` | At least one job failed and at least one completed. |
| `failed` | No job completed. |
| `invalid` | The request was rejected before execution (parse or validation error). |
| `failed` (error envelope) | The request was valid but execution could not start, for example the data file was missing. |

A failed job includes an `error` string and no `metrics`. Per-job failures do
not abort the rest of the sweep.

### Error envelope

Validation and parse failures return a document instead of throwing:

```json
{
  "schema_version": 1,
  "status": "invalid",
  "error": {
    "type": "validation_error",
    "path": "$.strategies[0].grid.short_window",
    "message": "Expected a positive integer"
  }
}
```

Error types are `json_parse_error`, `validation_error`, and `runtime_error`.
`error.path` uses a JSONPath-like expression rooted at `$`.

## Running from the command line

```bash
# Built-in three-strategy demo (text output)
./build/backtester

# JSON experiment, human-readable output
./build/backtester --experiment experiments/crossover_sweep.json

# JSON experiment, machine-readable response
./build/backtester --experiment experiments/crossover_sweep.json --json

# Read the request from stdin (service-style invocation)
cat request.json | ./build/backtester --experiment - --json
```

JSON responses are written to stdout with no extra text, so stdout can be piped
directly into a JSON parser. With `--json`, parse/validation errors still exit
with status `0` and produce a structured error envelope; use the envelope's
`status` field to distinguish success from failure. Without `--json`, invalid
requests exit with status `1` and print a diagnostic to stderr.

## Calling the backtester as a service

`run_experiment_json()` in
[`include/backtester/experiment/experiment.h`](../include/backtester/experiment/experiment.h)
is the transport-neutral entry point:

```cpp
#include "experiment/experiment.h"

// request_body is the raw JSON request document.
std::string response_body = run_experiment_json(request_body, /*pretty=*/false);
```

It never throws for request or data errors: it always returns a
schema-shaped JSON response. A future HTTP server can be a thin adapter:

```text
POST /experiments
  body   -> run_experiment_json(body)
  200    -> response with status "completed" | "partial" | "failed"
  400    -> response with status "invalid"
  500    -> response with status "failed" and error.type "runtime_error"
```

The lower-level API is also available when a service wants to inspect results
without a JSON round trip:

```cpp
ExperimentRequest request = parse_experiment_json(body); // throws on invalid
ExperimentResult result = run_experiment(request);       // loads data + runs
```

### Operational notes for a hosted service

- `data.path` is resolved by the backtester process. Restrict it to a
  configured data root (or replace it with a registered dataset ID) before
  exposing the endpoint publicly; the current CLI accepts any readable path.
- Cap `strategies[].grid` sizes and request body size at the HTTP layer. The
  core validates each parameter but does not impose a maximum job count.
- `max_concurrent_jobs` only bounds concurrency within one request. A service
  should also bound how many requests it runs at once.
- `include_trades` can make responses very large; keep it off for sweeps.
- Use `experiment_id` for request correlation and logging. It is echoed in the
  response but is not required to be globally unique.

## Examples

| File | Demonstrates |
| --- | --- |
| [`experiments/demo.json`](../experiments/demo.json) | The full 403-job three-strategy demo. |
| [`experiments/crossover_sweep.json`](../experiments/crossover_sweep.json) | A small parameter grid. |
| [`experiments/rsi_sweep.json`](../experiments/rsi_sweep.json) | Explicit execution costs and concurrency. |
| [`experiments/donchian_breakout.json`](../experiments/donchian_breakout.json) | A single explicit configuration with trades included. |

## Distributed execution

An experiment document is the coordinator's input. Once expanded, each job can
be sent to a worker as a `backtest_job` message, and the worker replies with a
`backtest_result` that echoes the job's `message_id`, identity, and parameters.
The wire contract is documented in [`docs/protocol.md`](protocol.md) and defined
by [`schema/backtest_job.schema.json`](../schema/backtest_job.schema.json) and
[`schema/backtest_result.schema.json`](../schema/backtest_result.schema.json).
The TCP transport and scheduling layer are not implemented yet.
