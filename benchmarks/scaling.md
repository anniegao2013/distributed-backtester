# Throughput, wall-clock time, and speedup

Captured 2026-11-14 17:08:47 PST on this machine:

| Property | Value |
| --- | --- |
| CPU | Apple M1 |
| Cores | 8 physical (4 performance + 4 efficiency), 8 logical |
| OS | macOS-26.6.2-arm64-arm-64bit-Mach-O |
| Build | `build/release` (Release) |
| Python | 3.13.1 |

## Workload

240 independent backtests over 17,346 daily bars (1950-01-03 to 2018-12-07, `data/market/sp500_1950_2018.csv`).

Each concurrency level `N` runs the same experiment with `execution.max_concurrent_jobs = N`. Threads use the local runner with a fixed pool of `N` worker threads; processes use a coordinator and `N` worker processes over loopback TCP (`--max-attempts 1`) with `--dispatch-batch 1`.

## Method

- 1 warm-up run per configuration, then 3 measured repetitions; values below are medians.
- **Wall clock** is measured end to end, from process launch to runner/coordinator exit, including market-data loading and process startup.
- **Throughput** is `jobs / wall clock` in jobs per second.
- **Speedup** is `sequential wall clock / configuration wall clock`.
- **Parallel efficiency** is `speedup / workers` as a percentage of linear scaling.

## Results

| Mode | Workers | Wall clock (s) | Range (s) | Throughput (jobs/s) | Speedup | Efficiency | Execution wall (s) | Mean job latency (ms) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Sequential | 1 | 0.123 | 0.119-0.148 | 1952.0 | 1.00x | 100% | 0.100 | 0.42 |
| Threads | 2 | 0.071 | 0.070-0.072 | 3381.3 | 1.73x | 87% | 0.052 | 0.43 |
| Threads | 4 | 0.047 | 0.046-0.047 | 5142.5 | 2.63x | 66% | 0.028 | 0.46 |
| Threads | 8 | 0.042 | 0.041-0.043 | 5700.2 | 2.92x | 37% | 0.023 | 0.76 |
| Processes | 1 | 0.263 | 0.262-0.267 | 911.9 | 0.47x | 47% | 0.190 | 0.52 |
| Processes | 2 | 0.153 | 0.144-0.154 | 1571.1 | 0.80x | 40% | 0.110 | 0.62 |
| Processes | 4 | 0.155 | 0.152-0.188 | 1550.1 | 0.79x | 20% | 0.070 | 0.82 |
| Processes | 8 | 0.162 | 0.114-0.167 | 1484.5 | 0.76x | 10% | 0.069 | 1.59 |

*Execution wall* is the runner/coordinator's own `summary.wall_seconds`, which starts after data loading and, for distributed runs, after workers connect. Each process-mode worker loads the dataset once and then reuses it from a per-worker cache; local modes preload the dataset once.

## Observations

- **Peak throughput:** Threads at 8 workers, 5700.2 jobs/s (2.92x sequential).
- **Thread scaling:** 8 threads reach 5700.2 jobs/s, 2.92x sequential (37% efficiency).
- **Process scaling:** 8 workers reach 1484.5 jobs/s, 0.76x sequential (10% efficiency).
- **Distributed overhead:** one worker sustains 911.9 jobs/s, 2.14x slower than sequential. Each job crosses a TCP boundary and pays per-job worker setup, while the dataset itself is cached after the worker's first load.
- **Threads vs processes:** the best thread configuration is 3.84x the throughput of the best process configuration on this workload.
- **Determinism:** all 24 measured runs and 8 warm-up runs produced identical job results: 240/240 jobs completed, best job `crossover-86` with Sharpe 0.6752.
- **Scaling caveat:** this host has 4 performance and 4 efficiency cores; uniform linear scaling should not be expected.

## Reproduction

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
python3 benchmarks/measure_scaling.py --levels 1,2,4,8 --reps 3 --warmup 1
```

The harness is `benchmarks/measure_scaling.py`; shared execution helpers are in `benchmarks/benchmark_execution.py`. Raw measurements are in `benchmarks/scaling_raw.json`.
