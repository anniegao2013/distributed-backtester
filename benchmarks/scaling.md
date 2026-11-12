# Throughput, wall-clock time, and speedup

Captured 2026-11-12 08:32:22 PST on this machine:

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
| Sequential | 1 | 1.591 | 1.588-1.603 | 150.8 | 1.00x | 100% | 1.573 | 6.53 |
| Threads | 2 | 0.844 | 0.844-0.886 | 284.4 | 1.89x | 94% | 0.826 | 6.80 |
| Threads | 4 | 0.467 | 0.457-0.468 | 513.9 | 3.41x | 85% | 0.448 | 7.32 |
| Threads | 8 | 0.389 | 0.384-0.391 | 617.5 | 4.09x | 51% | 0.370 | 10.38 |
| Processes | 1 | 4.797 | 4.765-4.871 | 50.0 | 0.33x | 33% | 4.728 | 19.29 |
| Processes | 2 | 2.500 | 2.489-2.538 | 96.0 | 0.64x | 32% | 2.444 | 19.98 |
| Processes | 4 | 1.368 | 1.335-1.436 | 175.5 | 1.16x | 29% | 1.330 | 21.52 |
| Processes | 8 | 1.018 | 0.982-1.035 | 235.7 | 1.56x | 20% | 0.935 | 29.92 |

*Execution wall* is the runner/coordinator's own `summary.wall_seconds`, which starts after data loading and, for distributed runs, after workers connect. Process-mode job latency includes each worker's per-job CSV load; local modes preload the dataset once.

## Observations

- **Peak throughput:** Threads at 8 workers, 617.5 jobs/s (4.09x sequential).
- **Thread scaling:** 8 threads reach 617.5 jobs/s, 4.09x sequential (51% efficiency).
- **Process scaling:** 8 workers reach 235.7 jobs/s, 1.56x sequential (20% efficiency).
- **Distributed overhead:** one worker sustains 50.0 jobs/s, 3.01x slower than sequential. Each job reloads the market-data CSV in a worker and crosses a TCP boundary.
- **Threads vs processes:** the best thread configuration is 2.62x the throughput of the best process configuration on this workload.
- **Determinism:** all 24 measured runs and 8 warm-up runs produced identical job results: 240/240 jobs completed, best job `crossover-86` with Sharpe 0.6752.
- **Scaling caveat:** this host has 4 performance and 4 efficiency cores; uniform linear scaling should not be expected.

## Reproduction

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
python3 benchmarks/measure_scaling.py --levels 1,2,4,8 --reps 3 --warmup 1
```

The harness is `benchmarks/measure_scaling.py`; shared execution helpers are in `benchmarks/benchmark_execution.py`. Raw measurements are in `benchmarks/scaling_raw.json`.
