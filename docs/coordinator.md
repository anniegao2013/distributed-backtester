# Coordinator

`backtester_coordinator` is the TCP server half of distributed execution. It
expands one experiment document into jobs, waits for workers to connect,
dispatches work to whichever worker is available, retries retryable failures on
another available worker, and aggregates the results into the same response
document that a local run produces.

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
| `--max-attempts N` | `3` | Maximum dispatch attempts per job. `1` disables retries. |
| `--no-retry-failed-results` | off | Do not retry `backtest_result` status `failed`. `worker_error` and transport failures remain retryable. |
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
permit and calls `pop_for()`, which waits until the next job is available and
always returns the oldest queued job. The timeout only lets an idle thread check
whether its worker is still connected; a job always wins the wait. Distribution
is therefore FIFO: the oldest queued job is claimed first even though several
workers pop concurrently.

A retryable failure with attempts remaining puts the job back at the **back** of
the queue, so other queued work keeps making progress and the retry can land on
a different worker. The job keeps its request-order index but gets a new
`message_id` and an incremented attempt count. The queue accepts this requeue
after it has been closed for new work, and idle workers stay available until
every job is terminal rather than exiting as soon as the queue is empty.

The number of permits is:

```text
min(execution.max_concurrent_jobs or hardware_concurrency, connected workers)
```

That means `max_concurrent_jobs` still caps in-flight work even if more workers
connect. With the default of `0`, all accepted workers can run concurrently.
Outcomes are written back into the original request-job order, so the aggregated
document is deterministic regardless of which worker ran which job.

## Job states

The coordinator is the single source of truth for job state. Workers only send
results; they never publish or persist state themselves.

| State | Meaning |
| --- | --- |
| `pending` | Waiting in the coordinator's FIFO job queue. |
| `running` | Assigned to a worker and currently executing. |
| `completed` | A successful result was received. |
| `failed` | Execution, transport, or result processing failed. |

A job moves `pending -> running` once it has been handed to a worker. If a
worker is already gone when the coordinator tries to send the job, the job stays
`pending` and returns to the queue.

A running job has three possible outcomes:

- **Success.** A `completed` result moves it to `completed`.
- **Retry.** A retryable failure with attempts remaining moves it back to
  `pending` (`running -> pending`) and returns it to the queue. This includes a
  `worker_error`, a transport/protocol failure, and, when
  `retry_failed_results` is enabled, a `backtest_result` with status `failed`.
- **Terminal failure.** A non-retryable result, or a retryable failure whose
  attempts are exhausted, moves it to `failed`. A worker-reported `invalid`
  result is always terminal because the same validation error would recur.

Jobs still queued after every worker has stopped move directly
`pending -> failed`. A job that was never dispatched is reported as `job was
not dispatched to a worker`; a job whose retry was still queued is reported as
`job was not completed after N attempt(s); no worker remained to retry it`.
Terminal states are final.

Progress observers can set `CoordinatorConfig::on_job_state_changed`. Callbacks
are serialized by the coordinator, though they may run on a worker thread.
`CoordinatorStats::job_states` contains the final state of every job in request
order after `run_distributed_experiment()` returns. `job_attempts` holds the
number of deliveries per job, and `job_retries` counts deliveries after the
first across the whole run.

## Worker liveness

The coordinator detects lost workers instead of waiting for a reply that will
never arrive. Detection has three paths:

- **Idle close.** Each worker thread checks `peer_closed()` before waiting for a
  job and every 100 ms while idle, so a crashed worker is removed even when no
  work is in flight. This also prevents a coordinator hang when every worker
  disappears with jobs still queued.
- **Pre-dispatch close.** A liveness check between claiming a job and sending it
  returns the job to the front of the FIFO queue. The job is still `pending`,
  so a healthy worker can run it.
- **Mid-job loss.** A close, reset, or malformed/partial reply while a result is
  expected removes the worker. The in-flight job returns to the queue when
  attempts remain; otherwise it is marked failed. In both cases the remaining
  queue is available to healthy workers.

A failed `send()` means the job never reached the worker, so it is requeued
without consuming an attempt. The coordinator cannot distinguish a job that ran
but whose reply was lost from one that never ran, so an in-flight loss consumes
an attempt and is retried with a new `message_id`. Backtests are read-only, so a
duplicate execution is harmless apart from wasted work.

Every loss increments `CoordinatorStats::workers_disconnected` and
`transport_failures`, is logged, and emits
`CoordinatorConfig::on_worker_disconnected`. The event carries the worker ID,
a human-readable reason, the in-flight `job_id` if there was one, whether that
job was marked failed (`job_failed`), and whether it was requeued for another
attempt (`job_retry_scheduled`). Lost workers are skipped during shutdown so
the same failure is not reported twice.

This detects process crashes and transport failures that close or reset the
socket. It is not a heartbeat: a worker process that is alive but hangs, or a
network partition that leaves the socket open, is not detected in version 1.
Job timeouts and OS keepalive tuning are separate concerns.

## Failure handling

| Situation | Result |
| --- | --- |
| Worker returns `completed` | `ExperimentJobOutcome::ok = true`, metrics and optional trades copied |
| Worker returns `invalid` | `ok = false`; terminal, with the validation message preserved; never retried |
| Worker returns `failed` | Retried while attempts remain when `retry_failed_results` is true; otherwise terminal with the runtime message |
| Worker returns `worker_error` | Retried while attempts remain; the error message is kept and `worker_errors` increments. When attempts are exhausted it becomes the terminal error |
| Worker closes while idle | Worker is removed; no job fails and queued work stays available |
| Worker closes before a job is sent | Job returns to the front of the queue without consuming an attempt; worker is removed |
| Worker closes while running a job, or a reply cannot be parsed | Job returns to the queue when attempts remain, otherwise it is marked failed; that worker stops pulling jobs and the remaining queue stays available |
| Attempt limit reached | Job is terminal as failed with the last attempt's error |
| A result or error repeats a handled `message_id` | Ignored; the first outcome, state, and attempt count are unchanged and `duplicate_results` increments |
| Every worker fails before finishing the queue | Pending jobs fail with `job was not dispatched to a worker`, or with the no-worker-remained message if they had already been attempted |

Per-job strategy and data failures do not abort the run. Startup problems, such
as being unable to bind, load the dataset, or read a valid `worker_ready`, throw
and make the executable exit non-zero.

Retries are bounded reassignments within one run. A deterministic runtime
failure can consume every allowed attempt; use `--max-attempts 1` or
`--no-retry-failed-results` to make such failures terminal immediately. Other
queued jobs continue on healthy workers while a retry waits.

## Aggregation and output

Local threads and remote workers report into the same
`ExperimentResultCollector`, keyed by the job's index in
`ExperimentRequest::jobs`. A job is terminal as soon as it has either completed
or failed; results may arrive in any order and the collector returns them in
request order. Retryable attempts are logged and requeued but are **not**
recorded, so each job still contributes exactly one terminal outcome. The run
waits until every job is terminal, so a mix of successes and failures always
produces one complete response document.

Result submission is idempotent. The coordinator records the `message_id` of
every attempt it has handled and ignores any later `backtest_result` or
`worker_error` that reuses one of those IDs. This matters because the
coordinator reads one reply per dispatch: without suppression, a duplicate left
in the socket would be consumed as the reply to the worker's next job. An
ignored duplicate is never recorded, never advances a job state, never consumes
an attempt, and does not count toward `results_received` or `worker_errors`; it
only increments `duplicate_results`. A reply with an unknown `message_id` is
still a correlation error and loses the worker. The collector also keeps the
first terminal outcome if two paths reach it for the same job.

If every worker stops while jobs are still queued, the coordinator marks those
jobs failed before completing. This is the only point where the coordinator
synthesizes outcomes; it never waits for work that no connected worker can
perform. The text summary adds a `Job retries: N` line when at least one retry
dispatch occurred and a `Duplicate results ignored: N` line when at least one
duplicate was suppressed.

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
config.max_job_attempts = 3;          // 1 disables retries
config.retry_failed_results = true;   // "invalid" is never retried
config.log = [](const std::string& line) { /* stderr */ };
config.on_job_state_changed = [](const JobStateTransition& change) {
    // change.index, change.job_id, change.from, change.to
};
config.on_worker_disconnected = [](const WorkerDisconnectEvent& event) {
    // event.worker_id, event.reason, event.job_id, event.job_failed
};

CoordinatorStats stats;
const ExperimentResult result =
    run_distributed_experiment(request, config, &stats);

// stats.job_states holds the final state per request job.
// stats.job_attempts holds the delivery count per request job.
// stats.job_retries counts deliveries after the first across the run.
// stats.duplicate_results counts ignored repeat submissions.
// stats.workers_disconnected counts workers lost during the run.
```

`run_distributed_experiment()` blocks for the whole run. `on_listening` fires
after the listening socket is bound and before the first accept, which is
useful for tests that bind port `0` and need the actual port. `CoordinatorStats`
reports registered workers, dispatched jobs, received results, worker errors,
transport failures, lost workers, retry counts, duplicate submissions, per-job
attempt counts, and the final coordinator-owned job state table.

`max_job_attempts` must be at least 1; an invalid policy is rejected with
`std::invalid_argument` before the listening socket is bound.

## Limitations

- No authentication or encryption; use a trusted network or a tunnel.
- No job timeout or worker reconnection. Retries are bounded reassignments
  within the current run, not recovery of a permanently failed worker.
- A job that is retried after a lost reply may run twice. Backtests are
  read-only, so the coordinator accepts the duplicate work. Duplicate
  *submissions* are suppressed by `message_id`, but a retry intentionally uses
  a new ID and therefore runs again.
- A worker that stays connected but hangs is not detected without a job
  timeout.
- A worker must see the dataset at the same path as the coordinator.
- One job in flight per worker; capacity is not advertised beyond availability.
