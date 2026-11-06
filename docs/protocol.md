# Worker protocol

The protocol layer defines the JSON messages that an experiment coordinator and
backtest workers exchange. It is transport-neutral: this repository defines,
tests, and implements the message flow over TCP. The coordinator is the TCP
server, workers are TCP clients, and [`backtester_worker`](worker.md) can dial
the coordinator with `--connect`. A line-oriented stdio transport remains
available for local development.

- Worker ready schema: [`schema/worker_ready.schema.json`](../schema/worker_ready.schema.json)
- Job schema: [`schema/backtest_job.schema.json`](../schema/backtest_job.schema.json)
- Result schema: [`schema/backtest_result.schema.json`](../schema/backtest_result.schema.json)
- Shutdown schema: [`schema/shutdown.schema.json`](../schema/shutdown.schema.json)
- Worker error schema: [`schema/worker_error.schema.json`](../schema/worker_error.schema.json)
- Examples: [`examples/protocol/`](../examples/protocol)
- C++ API: [`include/backtester/protocol/protocol.h`](../include/backtester/protocol/protocol.h)

The C++ parser and validator are the source of truth. The JSON Schema files
describe the same contract for editors and non-C++ producers.

## Message catalog

| Type | Direction | Purpose |
| --- | --- | --- |
| `worker_ready` | worker → coordinator | The worker is available and identifies itself. |
| `backtest_job` | coordinator → worker | Assign one fully-expanded backtest. |
| `backtest_result` | worker → coordinator | Return the completed, failed, or invalid result of one job. |
| `shutdown` | coordinator → worker | Ask the worker to terminate after the current job. |
| `worker_error` | worker → coordinator | Report an execution or protocol failure. |

`protocol_error` is a deprecated input alias of `worker_error`. A parser accepts
either type name; a serializer always emits `worker_error`.

## Message flow

```text
experiment request
  -> coordinator starts a TCP server and listens
  -> coordinator expands strategies into jobs
  -> workers connect independently and each sends one worker_ready
  -> coordinator accepts the configured number of workers
  -> one backtest_job per job, each with a unique message_id
  -> worker validates and runs the backtest
  -> one backtest_result per job, echoing message_id
  -> coordinator assigns the next job to that worker
  -> coordinator aggregates results into the experiment response
  -> coordinator sends one shutdown when the run is complete
  -> worker finishes the current job and closes the connection
```

A job is atomic. A worker returns exactly one `backtest_result` for every
readable `backtest_job` envelope, including jobs it rejects before execution. A
`worker_error` is reserved for messages whose envelope cannot be processed and
for worker-level failures that cannot be represented as a job result. A
`shutdown` is a control message and produces no reply. `worker_ready` is an
announcement and also produces no reply.

## Envelope

Every message has these top-level fields:

| Field | Required | Description |
| --- | --- | --- |
| `$schema` | No | Optional URI for editor tooling. Ignored by the parser. |
| `protocol_version` | Yes | Must be `1`. |
| `type` | Yes | `worker_ready`, `backtest_job`, `backtest_result`, `shutdown`, or `worker_error` (`protocol_error` is a legacy input alias). |
| `message_id` | Ready/job/result/shutdown: yes; error: no | Correlation key. Starts with a letter or digit, then letters, digits, `.`, `_`, `-`, or `:`; at most 128 characters. |
| `worker_id` | Ready: yes; result/error: no | Worker identity. Same character rules as `message_id`. |

`message_id` is chosen by the sender and echoed by the receiver. It is the
correlation key, not the job identity: the coordinator may retry the same job
with a new `message_id`, or reuse it so a worker can cache and deduplicate the
reply. `job_id` is deterministic within an experiment (`ma-1`, `ma-2`, ...) but
is not globally unique, so use `(experiment_id, job_id)` for reporting and
`message_id` for wire correlation.

## `worker_ready`

Worker-to-coordinator availability announcement. The worker sends it once,
immediately after the transport is connected and before it waits for work. It
tells the coordinator which identity owns the connection. No reply is defined.

```json
{
  "$schema": "../schema/worker_ready.schema.json",
  "protocol_version": 1,
  "type": "worker_ready",
  "message_id": "worker-a:ready:0001",
  "worker_id": "worker-a"
}
```

| Field | Required | Description |
| --- | --- | --- |
| `message_id` | Yes | Correlation key for logs and diagnostics. |
| `worker_id` | Yes | Identity of the worker that owns this connection. |

A worker sends this as the application-level handshake immediately after the
TCP connection is established. It carries no capacity or strategy inventory in
version 1; those can be added as optional fields without changing the message
flow.

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

## `shutdown`

Coordinator-to-worker control message. It asks the worker to finish the job it
is already running, if any, stop accepting new messages, and close its side of
the connection. No reply is defined; the coordinator observes the close.

```json
{
  "$schema": "../schema/shutdown.schema.json",
  "protocol_version": 1,
  "type": "shutdown",
  "message_id": "crossover-sweep:shutdown:0001",
  "reason": "experiment run complete"
}
```

| Field | Required | Description |
| --- | --- | --- |
| `message_id` | Yes | Correlation key, recorded in worker logs. |
| `reason` | No | Human-readable explanation for logs and diagnostics. |

A worker also stops when the connection closes, whether the coordinator closes
it cleanly or the transport reports an error. Jobs queued after a valid
`shutdown` are not processed. An invalid `shutdown` payload produces a
`worker_error` and does not stop the worker.

## `worker_error`

Worker-to-coordinator failure report. It covers protocol failures, where the
envelope or payload cannot be processed, and worker-level execution failures
that prevent a `backtest_result` from being produced.

```json
{
  "$schema": "../schema/worker_error.schema.json",
  "protocol_version": 1,
  "type": "worker_error",
  "message_id": "crossover-sweep:crossover-1:0001",
  "worker_id": "worker-a",
  "error": {
    "type": "execution_error",
    "path": "$.job.data.path",
    "message": "Could not read market data file"
  }
}
```

| Field | Required | Description |
| --- | --- | --- |
| `message_id` | No | Echoed only when the envelope was readable. |
| `worker_id` | No | Worker identity, for logs and diagnostics. |
| `error.type` | Yes | One of the categories below. |
| `error.path` | No | JSONPath-like location, when known. |
| `error.message` | Yes | Human-readable explanation. |

Error types:

| Type | Meaning |
| --- | --- |
| `json_parse_error` | The bytes were not a single valid JSON document. |
| `unsupported_version` | `protocol_version` was missing or not `1`. |
| `unknown_message_type` | `type` was not one of the five known values. |
| `validation_error` | The envelope or payload was readable but malformed. |
| `execution_error` | The worker could not produce a result for a readable job. |

Per-job strategy and data failures still use `backtest_result` with status
`invalid` or `failed`; that preserves the one-result-per-job guarantee. A
`worker_error` means no result is available for the message.

The coordinator treats a `worker_error` as a failed attempt for the current job.
It keeps the connection open because a protocol or execution error may be
specific to one job, but it records the failure in the aggregate result.

`protocol_error` is the previous name of this message. It remains valid input,
and an error parsed under that name is serialized as `worker_error`.

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
`error.type: "validation_error"`, not a `worker_error`.

## Versioning

Every message declares `"protocol_version": 1`. Additive, backward-compatible
fields may be added within version 1; breaking changes bump the version.
Receivers must reject unsupported versions with a `worker_error` of type
`unsupported_version` when they can still read the envelope. `parse_protocol_envelope()`
does not enforce the supported version; it reports the version it saw so the
server can echo `message_id` in that reply. The type-specific parsers and
`parse_protocol_message_json()` do enforce version 1.

## Transport notes

The worker loop consumes a `comm::MessageChannel` implementation that yields
one JSON document per `receive()` and accepts one per `send()` (see
[`communication.md`](communication.md) and [`worker.md`](worker.md)). After
`connect()` the worker sends one `worker_ready` before it waits for work.

The TCP adapter uses **length-prefixed frames**: a 4-byte big-endian unsigned
payload length followed by exactly that many bytes of UTF-8 JSON. Frames must be
non-empty and at most 8 MiB. Partial reads and writes are retried by the
socket helpers, so a message may arrive in any number of TCP segments. The
coordinator is the server; each worker is a client and sends `worker_ready`
immediately after connecting. The worker executable selects TCP with
`--connect HOST:PORT`; otherwise it keeps using the line-oriented
`StdioTransport` for local development.

The coordinator accepts the configured number of workers before dispatching.
It then sends at most one job at a time to each worker. A worker's thread
blocks on that job's result, so a worker is available for a new job exactly
when its previous result has arrived. When the job queue is empty the
coordinator sends one `shutdown` to every worker and aggregates the results.

The transport owns concerns that are deliberately not in the message:

- connection lifecycle and coordinator-side registration bookkeeping;
- bounded in-flight jobs and dynamic assignment to available workers;
- logging and metrics around network and execution time.

Not yet implemented in version 1:

- timeouts, automatic retry, and duplicate suppression by `message_id`;
- reconnection after a worker drops;
- authentication and encryption.

## C++ usage

```cpp
#include "protocol/protocol.h"

// Coordinator side.
BacktestJobMessage job;
job.message_id = "crossover-sweep:crossover-1:0001";
job.job = { /* ... */ };
const std::string request = backtest_job_to_json(job, /*pretty=*/false);

// Worker side: announce readiness once, then serve.
WorkerReadyMessage ready;
ready.message_id = "worker-a:ready:0001";
ready.worker_id = "worker-a";
const std::string announcement =
    worker_ready_to_json(ready, /*pretty=*/false);

try {
    const ProtocolEnvelope envelope = parse_protocol_envelope(request);

    if (envelope.protocol_version != kBacktestProtocolVersion) {
        // Reply with make_worker_error("unsupported_version", ...,
        //                               "$.protocol_version",
        //                               envelope.message_id, "worker-a").
    } else if (envelope.type == "backtest_job") {
        const BacktestJobMessage parsed =
            parse_backtest_job_json(request);
        // Run parsed.job, then build a BacktestResultMessage.
    } else if (envelope.type == "shutdown") {
        const ShutdownMessage parsed = parse_shutdown_json(request);
        // Finish the current job, then stop the loop and close.
    }
} catch (const JsonParseError& error) {
    // Reply with make_worker_error("json_parse_error", error.what(), ...).
} catch (const ProtocolValidationError& error) {
    // Reply with make_worker_error("validation_error", error.what(),
    //                              error.path(), ...).
}
```

`parse_protocol_message_json()` parses and dispatches all five message types in
one call. The type-specific parsers are useful when the transport already knows
what to expect, or when it wants a precise error for a mismatched `type`.
`parse_protocol_error_json()` and `make_protocol_error()` remain as deprecated
aliases for the worker-error API.
