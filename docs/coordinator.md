# Coordinator

`backtester_coordinator` is the TCP server half of distributed execution. It
expands one experiment document into jobs, waits for workers to connect,
dispatches work to whichever worker is available, and aggregates the results
into the same response document that a local run produces.

The wire contract is defined in [`protocol.md`](protocol.md); the worker process
is described in [`worker.md`](worker.md).

## Sequence

```text
Coordinator                                   Worker
-----------                                   ------
bind HOST:PORT and listen
                                              start independently
accept connection  <------------------------  connect to HOST:PORT
read worker_ready  <------------------------  send worker_ready
send backtest_job  ------------------------->  receive one job
                                              validate, load data, run engine
read backtest_result  <---------------------  send one result
send next job  ----------------------------->  worker is available again
...
send shutdown  ----------------------------->  finish current job, close
aggregate results and exit
```

A worker is available exactly when it has finished its previous job. The
coordinator blocks on that worker's reply and then hands it the next queued job.
Slow jobs therefore do not hold up other workers.

Each accepted connection is a `comm::TcpChannel` implementing the shared
[`comm::MessageChannel`](communication.md) contract. The coordinator sends and
receives whole JSON documents and leaves framing to the communication layer.

## Usage

```bash
# Start a coordinator on the default local address. It accepts two workers
# before dispatching.
./build/backtester_coordinator \
  --experiment experiments/crossover_sweep.json \
  --workers 2

# In separate terminals or hosts, point each worker at it.
./build/backtester_worker --worker-id worker-a --connect 127.0.0.1:9000
./build/backtester_worker --worker-id worker-b --connect 127.0.0.1:9000

# Emit the aggregated ExperimentResult document instead of text.
./build/backtester_coordinator -e experiments/demo.json -n 2 --json
```

Options:

| Option | Default | Description |
| --- | --- | --- |
| `-e`, `--experiment PATH` | required | Experiment JSON. Use `-` to read it from stdin. |
| `-l`, `--listen HOST:PORT` | `127.0.0.1:9000` | Bind address. Use `0.0.0.0:9000` to accept remote workers. Port `0` selects an ephemeral port. |
| `-n`, `--workers N` | `1` | Number of worker connections to accept before dispatch starts. |
| `--json` | off | Write `experiment_result_to_json()` instead of the text summary. |

The coordinator does not exit until it has accepted `--workers` connections.
Start it first, then start the workers.

## Scheduling

The experiment is finalized before dispatch, so `request.jobs` is already the
fully-expanded list of concrete backtests. The coordinator builds one
`backtest_job` per entry, attaching the experiment's `data`, `execution`, and
`output` settings and choosing a unique `message_id`.

Every job is appended to a thread-safe FIFO `JobQueue` in request order before
any worker thread starts. The queue is then closed, so workers stop waiting once
the last job has been handed out. Each worker thread acquires a concurrency
permit and calls `pop()`, which blocks until the next job is available and always
returns the oldest queued job. Distribution is therefore FIFO: the oldest queued
job is claimed first even though several workers pop concurrently.

The number of permits is:

```text
min(execution.max_concurrent_jobs or hardware_concurrency, connected workers)
```

That means `max_concurrent_jobs` still caps in-flight work even if more workers
connect. With the default of `0`, all accepted workers can run concurrently.
Outcomes are written back into the original request-job order, so the aggregated
document is deterministic regardless of which worker ran which job.

## Failure handling

| Situation | Result |
| --- | --- |
| Worker returns `completed` | `ExperimentJobOutcome::ok = true`, metrics and optional trades copied |
| Worker returns `invalid` or `failed` | `ok = false`; the result error message is kept |
| Worker returns `worker_error` | `ok = false`; the error message is kept and `worker_errors` increments |
| Worker closes the connection or a reply cannot be parsed | Current job is marked failed; that worker stops pulling jobs, and the remaining queue stays available to other workers |
| Every worker fails before finishing the queue | Unassigned jobs remain failed with `job was not dispatched to a worker` |

Per-job strategy and data failures do not abort the run. Startup problems, such
as being unable to bind, load the dataset, or read a valid `worker_ready`, throw
and make the executable exit non-zero.

Because version 1 has no retry, a job that was in flight when a worker died is
reported as failed rather than reassigned. Other queued jobs continue on healthy
workers.

## Aggregation and output

Local threads and remote workers report into the same
`ExperimentResultCollector`, keyed by the job's index in
`ExperimentRequest::jobs`. A job is terminal as soon as it has either completed
or failed; results may arrive in any order and the collector returns them in
request order. The run waits until every job is terminal, so a mix of successes
and failures always produces one complete response document.

If every worker stops while jobs are still queued, the coordinator marks those
jobs failed with `job was not dispatched to a worker` before completing. This is
the only point where the coordinator synthesizes outcomes; it never waits for
work that no connected worker can perform.

Results are aggregated by the same `build_experiment_result()` helper used by
local runs. The returned `ExperimentResult` contains:

- the experiment request settings and data summary;
- per-job outcomes in request order;
- status `completed`, `partial`, or `failed`;
- best job, job counts, wall time, throughput, latency, and CPU statistics.

The coordinator loads the dataset once to report the same data summary as a
local run (`trading_days`, `first_date`, `last_date`). Workers still resolve
`data.path` in their own environment, so version 1 assumes shared storage.

`cpu_seconds`, `cpu_utilization`, and `cpu_efficiency` describe the coordinator
process, not the aggregated worker processes. `wall_seconds` and per-job
`duration_seconds` do include the distributed execution path.

Text output is a short summary. `--json` emits the full
[`schema/experiment_result.schema.json`](../schema/experiment_result.schema.json)
document, so existing consumers of `backtester --json` can read distributed
runs without changes.

## C++ API

```cpp
#include "coordinator/coordinator.h"

CoordinatorConfig config;
config.listen_host = "0.0.0.0";
config.listen_port = 9000;
config.expected_workers = 4;
config.log = [](const std::string& line) { /* stderr */ };

CoordinatorStats stats;
const ExperimentResult result =
    run_distributed_experiment(request, config, &stats);
```

`run_distributed_experiment()` blocks for the whole run. `on_listening` fires
after the listening socket is bound and before the first accept, which is
useful for tests that bind port `0` and need the actual port. `CoordinatorStats`
reports registered workers, dispatched jobs, received results, worker errors,
and transport failures.

## Limitations

- No authentication or encryption; use a trusted network or a tunnel.
- No job timeout, retry, or reconnection.
- A worker must see the dataset at the same path as the coordinator.
- One job in flight per worker; capacity is not advertised beyond availability.
