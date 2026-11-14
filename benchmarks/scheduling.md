# Scheduling overhead and worker utilization

Derived from `benchmarks/scaling_raw.json` at 2026-11-14 00:58:02 PST. Workload: 240 jobs on 8 logical CPUs (Apple M1).

## Definitions

| Metric | Definition |
| --- | --- |
| Busy worker-time | Sum of engine-reported job durations (worker-side in process mode). |
| Available worker-time | Execution wall time x number of workers. |
| Scheduling overhead | Available worker-time - busy worker-time. This is idle worker-time, including dispatch gaps, queueing, and the end-of-run tail. |
| Worker utilization | Busy worker-time / available worker-time. |
| Idle per job | Scheduling overhead / jobs completed. |
| Work inflation | Busy worker-time / sequential busy worker-time. Values above 1 mean each job became more expensive under concurrency. |
| Baseline-work utilization | Sequential busy worker-time / available worker-time. This estimates the fraction of capacity spent on the original backtest work, excluding inflated job cost. |

Both local and process modes reuse parsed datasets across jobs. Process-mode durations still include each worker's first dataset load and TCP scheduling effects, but not a CSV parse per job. Utilization therefore measures busy-ness, not useful work.

## Results

| Mode | Workers | Execution wall (s) | Busy (s) | Idle (s) | Utilization | Idle/job (ms) | Work inflation | Baseline-work utilization | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Sequential | 1 | 1.6032 | 1.6027 | 0.0004 | 100.0% | 0.00 | 1.00x | 100.0% | 1.00x |
| Threads | 2 | 0.8252 | 1.6400 | 0.0105 | 99.4% | 0.04 | 1.02x | 97.1% | 1.94x |
| Threads | 4 | 0.4466 | 1.7709 | 0.0195 | 98.9% | 0.08 | 1.10x | 89.7% | 3.59x |
| Threads | 8 | 0.3197 | 2.5002 | 0.0542 | 98.2% | 0.23 | 1.56x | 62.7% | 5.01x |
| Processes | 1 | 1.7021 | 1.6149 | 0.0871 | 94.9% | 0.36 | 1.01x | 94.2% | 0.94x |
| Processes | 2 | 0.9054 | 1.7074 | 0.1027 | 94.4% | 0.43 | 1.07x | 88.5% | 1.77x |
| Processes | 4 | 0.4708 | 1.7799 | 0.1024 | 94.6% | 0.43 | 1.11x | 85.1% | 3.41x |
| Processes | 8 | 0.3479 | 2.5494 | 0.2389 | 91.4% | 1.00 | 1.59x | 57.6% | 4.61x |

Values are medians across repetitions. Execution wall excludes process startup and initial data loading; those appear in the harness overhead column of `scaling.md`.

## Per-worker balance

Coordinator dispatch lines are paired with worker-reported job durations to estimate per-worker busy time.

| Configuration | Workers | Jobs/worker (min / median / max) | Utilization (min / median / max) | Job imbalance |
| --- | ---: | ---: | ---: | ---: |
| processes-1 | 1 | 240 / 240.0 / 240 | 94.9% / 94.9% / 94.9% | 1.00x |
| processes-2 | 2 | 120 / 120.0 / 120 | 93.9% / 94.4% / 94.9% | 1.00x |
| processes-4 | 4 | 60 / 60.0 / 60 | 93.5% / 94.5% / 95.7% | 1.00x |
| processes-8 | 8 | 28 / 30.0 / 32 | 89.5% / 91.0% / 92.0% | 1.14x |

### Processes at 8 workers

| Worker | Jobs | Busy time (s) | Utilization |
| --- | ---: | ---: | ---: |
| `worker-1` | 30 | 0.3190 | 90.5% |
| `worker-2` | 32 | 0.3195 | 90.6% |
| `worker-3` | 28 | 0.3180 | 91.4% |
| `worker-4` | 30 | 0.3142 | 90.5% |
| `worker-5` | 30 | 0.3189 | 91.9% |
| `worker-6` | 30 | 0.3251 | 91.9% |
| `worker-7` | 29 | 0.3191 | 92.0% |
| `worker-8` | 30 | 0.3196 | 89.5% |

## Findings

- **Scheduling overhead is small at low concurrency.** Idle time is 0.00 ms sequentially, 0.04 ms at 2 threads, 0.08 ms at 4 threads, 0.36 ms at 1 process, 0.43 ms at 2 processes, 0.43 ms at 4 processes. At the maximum level it rises to 0.23 ms per job for 8 threads and 1.00 ms per job for 8 processes, so idle capacity is not the main scaling limit.
- **Occupancy stays high.** Worker utilization is 98.2% for 8 threads and 91.4% for 8 processes. The distributed dispatcher keeps every worker busy almost continuously.
- **Work inflation dominates.** Total busy work grows 1.56x for 8 threads and 1.59x for 8 processes relative to sequential local work. For processes, 1.01x is already present with one worker, which is close to noise after the worker dataset cache removes per-job CSV parsing. Concurrency adds 1.58x more through contention and CPU oversubscription.
- **Useful-work utilization is lower than occupancy.** Baseline-work utilization falls from 100.0% sequentially to 62.7% for 8 threads and 57.6% for 8 processes. High process occupancy mostly reflects work inflation under contention, not additional useful backtest throughput.
- **Dispatch balance is good.** At 8 processes, workers receive 28-32 jobs (median 30.0) and run at 89.5-92.0% utilization. FIFO worker-driven dispatch shifts work toward faster workers, so there is no evidence of a straggler or an imbalanced queue.
- **The coordinator is not a CPU bottleneck.** Its process CPU time at 8 workers is 0.071 s versus 2.55 s of worker busy time (2.8%). The cost of distribution is paid in first-load overhead and contention inside the workers.

## Caveats

- Job durations are per-job wall-clock measurements, not CPU time. Process-mode durations include each worker's first dataset load and then reuse the parsed dataset from the worker cache.
- Idle time includes dispatch gaps, queueing, and the end-of-run tail within the execution phase. Coordinator startup and the initial data load are outside that timer.
- Work inflation aggregates CPU contention, cache and memory pressure, file I/O, and CPU oversubscription; this report does not decompose those causes.
- Figures come from one host and one 240-job workload. Absolute utilization will differ with job size, data size, and core count.

## Reproduction

```bash
python3 benchmarks/measure_scaling.py
python3 benchmarks/analyze_scheduling.py
```

The analysis reads `benchmarks/scaling_raw.json` and writes `benchmarks/scheduling.md` plus `benchmarks/scheduling_raw.json`.
