# Throughput, wall-clock time, and speedup

Captured 2026-11-14 13:55:03 PST on this machine:

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
| Sequential | 1 | 0.164 | 0.164-0.165 | 1461.2 | 1.00x | 100% | 0.146 | 0.61 |
| Threads | 2 | 0.093 | 0.093-0.094 | 2571.8 | 1.76x | 88% | 0.075 | 0.62 |
| Threads | 4 | 0.062 | 0.061-0.066 | 3875.3 | 2.65x | 66% | 0.042 | 0.69 |
| Threads | 8 | 0.057 | 0.056-0.059 | 4175.7 | 2.86x | 36% | 0.038 | 1.25 |
| Processes | 1 | 0.315 | 0.303-0.320 | 762.5 | 0.52x | 52% | 0.247 | 0.72 |
| Processes | 2 | 0.210 | 0.202-0.212 | 1141.2 | 0.78x | 39% | 0.137 | 0.79 |
| Processes | 4 | 0.154 | 0.150-0.158 | 1560.0 | 1.07x | 27% | 0.088 | 1.05 |
| Processes | 8 | 0.164 | 0.161-0.164 | 1466.6 | 1.00x | 13% | 0.085 | 2.04 |

*Execution wall* is the runner/coordinator's own `summary.wall_seconds`, which starts after data loading and, for distributed runs, after workers connect. Process-mode job latency includes each worker's per-job CSV load; local modes preload the dataset once.

## Observations

- **Peak throughput:** Threads at 8 workers, 4175.7 jobs/s (2.86x sequential).
- **Thread scaling:** 8 threads reach 4175.7 jobs/s, 2.86x sequential (36% efficiency).
- **Process scaling:** 8 workers reach 1466.6 jobs/s, 1.00x sequential (13% efficiency).
- **Distributed overhead:** one worker sustains 762.5 jobs/s, 1.92x slower than sequential. Each job reloads the market-data CSV in a worker and crosses a TCP boundary.
- **Threads vs processes:** the best thread configuration is 2.85x the throughput of the best process configuration on this workload.
- **Determinism:** all 24 measured runs and 8 warm-up runs produced identical job results: 240/240 jobs completed, best job `crossover-86` with Sharpe 0.6752.
- **Scaling caveat:** this host has 4 performance and 4 efficiency cores; uniform linear scaling should not be expected.

## Reproduction

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
python3 benchmarks/measure_scaling.py --levels 1,2,4,8 --reps 3 --warmup 1
```

The harness is `benchmarks/measure_scaling.py`; shared execution helpers are in `benchmarks/benchmark_execution.py`. Raw measurements are in `benchmarks/scaling_raw.json`.
