# Scheduling overhead and worker utilization

Derived from `benchmarks/scaling_raw.json` at 2026-11-14 17:09:22 PST. Workload: 240 jobs on 8 logical CPUs (Apple M1).

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
| Median / p90 job latency | Typical and tail per-job durations. Inflation ratios divide each configuration by the sequential value. |

Both local and process modes reuse parsed datasets across jobs. Process-mode durations still include each worker's first dataset load and TCP scheduling effects, but not a CSV parse per job. Utilization therefore measures busy-ness, not useful work.

## Results

| Mode | Workers | Execution wall (s) | Busy (s) | Idle (s) | Utilization | Idle/job (ms) | Work inflation | Baseline-work utilization | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Sequential | 1 | 0.1004 | 0.1003 | 0.0001 | 99.9% | 0.00 | 1.00x | 99.9% | 1.00x |
| Threads | 2 | 0.0523 | 0.1043 | 0.0003 | 99.7% | 0.00 | 1.04x | 95.9% | 1.92x |
| Threads | 4 | 0.0280 | 0.1104 | 0.0014 | 98.8% | 0.01 | 1.10x | 89.6% | 3.59x |
| Threads | 8 | 0.0234 | 0.1824 | 0.0033 | 98.2% | 0.01 | 1.82x | 53.7% | 4.30x |
| Processes | 1 | 0.1896 | 0.1254 | 0.0657 | 65.3% | 0.27 | 1.25x | 52.9% | 0.53x |
| Processes | 2 | 0.1105 | 0.1482 | 0.0699 | 68.2% | 0.29 | 1.48x | 45.4% | 0.91x |
| Processes | 4 | 0.0697 | 0.1978 | 0.0809 | 70.9% | 0.34 | 1.97x | 36.0% | 1.44x |
| Processes | 8 | 0.0689 | 0.3822 | 0.1693 | 69.1% | 0.71 | 3.81x | 18.2% | 1.46x |

Values are medians across repetitions. Execution wall excludes process startup and initial data loading; those appear in the harness overhead column of `scaling.md`.

## Per-job latency inflation

The busy-work ratio above is a sum and therefore tracks the mean. Splitting the same jobs into a median and a p90 separates a broad slowdown from a tail caused by core placement or descheduling.

| Mode | Workers | Median job (ms) | Median vs sequential | p90 job (ms) | p90 vs sequential |
| --- | ---: | ---: | ---: | ---: | ---: |
| Sequential | 1 | 0.412 | 1.00x | 0.432 | 1.00x |
| Threads | 2 | 0.428 | 1.04x | 0.444 | 1.03x |
| Threads | 4 | 0.445 | 1.08x | 0.495 | 1.15x |
| Threads | 8 | 0.452 | 1.10x | 1.453 | 3.36x |
| Processes | 1 | 0.452 | 1.10x | 0.490 | 1.13x |
| Processes | 2 | 0.482 | 1.17x | 0.548 | 1.27x |
| Processes | 4 | 0.512 | 1.24x | 0.740 | 1.71x |
| Processes | 8 | 0.640 | 1.55x | 1.471 | 3.40x |

A configuration whose p90 inflation greatly exceeds its median inflation is only slow for a subset of jobs. That pattern points to scheduling and core placement rather than uniform work inflation.

## Per-worker balance

Coordinator dispatch lines are paired with worker-reported job durations to estimate per-worker busy time.

| Configuration | Workers | Jobs/worker (min / median / max) | Utilization (min / median / max) | Job imbalance |
| --- | ---: | ---: | ---: | ---: |
| processes-1 | 1 | 240 / 240.0 / 240 | 65.3% / 65.3% / 65.3% | 1.00x |
| processes-2 | 2 | 120 / 120.0 / 120 | 68.1% / 68.2% / 68.3% | 1.00x |
| processes-4 | 4 | 59 / 59.0 / 61 | 70.6% / 70.8% / 71.9% | 1.03x |
| processes-8 | 8 | 27 / 29.5 / 34 | 59.1% / 69.7% / 74.0% | 1.26x |

### Processes at 8 workers

| Worker | Jobs | Busy time (s) | Utilization |
| --- | ---: | ---: | ---: |
| `worker-1` | 29 | 0.0484 | 71.1% |
| `worker-2` | 30 | 0.0478 | 69.3% |
| `worker-3` | 29 | 0.0491 | 70.2% |
| `worker-4` | 31 | 0.0464 | 67.3% |
| `worker-5` | 27 | 0.0510 | 74.0% |
| `worker-6` | 29 | 0.0490 | 71.1% |
| `worker-7` | 34 | 0.0451 | 59.1% |
| `worker-8` | 30 | 0.0475 | 68.9% |

## Findings

- **Scheduling overhead is small at low concurrency.** Idle time is 0.00 ms sequentially, 0.00 ms at 2 threads, 0.01 ms at 4 threads, 0.27 ms at 1 process, 0.29 ms at 2 processes, 0.34 ms at 4 processes. At the maximum level it rises to 0.01 ms per job for 8 threads and 0.71 ms per job for 8 processes. The worst case is 169% of a sequential job, so per-job dispatch and queueing overhead is now a co-equal scaling limit, not just a low-concurrency detail.
- **Threads stay saturated; process occupancy does not.** Worker utilization is 98.2% for 8 threads and 69.1% for 8 processes. With sub-millisecond jobs, worker setup and the TCP dispatch round trip are a visible share of each process-mode slot.
- **Work inflation dominates.** Total busy work grows 1.82x for 8 threads and 3.81x for 8 processes relative to sequential local work. For processes, 1.25x is already present with one worker: with jobs this small, fixed per-job worker setup is a larger fraction of the measured duration, so it is not the CSV parse. Concurrency adds 3.05x more through contention and CPU oversubscription.
- **Inflation is concentrated in the tail.** For threads at 8 workers the median job is 1.10x sequential but the p90 is 3.36x. A distribution that shifts uniformly would move both together; a widening gap means only a subset of jobs is slow, which points to scheduling and core placement rather than to uniform contention in the per-bar work.
- **Useful-work utilization is lower than occupancy.** Baseline-work utilization falls from 99.9% sequentially to 53.7% for 8 threads and 18.2% for 8 processes. The gap is inflated job cost. In process mode a growing share of that gap is fixed per-job worker setup and dispatch rather than useful backtest throughput.
- **Dispatch balance is good.** At 8 processes, workers receive 27-34 jobs (median 29.5) and run at 59.1-74.0% utilization. FIFO worker-driven dispatch shifts work toward faster workers, so there is no evidence of a straggler or an imbalanced queue.
- **The coordinator is not a CPU bottleneck.** Its process CPU time at 8 workers is 0.040 s versus 0.38 s of worker busy time (10.5%). The cost of distribution is paid in fixed per-job worker setup, dispatch, and contention inside the workers.

## Caveats

- Job durations are per-job wall-clock measurements, not CPU time. Process-mode durations include each worker's first dataset load and then reuse the parsed dataset from the worker cache.
- Idle time includes dispatch gaps, queueing, and the end-of-run tail within the execution phase. Coordinator startup and the initial data load are outside that timer.
- Work inflation aggregates CPU contention, cache and memory pressure, file I/O, and CPU oversubscription. The median/p90 split separates a broad slowdown from a tail but does not attribute the tail to a specific hardware or OS cause.
- Figures come from one host and one 240-job workload. Absolute utilization will differ with job size, data size, and core count.

## Reproduction

```bash
python3 benchmarks/measure_scaling.py
python3 benchmarks/analyze_scheduling.py
```

The analysis reads `benchmarks/scaling_raw.json` and writes `benchmarks/scheduling.md` plus `benchmarks/scheduling_raw.json`.
