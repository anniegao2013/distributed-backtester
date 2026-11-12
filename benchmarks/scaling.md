# Throughput, wall-clock time, and speedup

Captured 2026-11-12 08:06:36 PST on this machine:

| Property | Value |
| --- | --- |
| CPU | Apple M1 |
| Cores | 8 physical (4 performance + 4 efficiency), 8 logical |
| OS | macOS-26.6.2-arm64-arm-64bit-Mach-O |
| Build | `build/release` (Release) |
| Python | 3.13.1 |

## Workload

240 independent backtests over 17,346 daily bars (1950-01-03 to 2018-12-07, `data/market/sp500_1950_2018.csv`).

Each concurrency level `N` runs the same experiment with `execution.max_concurrent_jobs = N`. Threads use the local runner (`std::async`); processes use a coordinator and `N` worker processes over loopback TCP (`--max-attempts 1`).

## Method

- 1 warm-up run per configuration, then 3 measured repetitions; values below are medians.
- **Wall clock** is measured end to end, from process launch to runner/coordinator exit, including market-data loading and process startup.
- **Throughput** is `jobs / wall clock` in jobs per second.
- **Speedup** is `sequential wall clock / configuration wall clock`.
- **Parallel efficiency** is `speedup / workers` as a percentage of linear scaling.

## Results

| Mode | Workers | Wall clock (s) | Range (s) | Throughput (jobs/s) | Speedup | Efficiency | Execution wall (s) | Mean job latency (ms) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Sequential | 1 | 1.669 | 1.653-1.960 | 143.8 | 1.00x | 100% | 1.649 | 6.83 |
| Threads | 2 | 0.885 | 0.883-0.941 | 271.1 | 1.89x | 94% | 0.864 | 7.09 |
| Threads | 4 | 0.499 | 0.490-0.534 | 480.8 | 3.34x | 84% | 0.479 | 7.78 |
| Threads | 8 | 0.392 | 0.391-0.419 | 612.1 | 4.26x | 53% | 0.372 | 10.79 |
| Processes | 1 | 5.044 | 4.956-5.187 | 47.6 | 0.33x | 33% | 4.964 | 20.24 |
| Processes | 2 | 2.606 | 2.574-2.626 | 92.1 | 0.64x | 32% | 2.526 | 20.66 |
| Processes | 4 | 1.476 | 1.441-1.482 | 162.6 | 1.13x | 28% | 1.431 | 23.27 |
| Processes | 8 | 1.144 | 1.144-1.183 | 209.8 | 1.46x | 18% | 1.078 | 34.08 |

*Execution wall* is the runner/coordinator's own `summary.wall_seconds`, which starts after data loading and, for distributed runs, after workers connect. Process-mode job latency includes each worker's per-job CSV load; local modes preload the dataset once.

## Observations

- **Peak throughput:** Threads at 8 workers, 612.1 jobs/s (4.26x sequential).
- **Thread scaling:** 8 threads reach 612.1 jobs/s, 4.26x sequential (53% efficiency).
- **Process scaling:** 8 workers reach 209.8 jobs/s, 1.46x sequential (18% efficiency).
- **Distributed overhead:** one worker sustains 47.6 jobs/s, 3.02x slower than sequential. Each job reloads the market-data CSV in a worker and crosses a TCP boundary.
- **Threads vs processes:** the best thread configuration is 2.92x the throughput of the best process configuration on this workload.
- **Determinism:** all 24 measured runs and 8 warm-up runs produced identical job results: 240/240 jobs completed, best job `crossover-86` with Sharpe 0.6752.
- **Scaling caveat:** this host has 4 performance and 4 efficiency cores; uniform linear scaling should not be expected.

## Reproduction

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
python3 benchmarks/measure_scaling.py --levels 1,2,4,8 --reps 3 --warmup 1
```

The harness is `benchmarks/measure_scaling.py`; shared execution helpers are in `benchmarks/benchmark_execution.py`. Raw measurements are in `benchmarks/scaling_raw.json`.
