# Coordinator

`backtester_coordinator` is the server half of distributed execution. It expands
one experiment into jobs, accepts workers, owns all job state, and aggregates the
results into the same response document a local run produces.

The wire contract is defined in [`protocol.md`](protocol.md), the worker process
in [`worker.md`](worker.md), and framing in [`communication.md`](communication.md).

## Role and boundaries

- The coordinator is the TCP server; workers are clients.
- It is the single source of truth for `pending`, `running`, `completed`, and
  `failed`. Workers never publish or persist state.
- It decides scheduling, retries, timeouts, and recovery. These policies do not
  change the protocol.
- It returns one complete `ExperimentResult`; local and distributed runs share
  the same aggregation code.

```text
Coordinator                                   Worker
-----------                                   ------
bind and listen
accept connection        <-------------------  connect
read worker_ready        <-------------------  send worker_ready
send backtest_job        ------------------->  run one job
read backtest_result     <-------------------  send one result
send next job            ------------------->  available again
...
send shutdown            ------------------->  exit after the current job
aggregate and return
```

## Execution model

1. The experiment is finalized before dispatch, so `request.jobs` is already the
   concrete list of backtests. One `backtest_job` is built per entry with the
   experiment's data, execution, and output settings.
2. After loading state and market data, the coordinator binds its listener and
   waits for the configured number of `worker_ready` announcements.
3. Every pending job enters a thread-safe pending-job queue in request order.
   The queue is closed for new work, but retries may still be requeued at the
   logical back.
4. Each worker thread takes one concurrency permit, then claims the next job
   under the configured scheduling policy. FIFO claims the oldest job; the
   priority policies may reorder pending work.
5. With dispatch prefetching enabled, the coordinator keeps more than one job
   assigned to a busy worker. It sends the next job before the current reply
   arrives, records each attempt when the message is sent, and consumes replies
   in dispatch order. The worker still executes one job at a time.
6. The prefetch target adapts to the queue depth: a full `dispatch_batch_size`
   while work is plentiful, an even per-worker share near the tail, and one job
   for each of the last few so no worker hoards them.
7. When every job is terminal, workers receive `shutdown` and the results are
   aggregated in request order.

The number of worker pipelines executing at once is:

```text
min(execution.max_concurrent_jobs or hardware_concurrency, connected workers)
```

Each active pipeline may hold up to `dispatch_batch_size` assigned jobs, but a
worker only runs one of them at a time.

### Scheduling policies

`--scheduling-policy` (or `CoordinatorConfig::scheduling_policy`) changes only
the order in which pending jobs are offered to workers:

| Policy | Order | Intended effect |
| --- | --- | --- |
| `fifo` | Request order (default) | Fair start order; no duration estimate. |
| `lifo` | Most recently queued first | Stack-discipline control baseline. |
| `shortest-first` | Smallest estimated job first | Lower mean job latency, at the risk of a long straggler. |
| `longest-first` | Largest estimated job first | LPT-style heuristic that usually shortens makespan. |

The two estimated policies use `estimate_job_cost()`, which ranks jobs from
strategy parameters: moving-average jobs grow with `short_window +
long_window`, Donchian jobs grow with `entry_period + exit_period`, and RSI jobs
share a constant estimate because their per-bar work does not depend on the
period. The estimate is relative, not a predicted clock time. Equal estimates
keep insertion order.

Retries always enter the logical back of the queue, so a retry does not jump
ahead of queued work under FIFO. Under the priority policies, a retry is still
ranked by its estimate, and under LIFO it is popped last. Changing the policy
does not change any job result: outcomes are written back by request index.

Idle workers stay available until every job is terminal, because another worker
may still requeue a retry.

## Job states

| State | Meaning |
| --- | --- |
| `pending` | Waiting in the pending-job queue. |
| `running` | Assigned to a worker and awaiting a result. |
| `completed` | A successful result was recorded. |
| `failed` | Execution, transport, timeout, or result processing failed terminally. |

A job becomes `running` only after its message is successfully sent. If sending
fails, it stays `pending` and returns to the logical front of the queue. A
retryable attempt returns it to `pending`; success and terminal failures are
final.

Observers can subscribe through `CoordinatorConfig::on_job_state_changed`.
`CoordinatorStats::job_states` and `job_attempts` expose the final state and
delivery count per request job.

## Reliability and failure handling

Retryable failures are reassigned to another available worker with a fresh
`message_id` and an incremented attempt count. `max_job_attempts` bounds the
deliveries; `retry_failed_results` controls whether a worker-reported `failed`
result is retryable.

| Situation | Attempt consumed | Outcome |
| --- | --- | --- |
| `completed` result | yes | Job becomes `completed`. |
| `invalid` result | yes | Terminal `failed`; the same validation error would recur. |
| `failed` result, retry enabled | yes | Requeued with a new `message_id`. |
| `failed` result, retry disabled | yes | Terminal `failed` with the runtime message. |
| `worker_error` | yes | Requeued while attempts remain; otherwise terminal with the worker error. |
| Send fails before delivery | no | Job returns to the front of the queue; worker is removed. |
| Reply missing, malformed, timed out, or connection lost | yes | The current job and every prefetched job in that worker's pipeline are requeued while attempts remain; otherwise terminal `failed`. |
| Attempt limit reached | yes | Terminal `failed` with the last error. |
| Repeated handled `message_id` | no | Ignored; the first outcome wins. |
| Every worker stops with jobs queued | n/a | Pending jobs fail with an undispatched or no-worker-remaining message. |

### Worker loss and timeouts

The coordinator removes a worker when:

- it closes while idle: `peer_closed()` is checked before waiting and every
  100 ms while idle;
- it closes between claiming and sending a job: the job returns to the queue;
- it closes, resets, or sends a malformed/partial reply while running a job;
- its dispatched job exceeds `job_timeout`, measured once per attempt across any
  skipped duplicate replies; this uses the same retry/failure path as a lost
  worker.

When a worker is lost, the job it was running and every job already prefetched
into its pipeline have consumed an attempt. Each is requeued while attempts
remain, and the remainder of the queue is offered to the healthy workers.

Every loss increments `workers_disconnected` and `transport_failures`; timeouts
also increment `job_timeouts`. Each loss is logged and emitted through
`CoordinatorConfig::on_worker_disconnected` with the worker ID, reason,
in-flight job, and whether the job was retried or failed.

This is not a heartbeat. A connected worker with no job in flight is only checked
for a closed socket; idle hangs are harmless because no work depends on them.

### Duplicate submissions

The coordinator records the `message_id` of every handled attempt. A later
`backtest_result` or `worker_error` that reuses one of those IDs is ignored: it
cannot overwrite an outcome, advance a state, consume an attempt, or be mistaken
for the next job's reply. `duplicate_results` counts ignored submissions. A
reply with an unknown `message_id` remains a correlation error. Prefetched jobs
still reply in dispatch order, so only the pipeline front is compared at a time.

## Persistence and recovery

Persistence is opt-in. With `--state PATH` (or
`CoordinatorConfig::state_path`), the coordinator writes one versioned JSON file
containing each job's identity, state, attempt count, and terminal outcome.
Writes go to `PATH.tmp` and are renamed over `PATH`, so a crash cannot leave a
half-written checkpoint.

| Persisted state | Action on relaunch |
| --- | --- |
| `completed` or `failed` | Reuse the outcome; do not run the job again. |
| `running` | Return to `pending`; the result is unknown and the persisted attempt count still bounds retries. |
| `pending` | Enqueue normally. |
| No outcome and `attempts >= max_job_attempts` | Fail with `job was interrupted by a coordinator restart after N attempt(s); no attempts remain`. |

Only pending jobs enter the queue. A resumed attempt uses `attempts + 1` in its
`message_id`. If every job is already terminal, the coordinator returns the
aggregated result without binding a socket or waiting for workers.

The file is validated before any socket opens. Malformed content, a different
`experiment_id`, a job identity mismatch, an unsupported `state_version`, or a
terminal state without an outcome raises `std::runtime_error`. Terminal outcomes
are authoritative over a lagging state. Persistence failures during a run are
logged and counted, not fatal, so a stale checkpoint cannot strand workers.

`max_job_attempts` and `job_timeout` are runtime policy, not persisted. A relaunch
with a lower attempt limit can immediately fail jobs whose persisted count is
already at or above it.

## Aggregation and output

Local threads and remote workers report into one `ExperimentResultCollector`
indexed by request order. Retryable attempts are not recorded, so every job
contributes exactly one terminal outcome. Results may arrive in any order; the
returned document is deterministic.

The coordinator loads the dataset once for the response summary. Workers resolve
`data.path` in their own environment, so version 1 assumes shared storage.
`wall_seconds` and per-job durations include the distributed path, while
`cpu_seconds`, `cpu_utilization`, and `cpu_efficiency` describe the coordinator
process only.

Text output is a short summary. `--json` emits the full
[`schema/experiment_result.schema.json`](../schema/experiment_result.schema.json)
document. The text form adds retry, duplicate, timeout, and recovery lines only
when they apply.

## Configuration

| Option | Default | Description |
| --- | --- | --- |
| `-e`, `--experiment PATH` | required | Experiment JSON; `-` reads stdin. |
| `-l`, `--listen HOST:PORT` | `127.0.0.1:9000` | Bind address; port `0` is ephemeral. |
| `-n`, `--workers N` | `1` | Workers to accept before dispatch. |
| `--max-attempts N` | `3` | Maximum deliveries per job; `1` disables retries. |
| `--no-retry-failed-results` | off | Retry only `worker_error` and transport/timeout failures, not `failed` results. |
| `--job-timeout SECONDS` | `0` | Reply deadline per attempt; `0` disables it. |
| `--dispatch-batch N` | `1` | Maximum jobs kept assigned to one worker before waiting for a reply; the target shrinks near the queue tail. |
| `--scheduling-policy NAME` | `fifo` | Pending-job order: `fifo`, `lifo`, `shortest-first`, or `longest-first`. |
| `--state PATH` | off | Durable coordinator state and restart recovery. |
| `--json` | off | Emit the aggregated result JSON. |

The coordinator does not exit until it has accepted `--workers` connections.
Start it before the workers.

## C++ API

```cpp
#include "coordinator/coordinator.h"

CoordinatorConfig config;
config.expected_workers = 4;
config.max_job_attempts = 3;
config.retry_failed_results = true;
config.job_timeout = std::chrono::seconds(30); // 0 disables
config.dispatch_batch_size = 4;                // 1 disables prefetching
config.scheduling_policy = SchedulingPolicy::LongestJobFirst;
config.state_path = "run-state.json";          // empty disables
config.log = [](const std::string& line) { /* stderr */ };
config.on_job_state_changed = [](const JobStateTransition& change) {
    // change.index, change.job_id, change.from, change.to
};
config.on_worker_disconnected = [](const WorkerDisconnectEvent& event) {
    // event.worker_id, event.reason, event.job_id,
    // event.job_failed, event.job_retry_scheduled
};

CoordinatorStats stats;
const ExperimentResult result =
    run_distributed_experiment(request, config, &stats);
```

`run_distributed_experiment()` blocks for the whole run. `on_listening` fires
after bind and before the first accept, which is useful with port `0`.

`CoordinatorStats` reports:

- dispatch and outcomes: `workers_registered`, `jobs_dispatched`,
  `results_received`, `worker_errors`, `transport_failures`,
  `workers_disconnected`, `job_retries`, `job_timeouts`, `duplicate_results`;
- per job: `job_attempts`, `job_states`;
- recovery: `state_recovered`, `jobs_recovered`,
  `jobs_requeued_after_restart`, `jobs_exhausted_after_restart`,
  `persistence_errors`.

`max_job_attempts` must be at least 1, `dispatch_batch_size` must be at least
1, and `job_timeout` must not be negative. Invalid policy values throw
`std::invalid_argument` before the socket is bound.

## Limitations

- No authentication or encryption; use a trusted network or a tunnel.
- Persistence is coordinator-local; two coordinators must not share one file.
- No worker reconnection or heartbeat. A timeout covers a dispatched job only.
- A timed-out worker is dropped without cancellation; a late result is ignored.
- A retry after a lost reply may run the same backtest twice. Backtests are
  read-only, so this only wastes work.
- Every worker must see the dataset at the same path as the coordinator.
- Workers still execute one job at a time and do not advertise capacity.
  Prefetching only hides the coordinator-to-worker dispatch round trip.
