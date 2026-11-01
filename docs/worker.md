# Worker

`backtester_worker` is a standalone process that turns one `backtest_job`
message into one `backtest_result`. It is a thin wrapper around the existing
backtesting engine: it contains no strategy logic and no new execution model.

The worker loop is transport-neutral. The coordinator/worker wire contract is
defined in [`protocol.md`](protocol.md); this document covers the process that
consumes it. A TCP adapter is **not** implemented yet. Today the executable uses
a line-oriented stdio transport so the full path can be exercised locally.

## Lifecycle

Each iteration of the loop performs the steps below:

1. **Connect.** `WorkerTransport::connect()` establishes the connection. For
   stdio this only checks that both streams are usable.
2. **Wait for a job.** `receive()` blocks until one complete JSON message
   arrives, or returns `std::nullopt` when the coordinator closes the stream.
3. **Deserialize.** `parse_protocol_envelope()` reads the envelope;
   `parse_backtest_job_json()` validates and parses the payload.
4. **Validate.** The worker checks that `strategy_type` is registered and that
   the parameter set is complete and in range. This is worker-side validation:
   the protocol layer accepts any finite parameter map.
5. **Run.** `load_market_data()` reads the dataset, `create_strategy()` builds
   the strategy, and `run_backtest()` executes the existing engine.
6. **Serialize.** `make_result_message()` echoes the job identity and
   `backtest_result_to_json()` validates and writes the reply.
7. **Send.** `send()` hands exactly one message back to the coordinator.
8. **Repeat** until the transport closes.

## Error mapping

Every received message produces exactly one reply.

| Input | Reply |
| --- | --- |
| Valid job that runs | `backtest_result`, status `completed` |
| Unknown strategy type, missing or out-of-range parameter | `backtest_result`, status `invalid`, error type `validation_error` |
| Missing/unreadable data, no bars in range, engine failure | `backtest_result`, status `failed`, error type `runtime_error` |
| Not a single valid JSON document | `protocol_error`, type `json_parse_error` |
| Readable envelope but invalid/missing envelope fields | `protocol_error`, type `validation_error` |
| `protocol_version` not supported | `protocol_error`, type `unsupported_version`, `message_id` echoed when readable |
| Envelope type other than `backtest_job` | `protocol_error`, type `unknown_message_type` |
| Envelope valid, job payload malformed | `protocol_error`, type `validation_error`, `message_id` echoed |

The distinction matters to the coordinator: an `invalid` result means the job
itself was bad, so retrying it elsewhere will fail the same way. A `failed`
result may be retryable on another worker. A `protocol_error` means the envelope
could not be routed at all.

`duration_seconds` measures worker-side validation, data loading, and
execution. It excludes queueing and transport time.

## Transport

```cpp
class WorkerTransport {
public:
    virtual ~WorkerTransport() = default;
    virtual void connect() {}
    virtual std::optional<std::string> receive() = 0;
    virtual void send(const std::string& message) = 0;
};
```

`receive()` returns exactly one complete JSON document and `send()` accepts
exactly one. Framing, connection lifecycle, retries, and duplicate suppression
belong to the implementation, not the loop. `run_worker()` lets transport
exceptions propagate, so a broken connection terminates the process with a
non-zero exit code instead of silently dropping work.

### Stdio development transport

`StdioTransport` frames messages as **one compact JSON document per line**.
Blank lines are skipped and a message larger than 8 MiB is rejected. Replies are
written compact and flushed, because pretty-printed JSON contains newlines and
cannot be line framed. Logs go to stderr so stdout carries only protocol
messages.

This transport exists to make the worker runnable and testable before TCP is
written. It is not a production transport and should not be used across
untrusted boundaries.

## Usage

```bash
# One compact job per line, one compact reply per line.
./build/backtester_worker --worker-id worker-a < jobs.jsonl > results.jsonl

# Diagnostics are separate from the protocol stream.
./build/backtester_worker --worker-id worker-a < jobs.jsonl 2> worker.log

# Help.
./build/backtester_worker --help
```

The process exits `0` at end of input and `1` on a configuration or transport
error. A malformed message is not fatal: it produces a `protocol_error` and the
loop continues.

Job paths are resolved in the worker's environment. Version 1 assumes shared
storage, such as a mounted volume, so the coordinator and worker see the same
`data.path`.

## C++ API

```cpp
#include "worker/worker.h"

// The transport is injected, so tests can use an in-memory queue.
MyTransport transport;
WorkerConfig config;
config.worker_id = "worker-a";
config.log = [](const std::string& line) { /* stderr */ };

const WorkerStats stats = run_worker(transport, config);
```

`execute_backtest_job(const BacktestJob&)` is also public. It never throws for a
bad job; the returned `WorkerJobOutcome` carries `completed`, `failed`, or
`invalid` plus the metrics, trades, or structured error. This is useful for
testing or for embedding the worker loop in another process.

`make_result_message()` builds the reply for a job, attaching metrics and trades
only for a completed run and an error otherwise. Both directions then go through
the protocol validators, so an inconsistent message is rejected before it
touches the wire.

## What is not here yet

- TCP listener/client, framing, reconnection, and timeouts.
- Worker registration and heartbeats.
- Duplicate suppression by `message_id`.
- Multiple jobs in flight per worker or a bounded work queue.
- Coordinator-side aggregation of results back into an experiment response.

Those are transport and coordination concerns. The worker loop is already shaped
to accept them: replace `StdioTransport` with a TCP implementation of
`WorkerTransport` and the job pipeline is unchanged.
