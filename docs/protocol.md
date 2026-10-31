# Worker protocol

The protocol layer defines the JSON messages that a future experiment
coordinator and backtest workers will exchange. It is transport-neutral: this
repository currently defines and tests the messages, but no TCP server, client,
scheduler, or retry loop exists yet.

- Job schema: [`schema/backtest_job.schema.json`](../schema/backtest_job.schema.json)
- Result schema: [`schema/backtest_result.schema.json`](../schema/backtest_result.schema.json)
- Examples: [`examples/protocol/`](../examples/protocol)
- C++ API: [`include/backtester/protocol/protocol.h`](../include/backtester/protocol/protocol.h)

The C++ parser and validator are the source of truth. The JSON Schema files
describe the same contract for editors and non-C++ producers.

## Message flow

```text
experiment request
  -> coordinator expands strategies into jobs
  -> one backtest_job per job, each with a unique message_id
  -> worker validates and runs the backtest
  -> one backtest_result per job, echoing message_id
  -> coordinator aggregates results into the experiment response
```

A job is atomic. A worker returns exactly one `backtest_result` for every
readable `backtest_job` envelope, including jobs it rejects before execution.
A `protocol_error` is reserved for messages whose envelope cannot be processed
at all.

## Envelope

Every message has these top-level fields:

| Field | Required | Description |
| --- | --- | --- |
| `$schema` | No | Optional URI for editor tooling. Ignored by the parser. |
| `protocol_version` | Yes | Must be `1`. |
| `type` | Yes | `backtest_job`, `backtest_result`, or `protocol_error`. |
| `message_id` | Job/result: yes; error: no | Correlation key. Starts with a letter or digit, then letters, digits, `.`, `_`, `-`, or `:`; at most 128 characters. |

`message_id` is chosen by the sender and echoed by the receiver. It is the
correlation key, not the job identity: the coordinator may retry the same job
with a new `message_id`, or reuse it so a worker can cache and deduplicate the
reply. `job_id` is deterministic within an experiment (`ma-1`, `ma-2`, ...) but
is not globally unique, so use `(experiment_id, job_id)` for reporting and
`message_id` for wire correlation.

## `backtest_job`

One fully-expanded backtest. Everything needed to run it travels with the
message; the worker never consults coordinator state.

```json
{
  "$schema": "../schema/backtest_job.schema.json",
  "protocol_version": 1,
  "type": "backtest_job",
  "message_id": "crossover-sweep:crossover-1:0001",
  "job": {
    "experiment_id": "crossover-sweep",
    "job_id": "crossover-1",
    "strategy_id": "crossover",
    "strategy_type": "moving_average_crossover",
    "parameters": {
      "short_window": 20,
      "long_window": 100
    },
    "data": {
      "path": "data/market/sp500_1950_2018.csv",
      "asset": "SP500",
      "start_date": "2000-01-01",
      "end_date": "2010-12-31"
    },
    "execution": {
      "initial_cash": 10000.0,
      "transaction_cost_rate": 0.001,
      "slippage_rate": 0.0005
    },
    "output": {
      "include_trades": false
    }
  }
}
```

### `job`

| Field | Required | Description |
| --- | --- | --- |
| `experiment_id` | Yes | Experiment that produced the job. Matches `^[A-Za-z0-9][A-Za-z0-9._-]*$`. |
| `job_id` | Yes | Deterministic job ID. Matches `^[A-Za-z0-9][A-Za-z0-9_-]*$`. |
| `strategy_id` | Yes | Strategy declaration ID. Same character rules as `job_id`. |
| `strategy_type` | Yes | Strategy registry name. The worker validates it and the parameter set before running. |
| `parameters` | Yes | One concrete parameter combination, not a grid. Values are finite numbers. |
| `data` | Yes | Dataset reference and date range. |
| `execution` | No | Cash and cost settings. Omitted fields use the defaults below. |
| `output` | No | Result-shaping options. |

### `data`

| Field | Required | Description |
| --- | --- | --- |
| `path` | Yes | OHLCV CSV path, resolved **in the worker's environment**. |
| `asset` | Yes | Symbol used for orders and portfolio accounting. |
| `start_date` | Yes | First bar in the backtest, `YYYY-MM-DD`. Earlier bars remain visible as warm-up. |
| `end_date` | Yes | Last bar, `YYYY-MM-DD`. Must not precede `start_date`. |

Distributed data placement is deliberately outside version 1 of the protocol:
workers must see the file at the same path, for example through a shared
volume. A later version can add content hashes or object-store references
without changing the rest of the message.

### `execution`

| Field | Default | Description |
| --- | --- | --- |
| `initial_cash` | `10000.0` | Starting cash. Must be positive. |
| `transaction_cost_rate` | `0.0` | Commission as a fraction of traded notional. Must be in `[0, 1)`. |
| `slippage_rate` | `0.0` | Adverse price movement per fill as a fraction of the bar open. Must be in `[0, 1)`. |

Concurrency (`max_concurrent_jobs` in the experiment document) is a coordinator
concern and is intentionally absent from a job.

### `output`

| Field | Default | Description |
| --- | --- | --- |
| `include_trades` | `false` | Ask the worker to return every filled trade. Off by default because large sweeps would otherwise produce very large messages. |

### Mapping an experiment to a job

The coordinator expands the experiment document before sending anything:

| Experiment document | Job message |
| --- | --- |
| `experiment_id` | `job.experiment_id` (echoed by the result) |
| `data` | `job.data` |
| `execution.initial_cash` | `job.execution.initial_cash` |
| `execution.transaction_cost_rate` | `job.execution.transaction_cost_rate` |
| `execution.slippage_rate` | `job.execution.slippage_rate` |
| `execution.max_concurrent_jobs` | Not sent; scheduling stays with the coordinator |
| `output.include_trades` | `job.output.include_trades` |
| Each expanded strategy combination | `job.job_id`, `job.strategy_id`, `job.strategy_type`, `job.parameters` |

A grid is expanded to concrete parameter sets before jobs are sent. The worker
never receives a grid and never chooses a parameter combination.

## `backtest_result`

A result echoes the job's identity and parameters, so the coordinator can
aggregate results without retaining the request.

```json
{
  "$schema": "../schema/backtest_result.schema.json",
  "protocol_version": 1,
  "type": "backtest_result",
  "message_id": "crossover-sweep:crossover-1:0001",
  "worker_id": "worker-a",
  "result": {
    "experiment_id": "crossover-sweep",
    "job_id": "crossover-1",
    "strategy_id": "crossover",
    "strategy_type": "moving_average_crossover",
    "parameters": {
      "short_window": 20,
      "long_window": 100
    },
    "status": "completed",
    "duration_seconds": 0.022,
    "metrics": {
      "total_return": 0.1241,
      "sharpe_ratio": 0.1515,
      "max_drawdown": 0.2917,
      "number_of_trades": 18,
      "total_costs": 483.99
    }
  }
}
```

| Field | Required | Description |
| --- | --- | --- |
| `message_id` | Yes | Echo of the job's `message_id`. |
| `worker_id` | No | Worker identity, for logs and diagnostics. Same character rules as `message_id`. |
| `result` | Yes | Outcome of the job. |

### `result.status`

| Status | Required fields | Meaning |
| --- | --- | --- |
| `completed` | `metrics`; no `error`; `trades` optional | The backtest ran. |
| `failed` | `error.type` = `runtime_error`; no `metrics` or `trades` | Execution failed, for example the data file was missing. |
| `invalid` | `error.type` = `validation_error`; no `metrics` or `trades` | The job was structurally valid but failed worker-side strategy validation. |

`duration_seconds` is worker-side execution time and must be non-negative. It
does not include queueing or network time; the coordinator measures those
separately.

`error.path` uses a JSONPath-like expression rooted at `$`, for example
`$.job.parameters.short_window`.

## `protocol_error`

Returned when the envelope itself cannot be processed. The worker should not
attempt to parse a job payload in this case.

```json
{
  "$schema": "../schema/backtest_result.schema.json",
  "protocol_version": 1,
  "type": "protocol_error",
  "message_id": "crossover-sweep:crossover-1:0001",
  "error": {
    "type": "unsupported_version",
    "path": "$.protocol_version",
    "message": "Unsupported protocol_version; this build speaks version 1"
  }
}
```

`message_id` is echoed only when it was readable. Error types:

| Type | Meaning |
| --- | --- |
| `json_parse_error` | The bytes were not a single valid JSON document. |
| `unsupported_version` | `protocol_version` was missing or not `1`. |
| `unknown_message_type` | `type` was not one of the three known values. |
| `validation_error` | The envelope was readable but malformed, for example a missing `message_id`. |

A future server should map this to a best-effort reply and then decide whether
to keep the connection open. A message whose `protocol_version` is unknown may
use a different envelope entirely, so closing the connection is also
reasonable.

## Validation boundaries

The protocol parser validates:

- envelope version, type, and correlation fields;
- required fields and unknown-field rejection at every level;
- types, finite numbers, date formats, and date ordering;
- identifier and parameter-map shapes;
- result status/error consistency.

The worker validates:

- that `strategy_type` exists in the strategy registry;
- required strategy parameters, value ranges, and cross-parameter rules;
- dataset availability and content.

An unknown strategy type or bad parameter set produces `status: "invalid"` with
`error.type: "validation_error"`, not a `protocol_error`.

## Versioning

Every message declares `"protocol_version": 1`. Additive, backward-compatible
fields may be added within version 1; breaking changes bump the version.
Receivers must reject unsupported versions with a `protocol_error` of type
`unsupported_version` when they can still read the envelope. `parse_protocol_envelope()`
does not enforce the supported version; it reports the version it saw so the
server can echo `message_id` in that reply. The type-specific parsers and
`parse_protocol_message_json()` do enforce version 1.

## Transport notes

The protocol does not define framing yet. The future TCP adapter must preserve
one JSON document per message and must handle partial reads and writes. Likely
choices are a length-prefixed frame or newline-delimited JSON; whichever is
chosen should be documented with the adapter and enforced with a maximum
message size.

The transport will also own concerns that are deliberately not in the message:

- connection lifecycle and worker registration;
- timeouts, retry, and duplicate suppression by `message_id`;
- backpressure and bounded in-flight jobs;
- authentication and encryption;
- logging and metrics around queue, network, and execution time.

## C++ usage

```cpp
#include "protocol/protocol.h"

// Coordinator side.
BacktestJobMessage job;
job.message_id = "crossover-sweep:crossover-1:0001";
job.job = { /* ... */ };
const std::string request = backtest_job_to_json(job, /*pretty=*/false);

// Worker side.
try {
    const ProtocolEnvelope envelope = parse_protocol_envelope(request);

    if (envelope.protocol_version != kBacktestProtocolVersion) {
        // Reply with make_protocol_error("unsupported_version", ...,
        //                                "$.protocol_version",
        //                                envelope.message_id).
    } else if (envelope.type == "backtest_job") {
        const BacktestJobMessage parsed =
            parse_backtest_job_json(request);
        // Run parsed.job, then build a BacktestResultMessage.
    }
} catch (const JsonParseError& error) {
    // Reply with make_protocol_error("json_parse_error", error.what(), ...).
} catch (const ProtocolValidationError& error) {
    // Reply with make_protocol_error("validation_error", error.what(),
    //                                error.path(), ...).
}
```

`parse_protocol_message_json()` parses and dispatches all three message types in
one call. The type-specific parsers are useful when the transport already knows
what to expect, or when it wants a precise error for a mismatched `type`.
