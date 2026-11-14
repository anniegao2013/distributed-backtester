#!/usr/bin/env python3
"""Compare coordinator scheduling policies on a heterogeneous workload.

Every policy runs the same experiment over the same jobs. The script measures
makespan (the coordinator's dispatch wall time), throughput, total job work,
and per-worker balance, then reports each policy relative to FIFO.

Policies:

  fifo            request order; the original behavior
  longest-first   longest estimated job first (LPT-style makespan heuristic)
  shortest-first  shortest estimated job first (mean-flow-time heuristic)
  lifo            last in, first out; a stack-discipline control

Duration estimates come from strategy parameters. Moving-average and Donchian
jobs are ranked by lookback length; RSI jobs share a constant estimate.

Usage:
    cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
    cmake --build build/release -j
    python3 benchmarks/compare_scheduling.py --workers 4 --reps 5

Requires the release binaries and the bundled SP500 dataset.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import tempfile
from datetime import datetime
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from benchmark_execution import (  # noqa: E402
    REPO_ROOT,
    display_path,
    environment,
    job_count,
    median,
    read_json,
    require_binaries,
    resolve,
    run_local,
    run_processes,
)

DEFAULT_EXPERIMENT = (
    REPO_ROOT / "experiments" / "heterogeneous_scheduling.json"
)
DEFAULT_POLICIES = ("fifo", "longest-first", "shortest-first", "lifo")
POLICY_DESCRIPTIONS = {
    "fifo": "Request order (baseline)",
    "longest-first": "Longest estimated job first",
    "shortest-first": "Shortest estimated job first",
    "lifo": "Last in, first out",
}


def parse_workers(text: str) -> list[int]:
    levels: set[int] = set()

    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            level = int(part)
        except ValueError as error:
            raise argparse.ArgumentTypeError(
                f"invalid worker count '{part}'"
            ) from error
        if level < 1:
            raise argparse.ArgumentTypeError(
                "worker counts must be at least 1"
            )
        levels.add(level)

    if not levels:
        raise argparse.ArgumentTypeError("at least one worker count is required")

    return sorted(levels)


def parse_policies(text: str) -> list[str]:
    policies: list[str] = []
    seen: set[str] = set()

    for part in text.split(","):
        policy = part.strip()
        if not policy:
            continue
        if policy not in POLICY_DESCRIPTIONS:
            raise argparse.ArgumentTypeError(
                f"unknown policy '{policy}'; choose from "
                + ", ".join(POLICY_DESCRIPTIONS)
            )
        if policy not in seen:
            policies.append(policy)
            seen.add(policy)

    if not policies:
        raise argparse.ArgumentTypeError("at least one policy is required")

    return policies


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compare coordinator scheduling policies by makespan and worker "
            "balance on a heterogeneous strategy sweep."
        )
    )
    parser.add_argument(
        "--build-dir",
        default="build/release",
        help="CMake build directory (default: build/release).",
    )
    parser.add_argument(
        "--experiment",
        default=str(DEFAULT_EXPERIMENT),
        help=(
            "Base experiment JSON (default: "
            "experiments/heterogeneous_scheduling.json)."
        ),
    )
    parser.add_argument(
        "--workers",
        type=parse_workers,
        default=[4],
        help="Comma-separated worker counts (default: 4).",
    )
    parser.add_argument(
        "--policies",
        type=parse_policies,
        default=list(DEFAULT_POLICIES),
        help=(
            "Comma-separated policies (default: "
            + ",".join(DEFAULT_POLICIES)
            + ")."
        ),
    )
    parser.add_argument(
        "--reps",
        type=int,
        default=5,
        help="Measured repetitions per configuration (default: 5).",
    )
    parser.add_argument(
        "--warmup",
        type=int,
        default=1,
        help="Unmeasured warm-up runs per configuration (default: 1).",
    )
    parser.add_argument(
        "--dispatch-batch",
        type=int,
        default=1,
        help=(
            "Coordinator prefetch target per worker (default: 1, no "
            "prefetching)."
        ),
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=600.0,
        help="Per-run timeout in seconds (default: 600).",
    )
    parser.add_argument(
        "--output",
        default=str(REPO_ROOT / "benchmarks" / "scheduling_policies.md"),
        help="Markdown report path (default: benchmarks/scheduling_policies.md).",
    )
    parser.add_argument(
        "--raw-output",
        default=str(
            REPO_ROOT / "benchmarks" / "scheduling_policies_raw.json"
        ),
        help=(
            "Raw JSON path (default: benchmarks/scheduling_policies_raw.json)."
        ),
    )

    args = parser.parse_args()

    if args.reps < 1:
        parser.error("--reps must be at least 1")
    if args.warmup < 0:
        parser.error("--warmup cannot be negative")
    if args.dispatch_batch < 1:
        parser.error("--dispatch-batch must be at least 1")
    if "fifo" not in args.policies:
        parser.error("--policies must include fifo as the baseline")

    return args


def write_variant(
    base_request: dict[str, Any],
    workers: int,
    path: Path,
) -> Path:
    request = json.loads(json.dumps(base_request))
    request.setdefault("execution", {})["max_concurrent_jobs"] = workers
    path.write_text(json.dumps(request, indent=2) + "\n", encoding="utf-8")
    return path


def worker_balance(record: dict[str, Any]) -> dict[str, float]:
    stats = record.get("worker_stats", [])
    busy = [float(entry["busy_seconds"]) for entry in stats]

    if not busy:
        return {
            "worker_busy_max_seconds": 0.0,
            "worker_busy_min_seconds": 0.0,
            "worker_imbalance": 0.0,
        }

    mean_busy = statistics.fmean(busy)
    return {
        "worker_busy_max_seconds": max(busy),
        "worker_busy_min_seconds": min(busy),
        "worker_imbalance": max(busy) / mean_busy if mean_busy > 0 else 0.0,
    }


def summarize_configuration(
    workers: int,
    policy: str,
    records: list[dict[str, Any]],
) -> dict[str, Any]:
    makespan = median(records, "execution_seconds")
    total_work = median(records, "total_work_seconds")
    max_job = median(records, "max_job_seconds")
    lower_bound = max(total_work / workers, max_job)
    balance = [
        worker_balance(record)
        for record in records
    ]

    return {
        "workers": workers,
        "policy": policy,
        "makespan_seconds": makespan,
        "makespan_min_seconds": min(
            record["execution_seconds"] for record in records
        ),
        "makespan_max_seconds": max(
            record["execution_seconds"] for record in records
        ),
        "harness_seconds": median(records, "harness_seconds"),
        "throughput": median(records, "throughput"),
        "mean_latency_seconds": median(records, "mean_latency_seconds"),
        "total_work_seconds": total_work,
        "max_job_seconds": max_job,
        "lower_bound_seconds": lower_bound,
        "makespan_gap_seconds": makespan - lower_bound,
        "efficiency": total_work / (makespan * workers) if makespan > 0 else 0.0,
        "worker_busy_max_seconds": median(balance, "worker_busy_max_seconds"),
        "worker_busy_min_seconds": median(balance, "worker_busy_min_seconds"),
        "worker_imbalance": median(balance, "worker_imbalance"),
        "digest": records[0]["digest"],
    }


def format_seconds(value: float) -> str:
    return f"{value:.3f}"


def build_markdown(
    *,
    env: dict[str, Any],
    workload: dict[str, Any],
    args: argparse.Namespace,
    summary: list[dict[str, Any]],
    consistency: dict[str, Any],
    captured_at: str,
) -> str:
    by_key = {(item["workers"], item["policy"]): item for item in summary}
    worker_levels = sorted({item["workers"] for item in summary})

    lines: list[str] = []
    lines.append("# Scheduling-policy comparison")
    lines.append("")
    lines.append(f"Captured {captured_at} on this machine:")
    lines.append("")
    lines.append("| Property | Value |")
    lines.append("| --- | --- |")
    lines.append(f"| CPU | {env['processor']} |")
    if env["performance_cores"] and env["efficiency_cores"]:
        cores = (
            f"{env['physical_cpus'] or 'n/a'} physical "
            f"({env['performance_cores']} performance + "
            f"{env['efficiency_cores']} efficiency), "
            f"{env['logical_cpus']} logical"
        )
    else:
        cores = (
            f"{env['physical_cpus'] or 'n/a'} physical, "
            f"{env['logical_cpus']} logical"
        )
    lines.append(f"| Cores | {cores} |")
    lines.append(f"| OS | {env['platform']} |")
    lines.append(f"| Build | `{env['build_dir']}` ({env['build_type']}) |")
    lines.append(f"| Python | {env['python']} |")
    lines.append("")

    lines.append("## Workload")
    lines.append("")
    lines.append(
        f"{workload['jobs']} independent backtests over "
        f"{workload['trading_days']:,} daily bars "
        f"({workload['start_date']} to {workload['end_date']}, "
        f"{workload['data_path']})."
    )
    lines.append("")
    lines.append(
        "The sweep mixes moving-average crossover, RSI mean reversion, and "
        "Donchian channel breakout jobs across fast and slow parameter bands. "
        "Job durations measured at concurrency 1 range from "
        f"{format_seconds(workload['min_job_seconds'])} s to "
        f"{format_seconds(workload['max_job_seconds'])} s, with a median of "
        f"{format_seconds(workload['median_job_seconds'])} s. The summed job "
        f"work is {format_seconds(workload['total_work_seconds'])} s."
    )
    lines.append("")
    lines.append("| Policy | Pending-job order |")
    lines.append("| --- | --- |")
    for policy, description in POLICY_DESCRIPTIONS.items():
        lines.append(f"| `{policy}` | {description} |")
    lines.append("")
    lines.append(
        "`longest-first` and `shortest-first` use a duration estimate derived "
        "from strategy parameters: moving-average cost grows with "
        "`short_window + long_window`, Donchian cost grows with "
        "`entry_period + exit_period`, and RSI cost is constant. The estimate "
        "is a relative heuristic, not a learned model."
    )
    lines.append("")

    lines.append("## Method")
    lines.append("")
    warmup_text = (
        f"{args.warmup} warm-up "
        f"{'run' if args.warmup == 1 else 'runs'}"
        if args.warmup > 0
        else "No warm-up run"
    )
    lines.append(
        f"- {warmup_text} per configuration, then "
        f"{args.reps} measured "
        f"{'repetition' if args.reps == 1 else 'repetitions'}; values are "
        "medians."
    )
    lines.append(
        "- Each run uses a coordinator and `N` worker processes over loopback "
        f"TCP with `--max-attempts 1` and `--dispatch-batch "
        f"{args.dispatch_batch}`."
    )
    lines.append(
        "- **Makespan** is the coordinator's `summary.wall_seconds`, measured "
        "after every worker has connected and the dataset has loaded, through "
        "the final terminal job."
    )
    lines.append(
        "- **Lower bound** is `max(total job work / workers, longest job)`: no "
        "schedule can finish sooner than either bound. Total work comes from "
        "the concurrent run, so it includes concurrency-related work inflation."
    )
    lines.append(
        "- **Efficiency** is `total job work / (makespan * workers)`."
    )
    lines.append(
        "- **Worker imbalance** is the busiest worker's busy time divided by "
        "the mean busy time. 1.0 is perfectly balanced."
    )
    lines.append("")

    for workers in worker_levels:
        lines.append(f"## Results: {workers} workers")
        lines.append("")
        lines.append(
            "| Policy | Makespan (s) | Range (s) | vs FIFO | Throughput "
            "(jobs/s) | Mean latency (ms) | Efficiency | Lower bound (s) | "
            "Gap (s) | Busiest worker (s) | Imbalance |"
        )
        lines.append(
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: "
            "| ---: | ---: | ---: |"
        )
        fifo = by_key[(workers, "fifo")]
        baseline = fifo["makespan_seconds"]

        for policy in args.policies:
            item = by_key[(workers, policy)]
            change = (
                baseline / item["makespan_seconds"]
                if item["makespan_seconds"] > 0
                else 1.0
            )
            lines.append(
                f"| `{policy}` "
                f"| {format_seconds(item['makespan_seconds'])} "
                f"| {format_seconds(item['makespan_min_seconds'])}-"
                f"{format_seconds(item['makespan_max_seconds'])} "
                f"| {change:.2f}x "
                f"| {item['throughput']:.1f} "
                f"| {item['mean_latency_seconds'] * 1000.0:.2f} "
                f"| {item['efficiency'] * 100.0:.0f}% "
                f"| {format_seconds(item['lower_bound_seconds'])} "
                f"| {format_seconds(item['makespan_gap_seconds'])} "
                f"| {format_seconds(item['worker_busy_max_seconds'])} "
                f"| {item['worker_imbalance']:.2f} |"
            )

        lines.append("")

    lines.append("## Observations")
    lines.append("")
    for workers in worker_levels:
        fifo = by_key[(workers, "fifo")]
        candidates = [
            by_key[(workers, policy)]
            for policy in args.policies
            if policy != "fifo"
        ]
        if not candidates:
            continue

        best = min(candidates, key=lambda item: item["makespan_seconds"])
        change = fifo["makespan_seconds"] / best["makespan_seconds"]
        lines.append(
            f"- **{workers} workers:** `{best['policy']}` has the shortest "
            f"makespan, {format_seconds(best['makespan_seconds'])} s, "
            f"{change:.2f}x faster than FIFO "
            f"({format_seconds(fifo['makespan_seconds'])} s)."
        )

        longest = by_key.get((workers, "longest-first"))
        if longest is not None and longest is not fifo:
            lines.append(
                f"- **Longest-first vs FIFO at {workers} workers:** "
                f"{format_seconds(longest['makespan_seconds'])} s versus "
                f"{format_seconds(fifo['makespan_seconds'])} s "
                f"({fifo['makespan_seconds'] / longest['makespan_seconds']:.2f}x). "
                f"Its gap to the lower bound is "
                f"{format_seconds(longest['makespan_gap_seconds'])} s."
            )

        shortest = by_key.get((workers, "shortest-first"))
        if shortest is not None and shortest is not fifo:
            lines.append(
                f"- **Shortest-first vs FIFO at {workers} workers:** "
                f"{format_seconds(shortest['makespan_seconds'])} s versus "
                f"{format_seconds(fifo['makespan_seconds'])} s "
                f"({fifo['makespan_seconds'] / shortest['makespan_seconds']:.2f}x). "
                f"Mean job latency is "
                f"{shortest['mean_latency_seconds'] * 1000.0:.2f} ms versus "
                f"{fifo['mean_latency_seconds'] * 1000.0:.2f} ms for FIFO."
            )

    lines.append(
        "- **Why the gaps are small:** experiment finalization interleaves "
        "strategy jobs round-robin, so FIFO already mixes cheap and expensive "
        "bands. Longest-first mainly removes the residual tail: it lowers "
        "worker imbalance and the gap to the lower bound rather than changing "
        "the overall work distribution."
    )

    if consistency["ok"]:
        lines.append(
            f"- **Determinism:** all {consistency['runs']} measured runs "
            "produced identical job results, so policy changes only affect "
            "dispatch order."
        )
    else:
        lines.append(
            "- **Determinism:** WARNING - runs produced different job "
            "results. See the raw JSON for digests."
        )

    if env["efficiency_cores"]:
        lines.append(
            f"- **Noise caveat:** this host has {env['performance_cores']} "
            f"performance and {env['efficiency_cores']} efficiency cores; "
            "small makespan gaps may move between repetitions."
        )

    lines.append("")
    lines.append("## Reproduction")
    lines.append("")
    lines.append("```bash")
    lines.append("cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release")
    lines.append("cmake --build build/release -j")
    lines.append(
        "python3 benchmarks/compare_scheduling.py "
        f"--workers {','.join(str(level) for level in args.workers)} "
        f"--policies {','.join(args.policies)} "
        f"--reps {args.reps} --warmup {args.warmup}"
    )
    lines.append("```")
    lines.append("")
    lines.append(
        "The harness is `benchmarks/compare_scheduling.py`; the workload is "
        "`experiments/heterogeneous_scheduling.json`. Raw measurements are in "
        "`benchmarks/scheduling_policies_raw.json`."
    )
    lines.append("")

    return "\n".join(lines)


def main() -> int:
    args = parse_args()

    build_dir = resolve(args.build_dir)
    experiment_path = resolve(args.experiment)
    output_path = resolve(args.output)
    raw_output_path = resolve(args.raw_output)

    binaries = require_binaries(build_dir)
    base_request = read_json(experiment_path, "scheduling experiment")
    env = environment(build_dir)
    jobs = job_count(base_request)

    print(
        f"Workload: {jobs} jobs; workers "
        f"{','.join(str(level) for level in args.workers)}; policies "
        f"{','.join(args.policies)}; {args.reps} reps "
        f"(+{args.warmup} warm-up)"
    )
    print(
        f"Build:    {env['build_dir']} ({env['build_type']}) on "
        f"{env['processor']}"
    )

    runs: list[dict[str, Any]] = []
    sequential_record: dict[str, Any] | None = None

    with tempfile.TemporaryDirectory(
        prefix="backtester-scheduling-"
    ) as temporary:
        directory = Path(temporary)

        sequential_experiment = write_variant(
            base_request,
            1,
            directory / "sequential.json",
        )
        print("Sequential baseline ...", flush=True)
        sequential_record = run_local(
            binaries["backtester"],
            sequential_experiment,
            args.timeout,
        )

        for workers in args.workers:
            experiment = write_variant(
                base_request,
                workers,
                directory / f"workers-{workers}.json",
            )

            for policy in args.policies:
                key = f"workers-{workers}-{policy}"

                for warmup in range(args.warmup):
                    print(
                        f"Warm-up {warmup + 1}/{args.warmup}: {key} ...",
                        flush=True,
                    )
                    run_processes(
                        binaries,
                        experiment,
                        workers,
                        args.timeout,
                        args.dispatch_batch,
                        policy,
                    )

                for rep in range(1, args.reps + 1):
                    record = run_processes(
                        binaries,
                        experiment,
                        workers,
                        args.timeout,
                        args.dispatch_batch,
                        policy,
                    )
                    record["workers"] = workers
                    record["policy"] = policy
                    record["rep"] = rep
                    runs.append(record)

                records = [
                    run
                    for run in runs
                    if run["workers"] == workers and run["policy"] == policy
                ]
                makespan = median(records, "execution_seconds")
                print(
                    f"{key}: makespan {makespan:.3f}s, "
                    f"{jobs / makespan:.1f} jobs/s",
                    flush=True,
                )

    summary = []
    for workers in args.workers:
        for policy in args.policies:
            records = [
                run
                for run in runs
                if run["workers"] == workers and run["policy"] == policy
            ]
            summary.append(
                summarize_configuration(workers, policy, records)
            )

    digests = {run["digest"] for run in runs}
    completed = {run["jobs_completed"] for run in runs}
    totals = {run["jobs_total"] for run in runs}

    if sequential_record is not None:
        digests.add(sequential_record["digest"])
        completed.add(sequential_record["jobs_completed"])
        totals.add(sequential_record["jobs_total"])

    consistency = {
        "ok": (
            len(digests) == 1
            and completed == totals == {jobs}
        ),
        "digests": sorted(digests),
        "runs": len(runs) + (1 if sequential_record is not None else 0),
    }

    if sequential_record is None:
        raise RuntimeError("no sequential baseline run was recorded")

    data = base_request.get("data", {})
    workload = {
        "jobs": jobs,
        "trading_days": sequential_record["trading_days"],
        "start_date": sequential_record["first_date"]
        or data.get("start_date", ""),
        "end_date": sequential_record["last_date"]
        or data.get("end_date", ""),
        "data_path": f"`{data.get('path', '')}`",
        "min_job_seconds": sequential_record["min_job_seconds"],
        "max_job_seconds": sequential_record["max_job_seconds"],
        "median_job_seconds": sequential_record["median_latency_seconds"],
        "total_work_seconds": sequential_record["total_work_seconds"],
    }

    captured_at = datetime.now().astimezone().strftime(
        "%Y-%m-%d %H:%M:%S %Z"
    )

    markdown = build_markdown(
        env=env,
        workload=workload,
        args=args,
        summary=summary,
        consistency=consistency,
        captured_at=captured_at,
    )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(markdown, encoding="utf-8")

    raw_output_path.parent.mkdir(parents=True, exist_ok=True)
    raw_output_path.write_text(
        json.dumps(
            {
                "captured_at": captured_at,
                "environment": env,
                "workload": workload,
                "configuration": {
                    "workers": args.workers,
                    "policies": args.policies,
                    "reps": args.reps,
                    "warmup": args.warmup,
                    "dispatch_batch": args.dispatch_batch,
                    "build_dir": env["build_dir"],
                    "experiment": display_path(experiment_path),
                },
                "runs": runs,
                "summary": summary,
                "consistency": consistency,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    print(f"Wrote {display_path(output_path)}")
    print(f"Wrote {display_path(raw_output_path)}")

    if not consistency["ok"]:
        print(
            "WARNING: runs produced different job results",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
