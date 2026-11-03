# Worker

`backtester_worker` is a standalone process that announces itself with
`worker_ready`, then turns one `backtest_job` message into one
`backtest_result`. It is a thin wrapper around the existing backtesting
engine: it contains no strategy logic and no new execution model.

The worker loop is transport-neutral. The coordinator/worker wire contract is
defined in [`protocol.md`](protocol.md); this document covers the process that
consumes it. The worker uses TCP when the executable is given
`--connect HOST:PORT`; with no coordinator address it keeps the line-oriented
stdio transport for local development.

## Lifecycle

The worker connects, announces readiness, then performs the steps below for
each message:

1. **Connect.** `WorkerTransport::connect()` establishes the connection. TCP
   dials the coordinator; stdio only checks that both streams are usable.
2. **Announce.** The worker sends one `worker_ready` message carrying its
   `worker_id`. The coordinator now knows which worker owns the connection.
3. **Wait for a message.** `receive()` blocks until one complete JSON message
   arrives, or returns `std::nullopt` when the coordinator closes the stream.
4. **Deserialize.** `parse_protocol_envelope()` reads the envelope. A
   `backtest_job` is parsed with `parse_backtest_job_json()`; a `shutdown` is
   parsed with `parse_shutdown_json()`.
5. **Validate.** For a job, the worker checks that `strategy_type` is registered
   and that the parameter set is complete and in range. This is worker-side
   validation: the protocol layer accepts any finite parameter map.
6. **Run.** `load_market_data()` reads the dataset, `create_strategy()` builds
   the strategy, and `run_backtest()` executes the existing engine.
7. **Serialize.** `make_result_message()` echoes the job identity and
   `backtest_result_to_json()` validates and writes the reply.
8. **Send.** `send()` hands exactly one message back to the coordinator.
9. **Repeat** until a valid `shutdown` arrives or the connection closes.

A `shutdown` takes effect between jobs. The worker finishes the job it is
already running, sends its result, and then exits without replying to the
shutdown. If jobs are already queued, they are not processed. An invalid
shutdown payload produces a `worker_error` and does not stop the loop.

## Error mapping

Every received job produces exactly one reply. A `shutdown` produces no reply.
`worker_ready` is sent unsolicited before the first receive.

| Input | Reply |
| --- | --- |
| Startup | `worker_ready`, once |
| Valid job that runs | `backtest_result`, status `completed` |
| Unknown strategy type, missing or out-of-range parameter | `backtest_result`, status `invalid`, error type `validation_error` |
| Missing/unreadable data, no bars in range, engine failure | `backtest_result`, status `failed`, error type `runtime_error` |
| Valid `shutdown` | No reply; the loop stops |
| Not a single valid JSON document | `worker_error`, type `json_parse_error` |
| Readable envelope but invalid/missing envelope fields | `worker_error`, type `validation_error` |
| `protocol_version` not supported | `worker_error`, type `unsupported_version`, `message_id` echoed when readable |
| Envelope type other than `backtest_job` or `shutdown` | `worker_error`, type `unknown_message_type` |
| Envelope valid, job or shutdown payload malformed | `worker_error`, type `validation_error`, `message_id` echoed |
| Worker could not produce a result for a readable job | `worker_error`, type `execution_error` |

The distinction matters to the coordinator: an `invalid` result means the job
itself was bad, so retrying it elsewhere will fail the same way. A `failed`
result may be retryable on another worker. A `worker_error` means no
`backtest_result` is available, either because the envelope could not be routed
or because the worker failed outside a job result.

`duration_seconds` measures worker-side validation, data loading, and
execution. It excludes queueing and transport time.

## Transport

The worker consumes the shared communication-layer contract; see
[`communication.md`](communication.md) for the full definition.

```cpp
namespace comm {

class MessageChannel {
public:
    virtual ~MessageChannel() = default;
    virtual void connect() {}
    virtual std::optional<std::string> receive() = 0;
    virtual void send(const std::string& message) = 0;
    virtual void close() {}
};

}  // namespace comm

using WorkerTransport = comm::MessageChannel;
```

`receive()` returns exactly one complete JSON document and `send()` accepts
exactly one. The worker calls `send()` once with `worker_ready` before the first
`receive()`. Framing, connection lifecycle, retries, and duplicate suppression
belong to the implementation, not the loop. `run_worker()` lets transport
exceptions propagate, so a broken connection terminates the process with a
non-zero exit code instead of silently dropping work.

### TCP transport

`TcpTransport` is an alias for `comm::TcpClientChannel`, the worker side of the
TCP communication layer. `connect()` resolves and dials the coordinator.
Messages are framed as a 4-byte big-endian payload length followed by one UTF-8
JSON document. Frames must be non-empty and at most 8 MiB. The socket helpers
retry partial reads and writes and report a clean close between frames as
`std::nullopt`, which ends the worker loop normally. A close in the middle of a
frame is a transport error.

### Stdio development transport

`StdioTransport` frames messages as **one compact JSON document per line**.
Blank lines are skipped and a message larger than 8 MiB is rejected. Replies are
written compact and flushed, because pretty-printed JSON contains newlines and
cannot be line framed. Logs go to stderr so stdout carries only protocol
messages.

This transport remains useful for local debugging and executable-level tests.
It is not a production transport and should not be used across untrusted
boundaries.

## Usage

```bash
# Connect to a coordinator listening on 127.0.0.1:9000.
./build/backtester_worker --worker-id worker-a --connect 127.0.0.1:9000

# One compact message per line over stdio instead. The worker runs until it
# reads a shutdown message or reaches end of input.
./build/backtester_worker --worker-id worker-a < messages.jsonl > results.jsonl

# Diagnostics are separate from the protocol stream.
./build/backtester_worker --worker-id worker-a < messages.jsonl 2> worker.log

# Help.
./build/backtester_worker --help
```

The first message the worker sends is always a ready announcement (shown as a
stdio line and, over TCP, as one frame):

```json
{
  "protocol_version": 1,
  "type": "worker_ready",
  "message_id": "worker-a:ready:0001",
  "worker_id": "worker-a"
}
```

A shutdown line looks like this (compact in the stream, pretty here):

```json
{
  "protocol_version": 1,
  "type": "shutdown",
  "message_id": "run-1:shutdown:0001",
  "reason": "run complete"
}
```

The process exits `0` at end of input or after a shutdown, and `1` on a
configuration or transport error. A malformed message is not fatal: it produces
a `worker_error` and the loop continues.

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
// stats.ready_messages_sent == 1, plus job and error counters.
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

- Connection timeouts, automatic reconnection, and retry after a worker drops.
- Heartbeats and worker capacity advertisement.
- Duplicate suppression by `message_id`.
- Multiple jobs in flight per worker.

Those are transport and coordination concerns. The worker loop is deliberately
small: a TCP client sends `worker_ready`, pulls one job at a time, and exits on
`shutdown` or a clean close. The coordinator owns registration, scheduling, and
aggregation; see [`coordinator.md`](coordinator.md).
