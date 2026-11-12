# Execution benchmark: sequential vs threads vs processes

Captured 2026-11-11 22:34:03 PST on this machine:

| Property | Value |
| --- | --- |
| CPU | Apple M1 |
| Cores | 8 physical (4 performance + 4 efficiency), 8 logical |
| OS | macOS-26.6.2-arm64-arm-64bit-Mach-O |
| Build | `build/release` (Release) |
| Python | 3.13.1 |

## Workload

240 independent backtests: a moving-average crossover grid of 10 short windows x 24 long windows, over 17,346 daily bars (1950-01-03 to 2018-12-07, `data/market/sp500_1950_2018.csv`).

| Mode | Concurrency | How it runs |
| --- | --- | --- |
| Sequential | 1 | Local runner with `execution.max_concurrent_jobs = 1` (one in-flight job). |
| Threads | 8 | Local runner with `max_concurrent_jobs = 8`; one `std::async` worker thread per in-flight job. |
| Processes | 8 | Coordinator plus 8 `backtester_worker` processes over loopback TCP (`--max-attempts 1`). |

## Results

Medians across 5 measured repetitions, after 1 warm-up run per mode.

| Mode | Concurrency | Harness wall (s) | Execution wall (s) | Fixed overhead (s) | Throughput (jobs/s) | Speedup vs sequential (harness) | Speedup vs sequential (execution) | Mean job latency (ms) | p95 job latency (ms) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Sequential | 1 | 1.617 | 1.596 | 0.021 | 150.4 | 1.00x | 1.00x | 6.62 | 9.91 |
| Threads | 8 | 0.363 | 0.344 | 0.019 | 697.5 | 4.46x | 4.64x | 9.71 | 16.34 |
| Processes | 8 | 1.025 | 0.969 | 0.063 | 247.7 | 1.58x | 1.65x | 30.74 | 41.26 |

*Harness wall* is measured from process launch to runner/coordinator exit. *Execution wall* is the run's own `summary.wall_seconds`, which starts after market data is loaded and, for distributed runs, after workers connect. *Fixed overhead* is their difference. Throughput and latency come from each run's result document. Process-mode latency includes each worker's per-job market-data load; local modes preload the dataset once.

### Repetitions

Each cell is `harness wall / execution wall` in seconds.

| Rep | Sequential | Threads | Processes |
| ---: | ---: | ---: | ---: |
| 1 | 1.612 / 1.592 | 0.365 / 0.346 | 1.035 / 0.969 |
| 2 | 1.617 / 1.596 | 0.360 / 0.342 | 1.013 / 0.957 |
| 3 | 1.614 / 1.595 | 0.363 / 0.344 | 1.067 / 0.991 |
| 4 | 1.622 / 1.598 | 0.362 / 0.343 | 1.025 / 0.978 |
| 5 | 1.638 / 1.619 | 0.369 / 0.349 | 1.025 / 0.963 |

## Observations

- **Threads:** 4.46x faster end to end (1.617 s -> 0.363 s) and 4.64x faster in the execution phase. That is 58% of linear scaling on 8 workers.
- **Processes:** 1.58x faster end to end (1.617 s -> 1.025 s) and 1.65x faster in the execution phase (21% of linear scaling).
- **Processes vs threads:** the process pipeline takes 2.82x longer in the execution phase. Each worker reloads the market-data CSV for every job and results cross a TCP boundary, so remote mean job latency is 30.7 ms versus 9.7 ms locally.
- **Fixed overhead:** sequential/threaded runs spend about 0.02 s outside the execution timer (market-data load); processes spend 0.06 s on coordinator load, worker startup, and shutdown - about 6% of distributed end-to-end time. The relative cost grows for shorter runs.
- **Determinism:** all 15 measured runs and 3 warm-up runs produced identical job results: 240/240 jobs completed, best job `crossover-86` with Sharpe 0.6752.
- **Scaling caveat:** this host has 4 performance and 4 efficiency cores, so sub-linear scaling is expected even without synchronization or I/O overhead.

## Reproduction

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
python3 benchmarks/benchmark_execution.py --build-dir build/release --workers 8 --reps 5 --warmup 1
```

The harness is `benchmarks/benchmark_execution.py`; the workload is `benchmarks/benchmark_sweep.json`. Raw measurements are in `benchmarks/raw_results.json`.
