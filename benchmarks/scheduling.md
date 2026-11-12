# Scheduling overhead and worker utilization

Derived from `benchmarks/scaling_raw.json` at 2026-11-12 08:33:57 PST. Workload: 240 jobs on 8 logical CPUs (Apple M1).

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

Process-mode job durations include each worker's per-job CSV load; local job durations reuse a preloaded dataset. Utilization therefore measures busy-ness, not useful work.

## Results

| Mode | Workers | Execution wall (s) | Busy (s) | Idle (s) | Utilization | Idle/job (ms) | Work inflation | Baseline-work utilization | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Sequential | 1 | 1.5729 | 1.5679 | 0.0051 | 99.7% | 0.02 | 1.00x | 99.7% | 1.00x |
| Threads | 2 | 0.8257 | 1.6309 | 0.0228 | 98.6% | 0.10 | 1.04x | 94.9% | 1.91x |
| Threads | 4 | 0.4482 | 1.7566 | 0.0394 | 97.7% | 0.16 | 1.12x | 87.5% | 3.51x |
| Threads | 8 | 0.3702 | 2.4903 | 0.4711 | 84.1% | 1.96 | 1.59x | 52.9% | 4.25x |
| Processes | 1 | 4.7277 | 4.6292 | 0.0985 | 97.9% | 0.41 | 2.95x | 33.2% | 0.33x |
| Processes | 2 | 2.4440 | 4.7954 | 0.0937 | 98.1% | 0.39 | 3.06x | 32.1% | 0.64x |
| Processes | 4 | 1.3295 | 5.1652 | 0.1298 | 97.5% | 0.54 | 3.29x | 29.5% | 1.18x |
| Processes | 8 | 0.9348 | 7.1814 | 0.2973 | 96.1% | 1.24 | 4.58x | 21.0% | 1.68x |

Values are medians across repetitions. Execution wall excludes process startup and initial data loading; those appear in the harness overhead column of `scaling.md`.

## Per-worker balance

Coordinator dispatch lines are paired with worker-reported job durations to estimate per-worker busy time.

| Configuration | Workers | Jobs/worker (min / median / max) | Utilization (min / median / max) | Job imbalance |
| --- | ---: | ---: | ---: | ---: |
| processes-1 | 1 | 240 / 240.0 / 240 | 97.9% / 97.9% / 97.9% | 1.00x |
| processes-2 | 2 | 120 / 120.0 / 120 | 97.9% / 98.1% / 98.3% | 1.00x |
| processes-4 | 4 | 60 / 60.0 / 60 | 97.2% / 97.6% / 98.2% | 1.00x |
| processes-8 | 8 | 29 / 30.0 / 31 | 95.1% / 96.1% / 97.3% | 1.07x |

### Processes at 8 workers

| Worker | Jobs | Busy time (s) | Utilization |
| --- | ---: | ---: | ---: |
| `worker-1` | 29 | 0.8888 | 95.1% |
| `worker-2` | 31 | 0.9014 | 95.7% |
| `worker-3` | 30 | 0.9069 | 97.0% |
| `worker-4` | 30 | 0.9047 | 96.6% |
| `worker-5` | 30 | 0.9006 | 96.3% |
| `worker-6` | 30 | 0.9073 | 95.7% |
| `worker-7` | 30 | 0.9093 | 95.9% |
| `worker-8` | 30 | 0.9111 | 97.3% |

## Findings

- **Scheduling overhead is small at low concurrency.** Idle time is 0.02 ms sequentially, 0.10 ms at 2 threads, 0.16 ms at 4 threads, 0.41 ms at 1 process, 0.39 ms at 2 processes, 0.54 ms at 4 processes. At the maximum level it rises to 1.96 ms per job for 8 threads and 1.24 ms per job for 8 processes, so idle capacity is not the main scaling limit.
- **Occupancy stays high.** Worker utilization is 84.1% for 8 threads and 96.1% for 8 processes. The distributed dispatcher keeps every worker busy almost continuously.
- **Work inflation dominates.** Total busy work grows 1.59x for 8 threads and 4.58x for 8 processes relative to sequential local work. For processes, 2.95x is already present with one worker (per-job CSV loading), and concurrency adds 1.55x more through contention and CPU oversubscription.
- **Useful-work utilization is lower than occupancy.** Baseline-work utilization falls from 99.7% sequentially to 52.9% for 8 threads and 21.0% for 8 processes. High process occupancy mostly reflects redundant data loading and contention, not more backtest throughput.
- **Dispatch balance is good.** At 8 processes, workers receive 29-31 jobs (median 30.0) and run at 95.1-97.3% utilization. FIFO worker-driven dispatch shifts work toward faster workers, so there is no evidence of a straggler or an imbalanced queue.
- **The coordinator is not a CPU bottleneck.** Its process CPU time at 8 workers is 0.094 s versus 7.18 s of worker busy time (1.3%). The cost of distribution is paid in per-job data loading and contention inside the workers.

## Caveats

- Job durations are per-job wall-clock measurements, not CPU time. Process-mode durations include parsing the CSV for every job.
- Idle time includes dispatch gaps, queueing, and the end-of-run tail within the execution phase. Coordinator startup and the initial data load are outside that timer.
- Work inflation aggregates CPU contention, cache and memory pressure, file I/O, and CPU oversubscription; this report does not decompose those causes.
- Figures come from one host and one 240-job workload. Absolute utilization will differ with job size, data size, and core count.

## Reproduction

```bash
python3 benchmarks/measure_scaling.py
python3 benchmarks/analyze_scheduling.py
```

The analysis reads `benchmarks/scaling_raw.json` and writes `benchmarks/scheduling.md` plus `benchmarks/scheduling_raw.json`.
