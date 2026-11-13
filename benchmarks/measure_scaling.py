#!/usr/bin/env python3
"""Measure wall-clock time, throughput, and speedup across concurrency levels.

The same 240-job experiment is run:

  sequential  Local runner, one in-flight job.
  threads     Local runner with 2, 4, ... worker threads.
  processes   Coordinator plus N worker processes over loopback TCP.

For each concurrency level the script records end-to-end wall-clock time,
computes throughput as jobs / wall-clock, and reports speedup and parallel
efficiency relative to the sequential baseline.

Usage:
    python3 benchmarks/measure_scaling.py
    python3 benchmarks/measure_scaling.py --levels 1,2,4,8 --reps 5

Requires a built backtester. A release build is recommended:
    cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
    cmake --build build/release -j
"""

from __future__ import annotations

import argparse
import json
import os
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

DEFAULT_EXPERIMENT = REPO_ROOT / "benchmarks" / "benchmark_sweep.json"


def default_levels() -> list[int]:
    cpus = os.cpu_count() or 1
    levels = [1]
    level = 2

    while level < cpus:
        levels.append(level)
        level *= 2

    if cpus not in levels:
        levels.append(cpus)

    return levels


def parse_levels(text: str) -> list[int]:
    levels: set[int] = set()

    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            level = int(part)
        except ValueError as error:
            raise argparse.ArgumentTypeError(
                f"invalid concurrency level '{part}'"
            ) from error
        if level < 1:
            raise argparse.ArgumentTypeError(
                "concurrency levels must be at least 1"
            )
        levels.add(level)

    if not levels:
        raise argparse.ArgumentTypeError("at least one level is required")

    return sorted(levels)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Measure wall-clock time, throughput, and speedup for the "
            "backtester's local-thread and distributed-process execution."
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
        help="Base experiment JSON (default: benchmarks/benchmark_sweep.json).",
    )
    parser.add_argument(
        "--levels",
        type=parse_levels,
        default=default_levels(),
        help=(
            "Comma-separated concurrency levels "
            f"(default: {','.join(str(level) for level in default_levels())})."
        ),
    )
    parser.add_argument(
        "--reps",
        type=int,
        default=3,
        help="Measured repetitions per configuration (default: 3).",
    )
    parser.add_argument(
        "--warmup",
        type=int,
        default=1,
        help="Unmeasured warm-up runs per configuration (default: 1).",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=600.0,
        help="Per-run timeout in seconds (default: 600).",
    )
    parser.add_argument(
        "--output",
        default=str(REPO_ROOT / "benchmarks" / "scaling.md"),
        help="Markdown report path (default: benchmarks/scaling.md).",
    )
    parser.add_argument(
        "--raw-output",
        default=str(REPO_ROOT / "benchmarks" / "scaling_raw.json"),
        help="Raw JSON path (default: benchmarks/scaling_raw.json).",
    )

    args = parser.parse_args()

    if args.reps < 1:
        parser.error("--reps must be at least 1")
    if args.warmup < 0:
        parser.error("--warmup cannot be negative")

    return args


def write_variant(
    base_request: dict[str, Any],
    concurrency: int,
    path: Path,
) -> Path:
    request = json.loads(json.dumps(base_request))
    request.setdefault("execution", {})["max_concurrent_jobs"] = concurrency
    path.write_text(json.dumps(request, indent=2) + "\n", encoding="utf-8")
    return path


def build_configs(levels: list[int]) -> list[dict[str, Any]]:
    configs: list[dict[str, Any]] = [
        {
            "key": "sequential",
            "label": "Sequential",
            "kind": "local",
            "workers": 1,
            "concurrency": 1,
        }
    ]

    for level in levels:
        if level == 1:
            continue
        configs.append(
            {
                "key": f"threads-{level}",
                "label": "Threads",
                "kind": "local",
                "workers": level,
                "concurrency": level,
            }
        )

    for level in levels:
        configs.append(
            {
                "key": f"processes-{level}",
                "label": "Processes",
                "kind": "processes",
                "workers": level,
                "concurrency": level,
            }
        )

    return configs


def run_config(
    config: dict[str, Any],
    binaries: dict[str, Path],
    experiment: Path,
    timeout: float,
) -> dict[str, Any]:
    if config["kind"] == "processes":
        record = run_processes(
            binaries, experiment, config["workers"], timeout
        )
    else:
        record = run_local(binaries["backtester"], experiment, timeout)

    record["config"] = config["key"]
    return record


def summarize(
    config: dict[str, Any],
    records: list[dict[str, Any]],
    jobs: int,
) -> dict[str, Any]:
    wall_seconds = median(records, "harness_seconds")
    walls = [record["harness_seconds"] for record in records]

    return {
        "key": config["key"],
        "label": config["label"],
        "kind": config["kind"],
        "workers": config["workers"],
        "concurrency": config["concurrency"],
        "wall_seconds": wall_seconds,
        "wall_min_seconds": min(walls),
        "wall_max_seconds": max(walls),
        "execution_seconds": median(records, "execution_seconds"),
        "throughput": jobs / wall_seconds,
        "mean_latency_seconds": median(records, "mean_latency_seconds"),
        "p95_latency_seconds": median(records, "p95_latency_seconds"),
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
    by_key = {item["key"]: item for item in summary}
    baseline = by_key["sequential"]["wall_seconds"]

    peak = max(summary, key=lambda item: item["throughput"])
    thread_keys = sorted(
        (item for item in summary if item["kind"] == "local"
         and item["key"] != "sequential"),
        key=lambda item: item["workers"],
    )
    process_keys = sorted(
        (item for item in summary if item["kind"] == "processes"),
        key=lambda item: item["workers"],
    )

    lines: list[str] = []
    lines.append("# Throughput, wall-clock time, and speedup")
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
        "Each concurrency level `N` runs the same experiment with "
        f"`execution.max_concurrent_jobs = N`. Threads use the local runner "
        "with a fixed pool of `N` worker threads; processes use a coordinator "
        "and `N` worker processes over loopback TCP (`--max-attempts 1`)."
    )
    lines.append("")

    lines.append("## Method")
    lines.append("")
    lines.append(
        f"- {args.warmup} warm-up "
        f"{'run' if args.warmup == 1 else 'runs'} per configuration, then "
        f"{args.reps} measured "
        f"{'repetition' if args.reps == 1 else 'repetitions'}; values below "
        "are medians."
    )
    lines.append(
        "- **Wall clock** is measured end to end, from process launch to "
        "runner/coordinator exit, including market-data loading and process "
        "startup."
    )
    lines.append(
        "- **Throughput** is `jobs / wall clock` in jobs per second."
    )
    lines.append(
        "- **Speedup** is `sequential wall clock / configuration wall clock`."
    )
    lines.append(
        "- **Parallel efficiency** is `speedup / workers` as a percentage "
        "of linear scaling."
    )
    lines.append("")

    lines.append("## Results")
    lines.append("")
    lines.append(
        "| Mode | Workers | Wall clock (s) | Range (s) | Throughput (jobs/s) "
        "| Speedup | Efficiency | Execution wall (s) | Mean job latency (ms) |"
    )
    lines.append(
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
    )

    for item in summary:
        speedup = baseline / item["wall_seconds"]
        efficiency = speedup / item["workers"] * 100.0

        lines.append(
            f"| {item['label']} | {item['workers']} "
            f"| {format_seconds(item['wall_seconds'])} "
            f"| {format_seconds(item['wall_min_seconds'])}-"
            f"{format_seconds(item['wall_max_seconds'])} "
            f"| {item['throughput']:.1f} "
            f"| {speedup:.2f}x "
            f"| {efficiency:.0f}% "
            f"| {format_seconds(item['execution_seconds'])} "
            f"| {item['mean_latency_seconds'] * 1000:.2f} |"
        )

    lines.append("")
    lines.append(
        "*Execution wall* is the runner/coordinator's own "
        "`summary.wall_seconds`, which starts after data loading and, for "
        "distributed runs, after workers connect. Process-mode job latency "
        "includes each worker's per-job CSV load; local modes preload the "
        "dataset once."
    )
    lines.append("")

    lines.append("## Observations")
    lines.append("")
    lines.append(
        f"- **Peak throughput:** {peak['label']} at {peak['workers']} "
        f"workers, {peak['throughput']:.1f} jobs/s "
        f"({baseline / peak['wall_seconds']:.2f}x sequential)."
    )

    if thread_keys:
        last_thread = thread_keys[-1]
        lines.append(
            f"- **Thread scaling:** {last_thread['workers']} threads reach "
            f"{last_thread['throughput']:.1f} jobs/s, "
            f"{baseline / last_thread['wall_seconds']:.2f}x sequential "
            f"({(baseline / last_thread['wall_seconds']) / last_thread['workers'] * 100:.0f}% "
            "efficiency)."
        )

    if process_keys:
        last_process = process_keys[-1]
        first_process = process_keys[0]
        process_ratio = (
            last_thread["throughput"] / last_process["throughput"]
            if thread_keys
            else 0.0
        )
        lines.append(
            f"- **Process scaling:** {last_process['workers']} workers reach "
            f"{last_process['throughput']:.1f} jobs/s, "
            f"{baseline / last_process['wall_seconds']:.2f}x sequential "
            f"({(baseline / last_process['wall_seconds']) / last_process['workers'] * 100:.0f}% "
            "efficiency)."
        )
        if first_process["throughput"] < by_key["sequential"]["throughput"]:
            lines.append(
                f"- **Distributed overhead:** one worker sustains "
                f"{first_process['throughput']:.1f} jobs/s, "
                f"{by_key['sequential']['throughput'] / first_process['throughput']:.2f}x "
                "slower than sequential. Each job reloads the market-data "
                "CSV in a worker and crosses a TCP boundary."
            )
        if thread_keys:
            lines.append(
                f"- **Threads vs processes:** the best thread configuration "
                f"is {process_ratio:.2f}x the throughput of the best process "
                "configuration on this workload."
            )

    if consistency["ok"]:
        warmup_text = (
            f" and {consistency['warmups']} warm-up "
            f"{'run' if consistency['warmups'] == 1 else 'runs'}"
            if consistency["warmups"]
            else ""
        )
        lines.append(
            f"- **Determinism:** all {consistency['runs']} measured "
            f"{'run' if consistency['runs'] == 1 else 'runs'}{warmup_text} "
            f"produced identical job results: {workload['jobs']}/"
            f"{workload['jobs']} jobs completed, best job "
            f"`{consistency['best_job_id']}` with Sharpe "
            f"{consistency['best_sharpe_ratio']:.4f}."
        )
    else:
        lines.append(
            "- **Determinism:** WARNING - configurations produced different "
            "job results. See `scaling_raw.json` for digests."
        )

    if env["efficiency_cores"]:
        lines.append(
            f"- **Scaling caveat:** this host has "
            f"{env['performance_cores']} performance and "
            f"{env['efficiency_cores']} efficiency cores; uniform linear "
            "scaling should not be expected."
        )

    lines.append("")
    lines.append("## Reproduction")
    lines.append("")
    lines.append("```bash")
    lines.append("cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release")
    lines.append("cmake --build build/release -j")
    lines.append(
        "python3 benchmarks/measure_scaling.py "
        f"--levels {','.join(str(level) for level in args.levels)} "
        f"--reps {args.reps} --warmup {args.warmup}"
    )
    lines.append("```")
    lines.append("")
    lines.append(
        "The harness is `benchmarks/measure_scaling.py`; shared execution "
        "helpers are in `benchmarks/benchmark_execution.py`. Raw measurements "
        "are in `benchmarks/scaling_raw.json`."
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
    base_request = read_json(experiment_path, "benchmark experiment")
    env = environment(build_dir)
    jobs = job_count(base_request)
    configs = build_configs(args.levels)

    print(
        f"Workload: {jobs} jobs; levels "
        f"{','.join(str(level) for level in args.levels)}; "
        f"{args.reps} reps (+{args.warmup} warm-up)"
    )
    print(
        f"Build:    {env['build_dir']} ({env['build_type']}) on "
        f"{env['processor']}"
    )

    runs: list[dict[str, Any]] = []
    warmup_digests: list[str] = []

    with tempfile.TemporaryDirectory(
        prefix="backtester-scaling-"
    ) as temporary:
        directory = Path(temporary)

        for config in configs:
            experiment = write_variant(
                base_request,
                config["concurrency"],
                directory / f"{config['key']}.json",
            )

            for warmup in range(args.warmup):
                print(
                    f"Warm-up {warmup + 1}/{args.warmup}: "
                    f"{config['key']} ...",
                    flush=True,
                )
                record = run_config(
                    config, binaries, experiment, args.timeout
                )
                warmup_digests.append(record["digest"])

            for rep in range(1, args.reps + 1):
                record = run_config(
                    config, binaries, experiment, args.timeout
                )
                record["rep"] = rep
                runs.append(record)

            records = [run for run in runs if run["config"] == config["key"]]
            wall = median(records, "harness_seconds")
            print(
                f"{config['key']}: {wall:.3f}s, "
                f"{jobs / wall:.1f} jobs/s",
                flush=True,
            )

    summary = [
        summarize(
            config,
            [run for run in runs if run["config"] == config["key"]],
            jobs,
        )
        for config in configs
    ]

    digests = {run["digest"] for run in runs} | set(warmup_digests)
    jobs_completed = {run["jobs_completed"] for run in runs}
    jobs_total = {run["jobs_total"] for run in runs}

    consistency = {
        "ok": (
            len(digests) == 1
            and jobs_completed == jobs_total == {jobs}
        ),
        "digests": sorted(digests),
        "runs": len(runs),
        "warmups": len(warmup_digests),
        "best_job_id": runs[0]["best_job_id"],
        "best_sharpe_ratio": runs[0]["best_sharpe_ratio"],
    }

    data = base_request.get("data", {})
    first_run = runs[0]
    workload = {
        "jobs": jobs,
        "trading_days": first_run["trading_days"],
        "start_date": first_run["first_date"] or data.get("start_date", ""),
        "end_date": first_run["last_date"] or data.get("end_date", ""),
        "data_path": f"`{data.get('path', '')}`",
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
                    "levels": args.levels,
                    "reps": args.reps,
                    "warmup": args.warmup,
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
            "WARNING: configurations produced different job results",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
