# Scheduling overhead and worker utilization

Derived from `benchmarks/scaling_raw.json` at 2026-11-14 13:57:11 PST. Workload: 240 jobs on 8 logical CPUs (Apple M1).

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
| Sequential | 1 | 0.1457 | 0.1456 | 0.0001 | 99.9% | 0.00 | 1.00x | 99.9% | 1.00x |
| Threads | 2 | 0.0748 | 0.1492 | 0.0004 | 99.8% | 0.00 | 1.02x | 97.3% | 1.95x |
| Threads | 4 | 0.0420 | 0.1668 | 0.0014 | 99.2% | 0.01 | 1.15x | 86.6% | 3.47x |
| Threads | 8 | 0.0378 | 0.2991 | 0.0053 | 98.3% | 0.02 | 2.06x | 48.1% | 3.85x |
| Processes | 1 | 0.2474 | 0.1717 | 0.0757 | 69.4% | 0.32 | 1.18x | 58.8% | 0.59x |
| Processes | 2 | 0.1374 | 0.1900 | 0.0826 | 70.0% | 0.34 | 1.31x | 53.0% | 1.06x |
| Processes | 4 | 0.0881 | 0.2517 | 0.1019 | 71.1% | 0.42 | 1.73x | 41.3% | 1.65x |
| Processes | 8 | 0.0850 | 0.4899 | 0.1894 | 72.2% | 0.79 | 3.37x | 21.4% | 1.71x |

Values are medians across repetitions. Execution wall excludes process startup and initial data loading; those appear in the harness overhead column of `scaling.md`.

## Per-worker balance

Coordinator dispatch lines are paired with worker-reported job durations to estimate per-worker busy time.

| Configuration | Workers | Jobs/worker (min / median / max) | Utilization (min / median / max) | Job imbalance |
| --- | ---: | ---: | ---: | ---: |
| processes-1 | 1 | 240 / 240.0 / 240 | 69.4% / 69.4% / 69.4% | 1.00x |
| processes-2 | 2 | 120 / 120.0 / 120 | 69.5% / 69.7% / 69.8% | 1.00x |
| processes-4 | 4 | 60 / 60.0 / 60 | 70.5% / 71.5% / 71.5% | 1.00x |
| processes-8 | 8 | 27 / 30.0 / 33 | 69.4% / 71.3% / 73.8% | 1.22x |

### Processes at 8 workers

| Worker | Jobs | Busy time (s) | Utilization |
| --- | ---: | ---: | ---: |
| `worker-1` | 30 | 0.0597 | 71.0% |
| `worker-2` | 28 | 0.0608 | 72.3% |
| `worker-3` | 30 | 0.0616 | 71.0% |
| `worker-4` | 31 | 0.0615 | 72.3% |
| `worker-5` | 33 | 0.0590 | 69.4% |
| `worker-6` | 33 | 0.0597 | 70.1% |
| `worker-7` | 27 | 0.0628 | 73.8% |
| `worker-8` | 30 | 0.0601 | 71.5% |

## Findings

- **Scheduling overhead is small at low concurrency.** Idle time is 0.00 ms sequentially, 0.00 ms at 2 threads, 0.01 ms at 4 threads, 0.32 ms at 1 process, 0.34 ms at 2 processes, 0.42 ms at 4 processes. At the maximum level it rises to 0.02 ms per job for 8 threads and 0.79 ms per job for 8 processes. The worst case is 130% of a sequential job, so per-job dispatch and queueing overhead is now a co-equal scaling limit, not just a low-concurrency detail.
- **Threads stay saturated; process occupancy does not.** Worker utilization is 98.3% for 8 threads and 72.2% for 8 processes. With sub-millisecond jobs, worker setup and the TCP dispatch round trip are a visible share of each process-mode slot.
- **Work inflation dominates.** Total busy work grows 2.06x for 8 threads and 3.37x for 8 processes relative to sequential local work. For processes, 1.18x is already present with one worker: with jobs this small, fixed per-job worker setup is a larger fraction of the measured duration, so it is not the CSV parse. Concurrency adds 2.85x more through contention and CPU oversubscription.
- **Useful-work utilization is lower than occupancy.** Baseline-work utilization falls from 99.9% sequentially to 48.1% for 8 threads and 21.4% for 8 processes. The gap is inflated job cost. In process mode a growing share of that gap is fixed per-job worker setup and dispatch rather than useful backtest throughput.
- **Dispatch balance is good.** At 8 processes, workers receive 27-33 jobs (median 30.0) and run at 69.4-73.8% utilization. FIFO worker-driven dispatch shifts work toward faster workers, so there is no evidence of a straggler or an imbalanced queue.
- **The coordinator is not a CPU bottleneck.** Its process CPU time at 8 workers is 0.053 s versus 0.49 s of worker busy time (10.8%). The cost of distribution is paid in fixed per-job worker setup, dispatch, and contention inside the workers.

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
