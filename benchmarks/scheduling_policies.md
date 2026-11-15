# Scheduling-policy comparison

Captured 2026-11-14 17:09:14 PST on this machine:

| Property | Value |
| --- | --- |
| CPU | Apple M1 |
| Cores | 8 physical (4 performance + 4 efficiency), 8 logical |
| OS | macOS-26.6.2-arm64-arm-64bit-Mach-O |
| Build | `build/release` (Release) |
| Python | 3.13.1 |

## Workload

225 independent backtests over 17,346 daily bars (1950-01-03 to 2018-12-07, `data/market/sp500_1950_2018.csv`).

The sweep mixes moving-average crossover, RSI mean reversion, and Donchian channel breakout jobs across fast and slow parameter bands. Job durations measured at concurrency 1 range from 0.000 s to 0.001 s, with a median of 0.000 s. The summed job work is 0.144 s.

| Policy | Pending-job order |
| --- | --- |
| `fifo` | Request order (baseline) |
| `longest-first` | Longest estimated job first |
| `shortest-first` | Shortest estimated job first |
| `lifo` | Last in, first out |

`longest-first` and `shortest-first` use a duration estimate derived from strategy parameters: moving-average and Donchian estimates grow with their lookback lengths, and the RSI estimate is constant. The estimate is a coarse ordering heuristic, not a calibrated cost model. Since the indicators became incremental, lookback length no longer predicts measured job cost, so the priority policies are expected to stay close to FIFO.

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
| `fifo` | 0.130 | 0.129-0.164 | 1.00x | 1724.4 | 0.84 | 73% | 0.095 | 0.035 | 0.095 | 1.00 |
| `longest-first` | 0.129 | 0.128-0.130 | 1.01x | 1739.5 | 0.83 | 72% | 0.094 | 0.036 | 0.094 | 1.00 |
| `shortest-first` | 0.130 | 0.128-0.130 | 1.01x | 1735.4 | 0.84 | 72% | 0.094 | 0.036 | 0.094 | 1.00 |
| `lifo` | 0.129 | 0.128-0.130 | 1.01x | 1740.1 | 0.84 | 73% | 0.094 | 0.035 | 0.094 | 1.00 |

## Results: 4 workers

| Policy | Makespan (s) | Range (s) | vs FIFO | Throughput (jobs/s) | Mean latency (ms) | Efficiency | Lower bound (s) | Gap (s) | Busiest worker (s) | Imbalance |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `fifo` | 0.075 | 0.074-0.076 | 1.00x | 2985.0 | 1.02 | 76% | 0.057 | 0.018 | 0.058 | 1.01 |
| `longest-first` | 0.075 | 0.074-0.076 | 1.01x | 3006.9 | 1.02 | 76% | 0.057 | 0.018 | 0.058 | 1.01 |
| `shortest-first` | 0.076 | 0.074-0.084 | 0.99x | 2954.9 | 1.01 | 75% | 0.057 | 0.019 | 0.057 | 1.01 |
| `lifo` | 0.075 | 0.075-0.077 | 1.00x | 2991.6 | 1.00 | 75% | 0.057 | 0.019 | 0.057 | 1.01 |

## Observations

- **2 workers:** `lifo` has the shortest makespan, 0.129 s, 1.01x faster than FIFO (0.130 s).
- **Longest-first vs FIFO at 2 workers:** 0.129 s versus 0.130 s (1.01x). Its gap to the lower bound is 0.036 s.
- **Shortest-first vs FIFO at 2 workers:** 0.130 s versus 0.130 s (1.01x). Mean job latency is 0.84 ms versus 0.84 ms for FIFO.
- **4 workers:** `longest-first` has the shortest makespan, 0.075 s, 1.01x faster than FIFO (0.075 s).
- **Longest-first vs FIFO at 4 workers:** 0.075 s versus 0.075 s (1.01x). Its gap to the lower bound is 0.018 s.
- **Shortest-first vs FIFO at 4 workers:** 0.076 s versus 0.075 s (0.99x). Mean job latency is 1.01 ms versus 1.02 ms for FIFO.
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
