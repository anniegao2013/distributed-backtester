# Throughput, wall-clock time, and speedup

Captured 2026-11-14 00:57:31 PST on this machine:

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
| Sequential | 1 | 1.623 | 1.612-1.643 | 147.9 | 1.00x | 100% | 1.603 | 6.68 |
| Threads | 2 | 0.844 | 0.836-0.885 | 284.4 | 1.92x | 96% | 0.825 | 6.83 |
| Threads | 4 | 0.466 | 0.453-0.471 | 514.9 | 3.48x | 87% | 0.447 | 7.38 |
| Threads | 8 | 0.339 | 0.336-0.387 | 708.7 | 4.79x | 60% | 0.320 | 10.42 |
| Processes | 1 | 1.774 | 1.760-1.815 | 135.3 | 0.91x | 91% | 1.702 | 6.73 |
| Processes | 2 | 0.952 | 0.944-0.957 | 252.2 | 1.71x | 85% | 0.905 | 7.11 |
| Processes | 4 | 0.529 | 0.521-0.538 | 453.5 | 3.07x | 77% | 0.471 | 7.42 |
| Processes | 8 | 0.434 | 0.430-0.447 | 553.6 | 3.74x | 47% | 0.348 | 10.62 |

*Execution wall* is the runner/coordinator's own `summary.wall_seconds`, which starts after data loading and, for distributed runs, after workers connect. Process-mode job latency includes each worker's per-job CSV load; local modes preload the dataset once.

## Observations

- **Peak throughput:** Threads at 8 workers, 708.7 jobs/s (4.79x sequential).
- **Thread scaling:** 8 threads reach 708.7 jobs/s, 4.79x sequential (60% efficiency).
- **Process scaling:** 8 workers reach 553.6 jobs/s, 3.74x sequential (47% efficiency).
- **Distributed overhead:** one worker sustains 135.3 jobs/s, 1.09x slower than sequential. Each job reloads the market-data CSV in a worker and crosses a TCP boundary.
- **Threads vs processes:** the best thread configuration is 1.28x the throughput of the best process configuration on this workload.
- **Determinism:** all 24 measured runs and 8 warm-up runs produced identical job results: 240/240 jobs completed, best job `crossover-86` with Sharpe 0.6752.
- **Scaling caveat:** this host has 4 performance and 4 efficiency cores; uniform linear scaling should not be expected.

## Reproduction

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
python3 benchmarks/measure_scaling.py --levels 1,2,4,8 --reps 3 --warmup 1
```

The harness is `benchmarks/measure_scaling.py`; shared execution helpers are in `benchmarks/benchmark_execution.py`. Raw measurements are in `benchmarks/scaling_raw.json`.
