# Communication layer

The communication layer is the boundary between protocol messages and the bytes
that carry them. It defines **one transport-neutral contract** used by both the
coordinator and the worker:

> A channel carries exactly one UTF-8 JSON document per send/receive operation,
> owns framing and connection lifecycle, and reports a clean peer close as
> `std::nullopt`.

It deliberately knows nothing about message types, job IDs, scheduling, or
retries. Those remain in the protocol, coordinator, and worker layers.

## Layer stack

```text
coordinator / worker          application logic: scheduling, job execution
        |
        v
protocol                      message structs, parsing, validation, correlation
        |
        v
comm::MessageChannel          one JSON document per call; framing and lifecycle
        |
        v
net::TcpSocket / stdio        byte transport and framing primitives
```

The coordinator and worker both talk to `comm::MessageChannel`. Neither needs to
know whether the other side is TCP or stdio.

## Contract

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
```

| Operation | Contract |
| --- | --- |
| `connect()` | Establishes the connection and performs any transport handshake. No-op for an already-connected socket. |
| `receive()` | Blocks until one complete document is available. Returns `std::nullopt` on a clean close **between** messages. |
| `send()` | Sends one complete document. Returns only after the document has been handed to the transport. |
| `close()` | Releases the connection. Safe to call more than once. |

### Error semantics

- A clean EOF between messages is **not** an error: `receive()` returns
  `std::nullopt`, and the worker loop exits normally.
- A close in the middle of a frame, malformed framing, an empty frame, or an
  oversized frame is a transport error and is reported by throwing.
- `send()` throws if the connection is broken or the document cannot be
  framed.
- The layer does not retry, reconnect, deduplicate, or interpret JSON. Callers
  decide whether a transport failure is recoverable.

### Size limit

Every implementation enforces the same maximum document size: **8 MiB**. This
keeps a single malformed length prefix from forcing an unbounded allocation.

## Implementations

| Type | Role | Framing |
| --- | --- | --- |
| `comm::TcpChannel` | Established TCP connection, used by the coordinator after `accept()` and by tests | 4-byte big-endian length + JSON bytes |
| `comm::TcpClientChannel` (aliased as `TcpTransport`) | Worker-side TCP client | Same as `TcpChannel` |
| `StdioTransport` | Local development worker | One compact JSON document per line |

`net::TcpSocket` performs the socket work and exposes `send_frame()`,
`receive_frame()`, `receive_frame_for()`, and the non-blocking `peer_closed()`
liveness check. `comm::TcpChannel` adapts those to the `MessageChannel` contract
and adds `receive_for()` for callers that need a bounded wait. Timeout policy
stays with the coordinator; see [`coordinator.md`](coordinator.md).
`TcpClientChannel` owns dialing and delegates to a `TcpChannel` after
`connect()`. `peer_closed()` uses `poll()` plus a non-blocking `MSG_PEEK`, so a
closed or reset peer is visible without consuming a frame; pending data is not
mistaken for a close.

The worker's public `WorkerTransport` name is an alias of
`comm::MessageChannel`, so existing worker code and tests use the shared
contract directly:

```cpp
using WorkerTransport = comm::MessageChannel;
```

## TCP frame format

```text
+----------------+--------------------------------+
| 4-byte length  | length bytes of UTF-8 JSON     |
| (big-endian)   |                                |
+----------------+--------------------------------+
```

- Length is an unsigned 32-bit big-endian value.
- Zero-length frames are rejected.
- Frames larger than 8 MiB are rejected before allocation.
- Reads and writes retry partial transfers and `EINTR`, so a frame may span any
  number of TCP segments.

## Coordinator usage

```cpp
comm::TcpChannel channel(listener.accept());

const auto announcement = channel.receive();
if (!announcement) {
    // worker closed before worker_ready
}

const WorkerReadyMessage ready =
    parse_worker_ready_json(*announcement);

channel.send(backtest_job_to_json(job, /*pretty=*/false));

const auto reply = channel.receive();
if (!reply) {
    // worker closed while running the job
}

channel.send(shutdown_to_json(shutdown, /*pretty=*/false));
channel.close();
```

## Worker usage

```cpp
comm::TcpClientChannel transport("127.0.0.1", 9000);
run_worker(transport, config);
```

`run_worker()` calls `connect()`, sends one `worker_ready`, then loops on
`receive()` until `std::nullopt` or a shutdown message.

## What belongs here

- connection establishment and close;
- framing and unframing;
- partial read/write handling;
- document size enforcement;
- clean-EOF versus mid-frame-error distinction.

## What does not belong here

- JSON parsing and schema validation;
- `message_id` correlation and duplicate suppression;
- job assignment or worker registration;
- retry policy, reconnection, heartbeats, and timeout policy (a TCP channel
  exposes a bounded receive, but the coordinator decides what a timeout means);
- authentication and encryption.

Those concerns are implemented in `protocol/`, `coordinator/`, and `worker/`.
They can change without changing the channel contract, and a new transport only
needs to implement `comm::MessageChannel`.
