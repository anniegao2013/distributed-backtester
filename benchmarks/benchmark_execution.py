#!/usr/bin/env python3
"""Benchmark sequential, thread-based, and process-based execution.

The same 240-job experiment is run three ways:

  sequential  Local runner with execution.max_concurrent_jobs = 1.
  threaded    Local runner with max_concurrent_jobs = N (N threads).
  processes   Coordinator plus N worker processes over loopback TCP.

Every mode is warmed once, then measured for several repetitions. The script
writes a markdown report (and optional raw JSON) with wall times, throughput,
speedups, and per-job latency.

Usage:
    python3 benchmarks/benchmark_execution.py
    python3 benchmarks/benchmark_execution.py --reps 10 --workers 4

Requires a built backtester. A release build is recommended:
    cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
    cmake --build build/release -j
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EXPERIMENT = REPO_ROOT / "benchmarks" / "benchmark_sweep.json"
MODES = ("sequential", "threaded", "processes")
MODE_LABELS = {
    "sequential": "Sequential",
    "threaded": "Threads",
    "processes": "Processes",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Benchmark the backtester's sequential, thread-based, and "
            "process-based execution modes."
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
        "--workers",
        type=int,
        default=os.cpu_count() or 1,
        help="Threads/processes for parallel modes (default: logical CPUs).",
    )
    parser.add_argument(
        "--reps",
        type=int,
        default=5,
        help="Measured repetitions per mode (default: 5).",
    )
    parser.add_argument(
        "--warmup",
        type=int,
        default=1,
        help="Unmeasured warm-up runs per mode (default: 1).",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=600.0,
        help="Per-run timeout in seconds (default: 600).",
    )
    parser.add_argument(
        "--output",
        default=str(REPO_ROOT / "benchmarks" / "results.md"),
        help="Markdown report path (default: benchmarks/results.md).",
    )
    parser.add_argument(
        "--raw-output",
        default=str(REPO_ROOT / "benchmarks" / "raw_results.json"),
        help="Raw JSON results path (default: benchmarks/raw_results.json).",
    )

    args = parser.parse_args()

    if args.workers < 1:
        parser.error("--workers must be at least 1")
    if args.reps < 1:
        parser.error("--reps must be at least 1")
    if args.warmup < 0:
        parser.error("--warmup cannot be negative")

    return args


def resolve(path: str) -> Path:
    candidate = Path(path).expanduser()
    return candidate if candidate.is_absolute() else REPO_ROOT / candidate


def require_binaries(build_dir: Path) -> dict[str, Path]:
    binaries = {
        "backtester": build_dir / "backtester",
        "coordinator": build_dir / "backtester_coordinator",
        "worker": build_dir / "backtester_worker",
    }
    missing = [str(path) for path in binaries.values() if not path.is_file()]

    if missing:
        sys.exit(
            "Missing benchmark binaries:\n  "
            + "\n  ".join(missing)
            + "\n\nBuild them first, for example:\n"
            + "  cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release\n"
            + "  cmake --build build/release -j"
        )

    return binaries


def sysctl(name: str) -> str | None:
    try:
        result = subprocess.run(
            ["sysctl", "-n", name],
            capture_output=True,
            text=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return result.stdout.strip() or None


def cpu_model() -> str:
    if platform.system() == "Darwin":
        model = sysctl("machdep.cpu.brand_string")
        if model:
            return model

    if platform.system() == "Linux":
        try:
            with open("/proc/cpuinfo", encoding="utf-8") as handle:
                for line in handle:
                    if line.startswith("model name"):
                        return line.split(":", 1)[1].strip()
        except OSError:
            pass

    return platform.processor() or platform.machine() or "unknown"


def physical_cpus() -> int | None:
    if platform.system() == "Darwin":
        value = sysctl("hw.physicalcpu")
        if value and value.isdigit():
            return int(value)

    if platform.system() == "Linux":
        try:
            with open("/proc/cpuinfo", encoding="utf-8") as handle:
                cores = {
                    line.split(":", 1)[1].strip()
                    for line in handle
                    if line.startswith("core id")
                }
        except OSError:
            return None
        return len(cores) or None

    return None


def build_type(build_dir: Path) -> str:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return "unknown"

    try:
        for line in cache.read_text(encoding="utf-8").splitlines():
            if line.startswith("CMAKE_BUILD_TYPE:"):
                value = line.split("=", 1)[1].strip()
                return value or "default (no optimization flags)"
    except OSError:
        pass

    return "unknown"


def performance_core_counts() -> tuple[int | None, int | None]:
    if platform.system() != "Darwin":
        return None, None

    performance = sysctl("hw.perflevel0.physicalcpu")
    efficiency = sysctl("hw.perflevel1.physicalcpu")

    return (
        int(performance) if performance and performance.isdigit() else None,
        int(efficiency) if efficiency and efficiency.isdigit() else None,
    )


def display_path(path: Path) -> str:
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def environment(build_dir: Path) -> dict[str, Any]:
    performance_cores, efficiency_cores = performance_core_counts()

    return {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": cpu_model(),
        "logical_cpus": os.cpu_count() or 1,
        "physical_cpus": physical_cpus(),
        "performance_cores": performance_cores,
        "efficiency_cores": efficiency_cores,
        "python": platform.python_version(),
        "build_dir": display_path(build_dir),
        "build_type": build_type(build_dir),
    }


def read_json(path: Path, description: str) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"Could not read {description} at {path}: {error}")


def write_experiment_variants(
    base_request: dict[str, Any],
    workers: int,
    directory: Path,
) -> dict[str, Path]:
    variants: dict[str, Path] = {}

    for mode, concurrency in (
        ("sequential", 1),
        ("threaded", workers),
        ("processes", workers),
    ):
        request = json.loads(json.dumps(base_request))
        execution = request.setdefault("execution", {})
        execution["max_concurrent_jobs"] = concurrency

        path = directory / f"{mode}.json"
        path.write_text(json.dumps(request, indent=2) + "\n", encoding="utf-8")
        variants[mode] = path

    return variants


def job_count(request: dict[str, Any]) -> int:
    total = 0

    for strategy in request.get("strategies", []):
        grid = strategy.get("grid")
        if isinstance(grid, dict) and grid:
            combinations = 1
            for values in grid.values():
                combinations *= len(values)
            total += combinations
        else:
            total += 1

    return total


def results_digest(document: dict[str, Any]) -> str:
    """Hashes the job outputs that every execution mode must reproduce."""
    digest = hashlib.sha256()

    for result in document.get("results", []):
        parameters = result.get("parameters", {})
        ordered_parameters = ",".join(
            f"{key}={parameters[key]!r}" for key in sorted(parameters)
        )
        metrics = result.get("metrics", {})

        digest.update(
            (
                f"{result.get('job_id')}|{result.get('strategy_id')}|"
                f"{ordered_parameters}|"
                f"{metrics.get('total_return', 0.0):.12g}|"
                f"{metrics.get('sharpe_ratio', 0.0):.12g}|"
                f"{metrics.get('max_drawdown', 0.0):.12g}|"
                f"{metrics.get('number_of_trades', 0)}\n"
            ).encode("utf-8")
        )

    return digest.hexdigest()


def worker_sort_key(worker_id: str) -> tuple[int, str]:
    match = re.search(r"(\d+)$", worker_id)
    return (int(match.group(1)) if match else 0, worker_id)


def parse_worker_stats(
    coordinator_log: str,
    document: dict[str, Any],
) -> list[dict[str, Any]]:
    """Maps coordinator dispatch lines to worker-side job durations.

    This is only available for distributed runs. It lets the scheduling
    analysis measure per-worker busy time and job balance, not just the
    aggregate sum of job durations.
    """
    durations = {
        result["job_id"]: float(result.get("duration_seconds", 0.0))
        for result in document.get("results", [])
    }
    assignments = re.findall(
        r"assigning job '([^']+)' to worker '([^']+)'",
        coordinator_log,
    )
    wall_seconds = float(
        document.get("summary", {}).get("wall_seconds", 0.0)
    )

    by_worker: dict[str, dict[str, Any]] = {}

    for job_id, worker_id in assignments:
        entry = by_worker.setdefault(
            worker_id,
            {"worker_id": worker_id, "jobs": 0, "busy_seconds": 0.0},
        )
        entry["jobs"] += 1
        entry["busy_seconds"] += durations.get(job_id, 0.0)

    for entry in by_worker.values():
        entry["utilization"] = (
            entry["busy_seconds"] / wall_seconds if wall_seconds > 0 else 0.0
        )

    return [
        by_worker[worker_id]
        for worker_id in sorted(by_worker, key=worker_sort_key)
    ]


def summarize(
    document: dict[str, Any],
    harness_seconds: float,
) -> dict[str, Any]:
    summary = document.get("summary", {})
    performance = summary.get("performance", {})
    data = document.get("data", {})

    if summary.get("jobs_completed") != summary.get("jobs_total"):
        raise RuntimeError(
            "Run did not complete every job: "
            f"{summary.get('jobs_completed')} of {summary.get('jobs_total')}"
        )
    if summary.get("jobs_failed", 0) != 0:
        raise RuntimeError(
            f"Run reported {summary['jobs_failed']} failed job(s)"
        )

    return {
        "harness_seconds": harness_seconds,
        "execution_seconds": float(summary["wall_seconds"]),
        "overhead_seconds": harness_seconds - float(summary["wall_seconds"]),
        "jobs_total": int(summary["jobs_total"]),
        "jobs_completed": int(summary["jobs_completed"]),
        "trading_days": int(data.get("trading_days", 0)),
        "first_date": data.get("first_date", ""),
        "last_date": data.get("last_date", ""),
        "throughput": float(performance["throughput"]),
        "mean_latency_seconds": float(performance["mean_latency"]),
        "median_latency_seconds": float(performance["median_latency"]),
        "p95_latency_seconds": float(performance["p95_latency"]),
        "total_work_seconds": float(performance["total_work_seconds"]),
        "cpu_seconds": float(performance["cpu_seconds"]),
        "cpu_utilization": float(performance["cpu_utilization"]),
        "max_concurrent": int(performance["max_concurrent"]),
        "best_job_id": summary.get("best_job_id", ""),
        "best_sharpe_ratio": float(summary.get("best_sharpe_ratio", 0.0)),
        "best_total_return": float(summary.get("best_total_return", 0.0)),
        "digest": results_digest(document),
    }


def run_local(
    binary: Path,
    experiment: Path,
    timeout: float,
) -> dict[str, Any]:
    command = [str(binary), "--experiment", str(experiment), "--json"]

    start = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    harness_seconds = time.perf_counter() - start

    if completed.returncode != 0:
        raise RuntimeError(
            f"{binary.name} exited with {completed.returncode}\n"
            f"stdout:\n{completed.stdout[-4000:]}\n"
            f"stderr:\n{completed.stderr[-4000:]}"
        )

    try:
        document = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(
            f"{binary.name} did not emit valid JSON: {error}\n"
            f"stdout:\n{completed.stdout[-4000:]}"
        ) from error

    return summarize(document, harness_seconds)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def wait_for_listening(
    coordinator: subprocess.Popen[bytes],
    stderr_path: Path,
    deadline: float,
) -> None:
    """Waits for the coordinator's bind log line without connecting a probe."""
    while time.monotonic() < deadline:
        if coordinator.poll() is not None:
            try:
                stderr = stderr_path.read_text(
                    encoding="utf-8", errors="replace"
                )
            except OSError:
                stderr = ""
            raise RuntimeError(
                "Coordinator exited before listening "
                f"(code {coordinator.returncode})\n{stderr[-4000:]}"
            )

        try:
            if "listening for workers on" in stderr_path.read_text(
                encoding="utf-8", errors="replace"
            ):
                return
        except OSError:
            pass

        time.sleep(0.01)

    raise TimeoutError("Timed out waiting for the coordinator to listen")


def stop_processes(
    processes: list[subprocess.Popen[bytes]],
    grace_seconds: float = 10.0,
) -> None:
    deadline = time.monotonic() + grace_seconds

    for process in processes:
        if process.poll() is None:
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGTERM)
            except (ProcessLookupError, PermissionError):
                process.terminate()

    for process in processes:
        remaining = max(0.1, deadline - time.monotonic())
        try:
            process.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                process.kill()
            process.wait(timeout=2.0)


def run_processes(
    binaries: dict[str, Path],
    experiment: Path,
    workers: int,
    timeout: float,
) -> dict[str, Any]:
    port = free_port()

    with tempfile.TemporaryDirectory(prefix="backtester-bench-") as temporary:
        directory = Path(temporary)
        stdout_path = directory / "coordinator.json"
        stderr_path = directory / "coordinator.stderr"
        worker_log_path = directory / "workers.stderr"

        coordinator_command = [
            str(binaries["coordinator"]),
            "--experiment",
            str(experiment),
            "--workers",
            str(workers),
            "--listen",
            f"127.0.0.1:{port}",
            "--max-attempts",
            "1",
            "--json",
        ]

        worker_procs: list[subprocess.Popen[bytes]] = []
        start = time.perf_counter()

        with (
            open(stdout_path, "wb") as coordinator_stdout,
            open(stderr_path, "wb") as coordinator_stderr,
            open(worker_log_path, "ab") as worker_log,
        ):
            coordinator = subprocess.Popen(
                coordinator_command,
                cwd=REPO_ROOT,
                stdout=coordinator_stdout,
                stderr=coordinator_stderr,
                start_new_session=True,
            )

            try:
                wait_for_listening(
                    coordinator,
                    stderr_path,
                    time.monotonic() + min(30.0, timeout),
                )

                for index in range(workers):
                    worker_procs.append(
                        subprocess.Popen(
                            [
                                str(binaries["worker"]),
                                "--worker-id",
                                f"worker-{index + 1}",
                                "--connect",
                                f"127.0.0.1:{port}",
                            ],
                            cwd=REPO_ROOT,
                            stdout=subprocess.DEVNULL,
                            stderr=worker_log,
                            start_new_session=True,
                        )
                    )

                remaining = timeout - (time.perf_counter() - start)
                if remaining <= 0:
                    raise TimeoutError(
                        f"Distributed run exceeded {timeout:.0f}s"
                    )
                coordinator.wait(timeout=remaining)
            except BaseException:
                stop_processes([coordinator, *worker_procs], grace_seconds=2.0)
                raise

            harness_seconds = time.perf_counter() - start
            stop_processes(worker_procs)

        try:
            stdout = stdout_path.read_text(encoding="utf-8")
            stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
            worker_log = worker_log_path.read_text(
                encoding="utf-8", errors="replace"
            )
        except OSError as error:
            raise RuntimeError(f"Could not read run output: {error}") from error

        if coordinator.returncode != 0:
            raise RuntimeError(
                f"Coordinator exited with {coordinator.returncode}\n"
                f"stdout:\n{stdout[-4000:]}\n"
                f"stderr:\n{stderr[-4000:]}\n"
                f"workers:\n{worker_log[-4000:]}"
            )

        try:
            document = json.loads(stdout)
        except json.JSONDecodeError as error:
            raise RuntimeError(
                f"Coordinator did not emit valid JSON: {error}\n"
                f"stdout:\n{stdout[-4000:]}\n"
                f"stderr:\n{stderr[-4000:]}"
            ) from error

    record = summarize(document, harness_seconds)
    record["worker_stats"] = parse_worker_stats(stderr, document)
    return record


def run_mode(
    mode: str,
    binaries: dict[str, Path],
    experiments: dict[str, Path],
    workers: int,
    timeout: float,
) -> dict[str, Any]:
    if mode == "processes":
        record = run_processes(
            binaries, experiments[mode], workers, timeout
        )
    else:
        record = run_local(binaries["backtester"], experiments[mode], timeout)

    record["mode"] = mode
    return record


def median(records: list[dict[str, Any]], key: str) -> float:
    return float(statistics.median(record[key] for record in records))


def format_seconds(value: float) -> str:
    return f"{value:.3f}"


def build_markdown(
    *,
    env: dict[str, Any],
    workload: dict[str, Any],
    args: argparse.Namespace,
    runs: list[dict[str, Any]],
    summary: dict[str, dict[str, float]],
    consistency: dict[str, Any],
    captured_at: str,
) -> str:
    sequential = summary["sequential"]
    threaded = summary["threaded"]
    processes = summary["processes"]

    thread_harness_speedup = sequential["harness_seconds"] / threaded["harness_seconds"]
    process_harness_speedup = sequential["harness_seconds"] / processes["harness_seconds"]
    thread_execution_speedup = sequential["execution_seconds"] / threaded["execution_seconds"]
    process_execution_speedup = sequential["execution_seconds"] / processes["execution_seconds"]
    process_vs_threads = processes["execution_seconds"] / threaded["execution_seconds"]

    thread_efficiency = thread_execution_speedup / args.workers * 100.0
    process_efficiency = process_execution_speedup / args.workers * 100.0

    lines: list[str] = []
    lines.append("# Execution benchmark: sequential vs threads vs processes")
    lines.append("")
    lines.append(f"Captured {captured_at} on this machine:")
    lines.append("")
    lines.append("| Property | Value |")
    lines.append("| --- | --- |")
    lines.append(f"| CPU | {env['processor']} |")
    if env["performance_cores"] and env["efficiency_cores"]:
        core_note = (
            f"{env['physical_cpus'] or 'n/a'} physical "
            f"({env['performance_cores']} performance + "
            f"{env['efficiency_cores']} efficiency), "
            f"{env['logical_cpus']} logical"
        )
    else:
        core_note = (
            f"{env['physical_cpus'] or 'n/a'} physical, "
            f"{env['logical_cpus']} logical"
        )
    lines.append(f"| Cores | {core_note} |")
    lines.append(f"| OS | {env['platform']} |")
    lines.append(f"| Build | `{env['build_dir']}` ({env['build_type']}) |")
    lines.append(f"| Python | {env['python']} |")
    lines.append("")

    lines.append("## Workload")
    lines.append("")
    lines.append(
        f"{workload['jobs']} independent backtests: a moving-average crossover "
        f"grid of {workload['short_windows']} short windows x "
        f"{workload['long_windows']} long windows, over "
        f"{workload['trading_days']:,} daily bars "
        f"({workload['start_date']} to {workload['end_date']}, "
        f"{workload['data_path']})."
    )
    lines.append("")
    lines.append("| Mode | Concurrency | How it runs |")
    lines.append("| --- | --- | --- |")
    lines.append(
        "| Sequential | 1 | Local runner with "
        "`execution.max_concurrent_jobs = 1` (one in-flight job). |"
    )
    lines.append(
        f"| Threads | {args.workers} | Local runner with "
        f"`max_concurrent_jobs = {args.workers}`; one `std::async` worker "
        "thread per in-flight job. |"
    )
    lines.append(
        f"| Processes | {args.workers} | Coordinator plus {args.workers} "
        "`backtester_worker` processes over loopback TCP "
        "(`--max-attempts 1`). |"
    )
    lines.append("")

    lines.append("## Results")
    lines.append("")
    lines.append(
        f"Medians across {args.reps} measured "
        f"{'repetition' if args.reps == 1 else 'repetitions'}, after "
        f"{args.warmup} warm-up "
        f"{'run' if args.warmup == 1 else 'runs'} per mode."
    )
    lines.append("")
    lines.append(
        "| Mode | Concurrency | Harness wall (s) | Execution wall (s) | "
        "Fixed overhead (s) | Throughput (jobs/s) | Speedup vs sequential "
        "(harness) | Speedup vs sequential (execution) | Mean job latency "
        "(ms) | p95 job latency (ms) |"
    )
    lines.append(
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
    )

    for mode in MODES:
        stats = summary[mode]
        harness_speedup = sequential["harness_seconds"] / stats["harness_seconds"]
        execution_speedup = (
            sequential["execution_seconds"] / stats["execution_seconds"]
        )
        lines.append(
            f"| {MODE_LABELS[mode]} | {int(stats['max_concurrent'])} "
            f"| {format_seconds(stats['harness_seconds'])} "
            f"| {format_seconds(stats['execution_seconds'])} "
            f"| {format_seconds(stats['overhead_seconds'])} "
            f"| {stats['throughput']:.1f} "
            f"| {harness_speedup:.2f}x "
            f"| {execution_speedup:.2f}x "
            f"| {stats['mean_latency_seconds'] * 1000:.2f} "
            f"| {stats['p95_latency_seconds'] * 1000:.2f} |"
        )

    lines.append("")
    lines.append(
        "*Harness wall* is measured from process launch to runner/coordinator "
        "exit. *Execution wall* is the run's own `summary.wall_seconds`, "
        "which starts after market data is loaded and, for distributed runs, "
        "after workers connect. *Fixed overhead* is their difference. "
        "Throughput and latency come from each run's result document. "
        "Process-mode latency includes each worker's per-job market-data "
        "load; local modes preload the dataset once."
    )
    lines.append("")

    lines.append("### Repetitions")
    lines.append("")
    lines.append(
        "Each cell is `harness wall / execution wall` in seconds."
    )
    lines.append("")
    lines.append(
        "| Rep | Sequential | Threads | Processes |"
    )
    lines.append("| ---: | ---: | ---: | ---: |")

    for rep in range(1, args.reps + 1):
        cells = []
        for mode in MODES:
            record = next(
                run
                for run in runs
                if run["mode"] == mode and run["rep"] == rep
            )
            cells.append(
                f"{format_seconds(record['harness_seconds'])} / "
                f"{format_seconds(record['execution_seconds'])}"
            )
        lines.append(f"| {rep} | " + " | ".join(cells) + " |")

    lines.append("")
    lines.append("## Observations")
    lines.append("")
    lines.append(
        f"- **Threads:** {thread_harness_speedup:.2f}x faster end to end "
        f"({format_seconds(sequential['harness_seconds'])} s -> "
        f"{format_seconds(threaded['harness_seconds'])} s) and "
        f"{thread_execution_speedup:.2f}x faster in the execution phase. "
        f"That is {thread_efficiency:.0f}% of linear scaling on "
        f"{args.workers} workers."
    )
    lines.append(
        f"- **Processes:** {process_harness_speedup:.2f}x faster end to end "
        f"({format_seconds(sequential['harness_seconds'])} s -> "
        f"{format_seconds(processes['harness_seconds'])} s) and "
        f"{process_execution_speedup:.2f}x faster in the execution phase "
        f"({process_efficiency:.0f}% of linear scaling)."
    )
    lines.append(
        f"- **Processes vs threads:** the process pipeline takes "
        f"{process_vs_threads:.2f}x longer in the execution phase. Each "
        "worker reloads the market-data CSV for every job and results cross "
        "a TCP boundary, so remote mean job latency is "
        f"{processes['mean_latency_seconds'] * 1000:.1f} ms versus "
        f"{threaded['mean_latency_seconds'] * 1000:.1f} ms locally."
    )
    process_overhead_share = (
        processes["overhead_seconds"] / processes["harness_seconds"] * 100.0
    )
    lines.append(
        f"- **Fixed overhead:** sequential/threaded runs spend about "
        f"{sequential['overhead_seconds']:.2f} s outside the execution timer "
        "(market-data load); processes spend "
        f"{processes['overhead_seconds']:.2f} s on coordinator load, worker "
        f"startup, and shutdown - about {process_overhead_share:.0f}% of "
        "distributed end-to-end time. The relative cost grows for shorter "
        "runs."
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
            "produced identical job results: "
            f"{workload['jobs']}/{workload['jobs']} jobs completed, best job "
            f"`{consistency['best_job_id']}` with Sharpe "
            f"{consistency['best_sharpe_ratio']:.4f}."
        )
    else:
        lines.append(
            "- **Determinism:** WARNING - execution modes produced "
            "different job results. See `raw_results.json` for digests."
        )

    if env["efficiency_cores"]:
        lines.append(
            f"- **Scaling caveat:** this host has "
            f"{env['performance_cores']} performance and "
            f"{env['efficiency_cores']} efficiency cores, so sub-linear "
            "scaling is expected even without synchronization or I/O "
            "overhead."
        )

    lines.append("")
    lines.append("## Reproduction")
    lines.append("")
    lines.append("```bash")
    lines.append("cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release")
    lines.append("cmake --build build/release -j")
    lines.append(
        f"python3 benchmarks/benchmark_execution.py "
        f"--build-dir build/release --workers {args.workers} "
        f"--reps {args.reps} --warmup {args.warmup}"
    )
    lines.append("```")
    lines.append("")
    lines.append(
        "The harness is `benchmarks/benchmark_execution.py`; the workload is "
        "`benchmarks/benchmark_sweep.json`. Raw measurements are in "
        "`benchmarks/raw_results.json`."
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
    strategy = base_request["strategies"][0]
    switches = strategy.get("grid", {})

    print(
        f"Workload: {jobs} jobs, {args.workers} workers, "
        f"{args.reps} reps (+{args.warmup} warm-up)"
    )
    print(
        f"Build:    {env['build_dir']} ({env['build_type']}) on "
        f"{env['processor']}"
    )

    with tempfile.TemporaryDirectory(
        prefix="backtester-experiments-"
    ) as temporary:
        experiments = write_experiment_variants(
            base_request, args.workers, Path(temporary)
        )

        warmup_digests: list[str] = []

        for warmup in range(args.warmup):
            for mode in MODES:
                print(
                    f"Warm-up {warmup + 1}/{args.warmup}: {mode} ...",
                    flush=True,
                )
                warmup_record = run_mode(
                    mode,
                    binaries,
                    experiments,
                    args.workers,
                    args.timeout,
                )
                warmup_digests.append(warmup_record["digest"])

        runs: list[dict[str, Any]] = []

        for rep in range(1, args.reps + 1):
            parts = []

            for mode in MODES:
                record = run_mode(
                    mode,
                    binaries,
                    experiments,
                    args.workers,
                    args.timeout,
                )
                record["rep"] = rep
                runs.append(record)
                parts.append(
                    f"{mode}={record['harness_seconds']:.3f}s"
                )

            print(f"Rep {rep}/{args.reps}: " + ", ".join(parts), flush=True)

    summary: dict[str, dict[str, float]] = {}

    for mode in MODES:
        records = [run for run in runs if run["mode"] == mode]
        summary[mode] = {
            "harness_seconds": median(records, "harness_seconds"),
            "execution_seconds": median(records, "execution_seconds"),
            "overhead_seconds": median(records, "overhead_seconds"),
            "throughput": median(records, "throughput"),
            "mean_latency_seconds": median(records, "mean_latency_seconds"),
            "p95_latency_seconds": median(records, "p95_latency_seconds"),
            "max_concurrent": float(
                statistics.median(
                    record["max_concurrent"] for record in records
                )
            ),
        }

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

    data_summary = base_request.get("data", {})
    first_run = runs[0]

    # The result document carries the authoritative trading-day count and
    # actual first/last bar.
    captured_at = datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")

    workload = {
        "jobs": jobs,
        "short_windows": len(switches.get("short_window", [])),
        "long_windows": len(switches.get("long_window", [])),
        "trading_days": first_run["trading_days"],
        "start_date": first_run["first_date"]
        or data_summary.get("start_date", ""),
        "end_date": first_run["last_date"] or data_summary.get("end_date", ""),
        "data_path": f"`{data_summary.get('path', '')}`",
    }

    markdown = build_markdown(
        env=env,
        workload=workload,
        args=args,
        runs=runs,
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
            "WARNING: execution modes produced different job results",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
