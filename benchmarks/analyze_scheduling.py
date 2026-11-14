#!/usr/bin/env python3
"""Evaluate scheduling overhead and worker utilization.

Reads the raw output from benchmarks/measure_scaling.py and reports:

  - busy worker-time versus available worker-time,
  - scheduling overhead (idle worker-time) per job,
  - work inflation caused by concurrency,
  - baseline-work utilization, and
  - per-worker job balance and utilization for distributed runs.

Usage:
    python3 benchmarks/measure_scaling.py
    python3 benchmarks/analyze_scheduling.py
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from datetime import datetime
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from benchmark_execution import (  # noqa: E402
    REPO_ROOT,
    display_path,
    median,
    read_json,
    worker_sort_key,
)

DEFAULT_INPUT = REPO_ROOT / "benchmarks" / "scaling_raw.json"
DEFAULT_OUTPUT = REPO_ROOT / "benchmarks" / "scheduling.md"
DEFAULT_RAW_OUTPUT = REPO_ROOT / "benchmarks" / "scheduling_raw.json"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Evaluate scheduling overhead and worker utilization from the "
            "scaling benchmark data."
        )
    )
    parser.add_argument(
        "--input",
        default=str(DEFAULT_INPUT),
        help="Raw scaling results (default: benchmarks/scaling_raw.json).",
    )
    parser.add_argument(
        "--output",
        default=str(DEFAULT_OUTPUT),
        help="Markdown report path (default: benchmarks/scheduling.md).",
    )
    parser.add_argument(
        "--raw-output",
        default=str(DEFAULT_RAW_OUTPUT),
        help="Computed metrics JSON (default: benchmarks/scheduling_raw.json).",
    )
    return parser.parse_args()


def resolve(path: str) -> Path:
    candidate = Path(path).expanduser()
    return candidate if candidate.is_absolute() else REPO_ROOT / candidate


def run_metrics(
    record: dict[str, Any],
    baseline_wall: float,
    baseline_work: float,
) -> dict[str, float]:
    workers = float(record["max_concurrent"])
    wall = float(record["execution_seconds"])
    work = float(record["total_work_seconds"])
    jobs = float(record["jobs_total"])
    available = wall * workers
    idle = available - work

    return {
        "wall_seconds": wall,
        "work_seconds": work,
        "available_seconds": available,
        "idle_seconds": idle,
        "utilization": work / available if available > 0 else 0.0,
        "idle_per_job_seconds": idle / jobs if jobs > 0 else 0.0,
        "work_inflation": work / baseline_work if baseline_work > 0 else 0.0,
        "baseline_work_utilization": (
            baseline_work / available if available > 0 else 0.0
        ),
        "speedup": baseline_wall / wall if wall > 0 else 0.0,
        "coordinator_cpu_seconds": float(record["cpu_seconds"]),
        "fixed_overhead_seconds": float(record["overhead_seconds"]),
    }


def aggregate_config(
    config: dict[str, Any],
    records: list[dict[str, Any]],
    baseline_wall: float,
    baseline_work: float,
) -> dict[str, Any]:
    metrics = [
        run_metrics(record, baseline_wall, baseline_work)
        for record in records
    ]

    return {
        "key": config["key"],
        "label": config["label"],
        "kind": config["kind"],
        "workers": config["workers"],
        "wall_seconds": median(metrics, "wall_seconds"),
        "work_seconds": median(metrics, "work_seconds"),
        "available_seconds": median(metrics, "available_seconds"),
        "idle_seconds": median(metrics, "idle_seconds"),
        "utilization": median(metrics, "utilization"),
        "idle_per_job_seconds": median(metrics, "idle_per_job_seconds"),
        "work_inflation": median(metrics, "work_inflation"),
        "baseline_work_utilization": median(
            metrics, "baseline_work_utilization"
        ),
        "speedup": median(metrics, "speedup"),
        "coordinator_cpu_seconds": median(
            metrics, "coordinator_cpu_seconds"
        ),
        "fixed_overhead_seconds": median(
            metrics, "fixed_overhead_seconds"
        ),
        "worker_stats": aggregate_workers(records),
    }


def aggregate_workers(
    records: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    by_worker: dict[str, list[dict[str, Any]]] = {}

    for record in records:
        for stat in record.get("worker_stats", []):
            by_worker.setdefault(stat["worker_id"], []).append(stat)

    rows: list[dict[str, Any]] = []

    for worker_id in sorted(by_worker, key=worker_sort_key):
        stats = by_worker[worker_id]
        rows.append(
            {
                "worker_id": worker_id,
                "jobs": statistics.median(stat["jobs"] for stat in stats),
                "busy_seconds": statistics.median(
                    stat["busy_seconds"] for stat in stats
                ),
                "utilization": statistics.median(
                    stat["utilization"] for stat in stats
                ),
            }
        )

    return rows


def build_markdown(
    *,
    source: dict[str, Any],
    source_path: str,
    configs: list[dict[str, Any]],
    captured_at: str,
) -> str:
    by_key = {config["key"]: config for config in configs}
    sequential = by_key["sequential"]
    threads = [
        config
        for config in configs
        if config["kind"] == "local" and config["key"] != "sequential"
    ]
    processes = [
        config for config in configs if config["kind"] == "processes"
    ]

    lines: list[str] = []
    lines.append("# Scheduling overhead and worker utilization")
    lines.append("")
    lines.append(
        f"Derived from `{source_path}` at {captured_at}. "
        f"Workload: {source['workload']['jobs']} jobs on "
        f"{source['environment']['logical_cpus']} logical CPUs "
        f"({source['environment']['processor']})."
    )
    lines.append("")

    lines.append("## Definitions")
    lines.append("")
    lines.append("| Metric | Definition |")
    lines.append("| --- | --- |")
    lines.append(
        "| Busy worker-time | Sum of engine-reported job durations "
        "(worker-side in process mode). |"
    )
    lines.append(
        "| Available worker-time | Execution wall time x number of workers. |"
    )
    lines.append(
        "| Scheduling overhead | Available worker-time - busy worker-time. "
        "This is idle worker-time, including dispatch gaps, queueing, and "
        "the end-of-run tail. |"
    )
    lines.append(
        "| Worker utilization | Busy worker-time / available worker-time. |"
    )
    lines.append(
        "| Idle per job | Scheduling overhead / jobs completed. |"
    )
    lines.append(
        "| Work inflation | Busy worker-time / sequential busy worker-time. "
        "Values above 1 mean each job became more expensive under "
        "concurrency. |"
    )
    lines.append(
        "| Baseline-work utilization | Sequential busy worker-time / "
        "available worker-time. This estimates the fraction of capacity "
        "spent on the original backtest work, excluding inflated job cost. |"
    )
    lines.append("")
    lines.append(
        "Both local and process modes reuse parsed datasets across jobs. "
        "Process-mode durations still include each worker's first dataset "
        "load and TCP scheduling effects, but not a CSV parse per job. "
        "Utilization therefore measures busy-ness, not useful work."
    )
    lines.append("")

    lines.append("## Results")
    lines.append("")
    lines.append(
        "| Mode | Workers | Execution wall (s) | Busy (s) | Idle (s) "
        "| Utilization | Idle/job (ms) | Work inflation | Baseline-work "
        "utilization | Speedup |"
    )
    lines.append(
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: "
        "| ---: |"
    )

    for config in configs:
        lines.append(
            f"| {config['label']} | {config['workers']} "
            f"| {config['wall_seconds']:.4f} "
            f"| {config['work_seconds']:.4f} "
            f"| {config['idle_seconds']:.4f} "
            f"| {config['utilization'] * 100:.1f}% "
            f"| {config['idle_per_job_seconds'] * 1000:.2f} "
            f"| {config['work_inflation']:.2f}x "
            f"| {config['baseline_work_utilization'] * 100:.1f}% "
            f"| {config['speedup']:.2f}x |"
        )

    lines.append("")
    lines.append(
        "Values are medians across repetitions. Execution wall excludes "
        "process startup and initial data loading; those appear in the "
        "harness overhead column of `scaling.md`."
    )
    lines.append("")

    if processes:
        lines.append("## Per-worker balance")
        lines.append("")
        lines.append(
            "Coordinator dispatch lines are paired with worker-reported job "
            "durations to estimate per-worker busy time."
        )
        lines.append("")
        lines.append(
            "| Configuration | Workers | Jobs/worker (min / median / max) "
            "| Utilization (min / median / max) | Job imbalance |"
        )
        lines.append("| --- | ---: | ---: | ---: | ---: |")

        for config in processes:
            workers = config["worker_stats"]
            if not workers:
                lines.append(
                    f"| {config['key']} | {config['workers']} "
                    "| n/a | n/a | n/a |"
                )
                continue

            jobs = [worker["jobs"] for worker in workers]
            utilization = [worker["utilization"] for worker in workers]
            imbalance = max(jobs) / min(jobs) if min(jobs) > 0 else 0.0

            lines.append(
                f"| {config['key']} | {len(workers)} "
                f"| {min(jobs):.0f} / {statistics.median(jobs):.1f} / "
                f"{max(jobs):.0f} "
                f"| {min(utilization) * 100:.1f}% / "
                f"{statistics.median(utilization) * 100:.1f}% / "
                f"{max(utilization) * 100:.1f}% "
                f"| {imbalance:.2f}x |"
            )

        lines.append("")

        busiest = max(processes, key=lambda config: config["workers"])
        if busiest["worker_stats"]:
            lines.append(
                f"### {busiest['label']} at {busiest['workers']} workers"
            )
            lines.append("")
            lines.append(
                "| Worker | Jobs | Busy time (s) | Utilization |"
            )
            lines.append("| --- | ---: | ---: | ---: |")
            for worker in busiest["worker_stats"]:
                lines.append(
                    f"| `{worker['worker_id']}` | {worker['jobs']:.0f} "
                    f"| {worker['busy_seconds']:.4f} "
                    f"| {worker['utilization'] * 100:.1f}% |"
                )
            lines.append("")

    lines.append("## Findings")
    lines.append("")

    max_thread = threads[-1] if threads else None
    max_process = processes[-1] if processes else None
    busiest = max_process

    low_idle = [
        f"{sequential['idle_per_job_seconds'] * 1000:.2f} ms sequentially"
    ]
    for config in threads + processes:
        if config["workers"] <= 4:
            if config["kind"] == "local":
                unit = (
                    "thread"
                    if config["workers"] == 1
                    else "threads"
                )
            else:
                unit = (
                    "process"
                    if config["workers"] == 1
                    else "processes"
                )
            low_idle.append(
                f"{config['idle_per_job_seconds'] * 1000:.2f} ms at "
                f"{config['workers']} {unit}"
            )

    max_idle = []
    if max_thread:
        max_idle.append(
            f"{max_thread['idle_per_job_seconds'] * 1000:.2f} ms per job "
            f"for {max_thread['workers']} threads"
        )
    if max_process:
        max_idle.append(
            f"{max_process['idle_per_job_seconds'] * 1000:.2f} ms per job "
            f"for {max_process['workers']} processes"
        )

    idle_text = (
        "- **Scheduling overhead is small at low concurrency.** Idle time is "
        + ", ".join(low_idle)
    )
    if max_idle:
        idle_text += ". At the maximum level it rises to " + " and ".join(
            max_idle
        )
    idle_text += ", so idle capacity is not the main scaling limit."
    lines.append(idle_text)

    occupancy = []
    if max_thread:
        occupancy.append(
            f"{max_thread['utilization'] * 100:.1f}% for "
            f"{max_thread['workers']} threads"
        )
    if max_process:
        occupancy.append(
            f"{max_process['utilization'] * 100:.1f}% for "
            f"{max_process['workers']} processes"
        )
    lines.append(
        "- **Occupancy stays high.** Worker utilization is "
        + " and ".join(occupancy)
        + ". The distributed dispatcher keeps every worker busy almost "
        "continuously."
    )

    inflation = []
    if max_thread:
        inflation.append(
            f"{max_thread['work_inflation']:.2f}x for "
            f"{max_thread['workers']} threads"
        )
    if max_process:
        inflation.append(
            f"{max_process['work_inflation']:.2f}x for "
            f"{max_process['workers']} processes"
        )
    inflation_text = (
        "- **Work inflation dominates.** Total busy work grows "
        + " and ".join(inflation)
        + " relative to sequential local work."
    )
    if max_process and processes:
        inflation_text += (
            f" For processes, {processes[0]['work_inflation']:.2f}x is "
            "already present with one worker, which is close to noise after "
            "the worker dataset cache removes per-job CSV parsing. "
            "Concurrency adds "
            f"{max_process['work_seconds'] / processes[0]['work_seconds']:.2f}x "
            "more through contention and CPU oversubscription."
        )
    lines.append(inflation_text)

    useful = []
    if max_thread:
        useful.append(
            f"{max_thread['baseline_work_utilization'] * 100:.1f}% for "
            f"{max_thread['workers']} threads"
        )
    if max_process:
        useful.append(
            f"{max_process['baseline_work_utilization'] * 100:.1f}% for "
            f"{max_process['workers']} processes"
        )
    lines.append(
        "- **Useful-work utilization is lower than occupancy.** Baseline-work "
        f"utilization falls from {sequential['baseline_work_utilization'] * 100:.1f}% "
        "sequentially to "
        + " and ".join(useful)
        + ". High process occupancy mostly reflects work inflation under "
        "contention, not additional useful backtest throughput."
    )

    if busiest and busiest["worker_stats"]:
        jobs = [worker["jobs"] for worker in busiest["worker_stats"]]
        utilization = [
            worker["utilization"] for worker in busiest["worker_stats"]
        ]
        lines.append(
            f"- **Dispatch balance is good.** At {busiest['workers']} "
            f"processes, workers receive {min(jobs):.0f}-{max(jobs):.0f} jobs "
            f"(median {statistics.median(jobs):.1f}) and run at "
            f"{min(utilization) * 100:.1f}-{max(utilization) * 100:.1f}% "
            "utilization. FIFO worker-driven dispatch shifts work toward "
            "faster workers, so there is no evidence of a straggler or an "
            "imbalanced queue."
        )

    if max_process:
        lines.append(
            "- **The coordinator is not a CPU bottleneck.** Its process CPU "
            f"time at {max_process['workers']} workers is "
            f"{max_process['coordinator_cpu_seconds']:.3f} s versus "
            f"{max_process['work_seconds']:.2f} s of worker busy time "
            f"({max_process['coordinator_cpu_seconds'] / max_process['work_seconds'] * 100:.1f}%). "
            "The cost of distribution is paid in first-load overhead and "
            "contention inside the workers."
        )
    lines.append("")

    lines.append("## Caveats")
    lines.append("")
    lines.append(
        "- Job durations are per-job wall-clock measurements, not CPU time. "
        "Process-mode durations include each worker's first dataset load and "
        "then reuse the parsed dataset from the worker cache."
    )
    lines.append(
        "- Idle time includes dispatch gaps, queueing, and the end-of-run "
        "tail within the execution phase. Coordinator startup and the "
        "initial data load are outside that timer."
    )
    lines.append(
        "- Work inflation aggregates CPU contention, cache and memory "
        "pressure, file I/O, and CPU oversubscription; this report does not "
        "decompose those causes."
    )
    lines.append(
        "- Figures come from one host and one 240-job workload. Absolute "
        "utilization will differ with job size, data size, and core count."
    )
    lines.append("")
    lines.append("## Reproduction")
    lines.append("")
    lines.append("```bash")
    lines.append("python3 benchmarks/measure_scaling.py")
    lines.append("python3 benchmarks/analyze_scheduling.py")
    lines.append("```")
    lines.append("")
    lines.append(
        "The analysis reads `benchmarks/scaling_raw.json` and writes "
        "`benchmarks/scheduling.md` plus "
        "`benchmarks/scheduling_raw.json`."
    )
    lines.append("")

    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    input_path = resolve(args.input)
    output_path = resolve(args.output)
    raw_output_path = resolve(args.raw_output)

    source = read_json(input_path, "raw scaling results")
    runs = source.get("runs", [])
    summary = source.get("summary", [])

    if not runs or not summary:
        raise SystemExit(
            f"{input_path} does not contain runs; run measure_scaling.py first"
        )

    sequential_runs = [
        run for run in runs if run["config"] == "sequential"
    ]
    if not sequential_runs:
        raise SystemExit("Scaling data has no sequential baseline")

    baseline_wall = statistics.median(
        run["execution_seconds"] for run in sequential_runs
    )
    baseline_work = statistics.median(
        run["total_work_seconds"] for run in sequential_runs
    )

    configs = [
        aggregate_config(
            config,
            [run for run in runs if run["config"] == config["key"]],
            baseline_wall,
            baseline_work,
        )
        for config in summary
    ]

    captured_at = datetime.now().astimezone().strftime(
        "%Y-%m-%d %H:%M:%S %Z"
    )

    markdown = build_markdown(
        source=source,
        source_path=display_path(input_path),
        configs=configs,
        captured_at=captured_at,
    )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(markdown, encoding="utf-8")

    raw_output_path.parent.mkdir(parents=True, exist_ok=True)
    raw_output_path.write_text(
        json.dumps(
            {
                "captured_at": captured_at,
                "source": display_path(input_path),
                "baseline": {
                    "wall_seconds": baseline_wall,
                    "work_seconds": baseline_work,
                },
                "configs": configs,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    print(f"Wrote {display_path(output_path)}")
    print(f"Wrote {display_path(raw_output_path)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
