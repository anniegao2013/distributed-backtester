# Scheduling-policy comparison

Captured 2026-11-13 23:29:38 PST on this machine:

| Property | Value |
| --- | --- |
| CPU | Apple M1 |
| Cores | 8 physical (4 performance + 4 efficiency), 8 logical |
| OS | macOS-26.6.2-arm64-arm-64bit-Mach-O |
| Build | `build/release` (Release) |
| Python | 3.13.1 |

## Workload

225 independent backtests over 17,346 daily bars (1950-01-03 to 2018-12-07, `data/market/sp500_1950_2018.csv`).

The sweep mixes moving-average crossover, RSI mean reversion, and Donchian channel breakout jobs across fast and slow parameter bands. Job durations measured at concurrency 1 range from 0.001 s to 0.038 s, with a median of 0.005 s. The summed job work is 1.834 s.

| Policy | Pending-job order |
| --- | --- |
| `fifo` | Request order (baseline) |
| `longest-first` | Longest estimated job first |
| `shortest-first` | Shortest estimated job first |
| `lifo` | Last in, first out |

`longest-first` and `shortest-first` use a duration estimate derived from strategy parameters: moving-average cost grows with `short_window + long_window`, Donchian cost grows with `entry_period + exit_period`, and RSI cost is constant. The estimate is a relative heuristic, not a learned model.

## Method

- 1 warm-up run per configuration, then 5 measured repetitions; values are medians.
- Each run uses a coordinator and `N` worker processes over loopback TCP with `--max-attempts 1` and `--dispatch-batch 1`.
- **Makespan** is the coordinator's `summary.wall_seconds`, measured after every worker has connected and the dataset has loaded, through the final terminal job.
- **Lower bound** is `max(total job work / workers, longest job)`: no schedule can finish sooner than either bound. Total work comes from the concurrent run, so it includes concurrency-related work inflation.
- **Efficiency** is `total job work / (makespan * workers)`.
- **Worker imbalance** is the busiest worker's busy time divided by the mean busy time. 1.0 is perfectly balanced.

## Results: 2 workers

| Policy | Makespan (s) | Range (s) | vs FIFO | Throughput (jobs/s) | Mean latency (ms) | Efficiency | Lower bound (s) | Gap (s) | Busiest worker (s) | Imbalance |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `fifo` | 1.012 | 1.011-1.016 | 1.00x | 222.3 | 8.61 | 96% | 0.968 | 0.044 | 0.974 | 1.00 |
| `longest-first` | 1.008 | 1.007-1.033 | 1.00x | 223.3 | 8.60 | 96% | 0.967 | 0.041 | 0.968 | 1.00 |
| `shortest-first` | 1.015 | 1.014-1.028 | 1.00x | 221.7 | 8.59 | 95% | 0.967 | 0.048 | 0.976 | 1.01 |
| `lifo` | 1.016 | 1.006-1.035 | 1.00x | 221.5 | 8.67 | 96% | 0.975 | 0.041 | 0.978 | 1.00 |

## Results: 4 workers

| Policy | Makespan (s) | Range (s) | vs FIFO | Throughput (jobs/s) | Mean latency (ms) | Efficiency | Lower bound (s) | Gap (s) | Busiest worker (s) | Imbalance |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `fifo` | 0.542 | 0.539-0.561 | 1.00x | 415.3 | 9.10 | 94% | 0.512 | 0.030 | 0.524 | 1.02 |
| `longest-first` | 0.532 | 0.531-0.536 | 1.02x | 422.9 | 9.11 | 96% | 0.512 | 0.020 | 0.513 | 1.00 |
| `shortest-first` | 0.547 | 0.546-0.551 | 0.99x | 411.3 | 9.11 | 94% | 0.513 | 0.034 | 0.527 | 1.03 |
| `lifo` | 0.536 | 0.535-0.540 | 1.01x | 419.9 | 9.10 | 96% | 0.512 | 0.024 | 0.519 | 1.01 |

## Observations

- **2 workers:** `longest-first` has the shortest makespan, 1.008 s, 1.00x faster than FIFO (1.012 s).
- **Longest-first vs FIFO at 2 workers:** 1.008 s versus 1.012 s (1.00x). Its gap to the lower bound is 0.041 s.
- **Shortest-first vs FIFO at 2 workers:** 1.015 s versus 1.012 s (1.00x). Mean job latency is 8.59 ms versus 8.61 ms for FIFO.
- **4 workers:** `longest-first` has the shortest makespan, 0.532 s, 1.02x faster than FIFO (0.542 s).
- **Longest-first vs FIFO at 4 workers:** 0.532 s versus 0.542 s (1.02x). Its gap to the lower bound is 0.020 s.
- **Shortest-first vs FIFO at 4 workers:** 0.547 s versus 0.542 s (0.99x). Mean job latency is 9.11 ms versus 9.10 ms for FIFO.
- **Why the gaps are small:** experiment finalization interleaves strategy jobs round-robin, so FIFO already mixes cheap and expensive bands. Longest-first mainly removes the residual tail: it lowers worker imbalance and the gap to the lower bound rather than changing the overall work distribution.
- **Determinism:** all 41 measured runs produced identical job results, so policy changes only affect dispatch order.
- **Noise caveat:** this host has 4 performance and 4 efficiency cores; small makespan gaps may move between repetitions.

## Reproduction

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
python3 benchmarks/compare_scheduling.py --workers 2,4 --policies fifo,longest-first,shortest-first,lifo --reps 5 --warmup 1
```

The harness is `benchmarks/compare_scheduling.py`; the workload is `experiments/heterogeneous_scheduling.json`. Raw measurements are in `benchmarks/scheduling_policies_raw.json`.
