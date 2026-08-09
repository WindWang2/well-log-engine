#!/usr/bin/env python3
"""Dense-curve benchmark regression gate (CI).

Parses the JSON emitted by welllog_dense_curve_benchmark and fails the job
when the run shows a significant regression or violates the structural
contract of the scenario.

Two layers of protection:

1. Structural invariants (scenario integrity)
   - schema/scenario/frame count must match the dense-curve scenario
   - prepared_points > 0 (the pipeline must actually feed the GPU)
   - derived/planned memory must stay inside the documented budgets

2. Performance ceilings relative to a committed baseline
   benchmarks/baseline/dense-curve-ubuntu-llvmpipe.json
   A metric fails when: value > baseline * factor + slack_ms
   - the multiplicative factor catches proportional regressions
   - the absolute slack absorbs shared-runner variance for small values

Baseline calibration: after a green run, record the measured JSON as the new
baseline (keep the measured values; the factor/slack do the tolerance work).
Run `--update-baseline <json>` to refresh, then commit the baseline file.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Iterable

DEFAULT_BASELINE = Path(__file__).resolve().parents[2] / "benchmarks" / "baseline" / "dense-curve-ubuntu-llvmpipe.json"

# (json_path, human label)
PERF_METRICS = [
    ("frame_ms.p95", "frame p95 (ms)"),
    ("frame_ms.p99", "frame p99 (ms)"),
    ("frame_plan_ms.p95", "frame-plan p95 (ms)"),
    ("upload_queue_ms.p99", "upload-queue p99 (ms)"),
]

MEMORY_BUDGETS = [
    ("memory_bytes.cpu_derived", "cpu_derived", "cpu_budget"),
    ("memory_bytes.gpu_planned", "gpu_planned", "gpu_budget"),
]


def dig(data: dict, path: str) -> Any:
    node: Any = data
    for part in path.split("."):
        node = node[part]
    return node


def set_path(data: dict, path: str, value: Any) -> None:
    node = data
    parts = path.split(".")
    for part in parts[:-1]:
        node = node.setdefault(part, {})
    node[parts[-1]] = value


def check_structural(data: dict, failures: list[str], warnings: list[str]) -> None:
    if data.get("schema") != "welllog.dense-curve-benchmark.v1":
        failures.append(f"unexpected benchmark schema: {data.get('schema')!r}")
    if data.get("scenario") != "single-curve-500k-4k":
        failures.append(f"unexpected benchmark scenario: {data.get('scenario')!r}")
    if data.get("measured_frames", 0) < 100:
        failures.append(
            f"measured_frames={data.get('measured_frames')} — expected >= 100"
        )
    prepared = dig(data, "prepared_points") if "prepared_points" in data else 0
    if prepared <= 0:
        failures.append("prepared_points <= 0 — dense-curve pipeline produced nothing")
    tasks = data.get("tasks", {})
    if tasks.get("cancelled", 0) != 0:
        warnings.append(f"benchmark tasks cancelled: {tasks['cancelled']}")


def check_memory(data: dict, failures: list[str]) -> None:
    mem = data.get("memory_bytes", {})
    for path, used_key, budget_key in MEMORY_BUDGETS:
        used = mem.get(used_key)
        budget = mem.get(budget_key)
        if used is None or budget is None:
            failures.append(f"memory budget metric missing: {path}")
            continue
        if used > budget:
            failures.append(
                f"{path}: {used} > budget {budget} ({used_key} exceeds {budget_key})"
            )


def check_performance(data: dict, baseline: dict, factor: float, slack_ms: float,
                      failures: list[str]) -> None:
    if not baseline:
        failures.append(
            "baseline file is missing or empty — commit benchmarks/baseline/"
            "dense-curve-ubuntu-llvmpipe.json (see script docstring)"
        )
        return
    for path, label in PERF_METRICS:
        try:
            value = dig(data, path)
            base = dig(baseline, path)
        except (KeyError, TypeError):
            failures.append(f"metric missing from run/baseline: {path}")
            continue
        limit = base * factor + slack_ms
        if value > limit:
            failures.append(
                f"{label}: {value:.3f} > baseline {base:.3f} * {factor} "
                f"+ {slack_ms:g}ms = {limit:.3f} (significant regression)"
            )


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("json", type=Path, help="benchmark JSON file to gate")
    parser.add_argument(
        "--baseline", type=Path, default=DEFAULT_BASELINE,
        help="baseline JSON file (default: %(default)s)",
    )
    parser.add_argument(
        "--factor", type=float, default=1.5,
        help="proportional tolerance over baseline (default: 1.5)",
    )
    parser.add_argument(
        "--slack-ms", type=float, default=5.0,
        help="absolute tolerance in ms (default: 5.0)",
    )
    parser.add_argument(
        "--update-baseline", action="store_true",
        help="overwrite the baseline file with this run's values, then gate",
    )
    args = parser.parse_args(list(argv) if argv is not None else None)

    try:
        data = json.loads(args.json.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print(f"benchmark_gate: cannot read benchmark JSON: {exc}", file=sys.stderr)
        return 2

    if args.update_baseline:
        args.baseline.write_text(
            json.dumps(data, indent=2, sort_keys=True) + "\n")
        print(f"benchmark_gate: baseline updated -> {args.baseline}")

    try:
        baseline = json.loads(args.baseline.read_text())
    except (OSError, json.JSONDecodeError):
        baseline = {}

    failures: list[str] = []
    warnings: list[str] = []
    check_structural(data, failures, warnings)
    check_memory(data, failures)
    check_performance(data, baseline, args.factor, args.slack_ms, failures)

    print("== dense-curve benchmark gate ==")
    print(f"frame_ms.p95 = {dig(data, 'frame_ms.p95'):.3f} ms  "
          f"(baseline {dig(baseline, 'frame_ms.p95') if baseline else '-'})")
    print(f"frame_plan_ms.p95 = {dig(data, 'frame_plan_ms.p95'):.3f} ms")
    print(f"upload_queue_ms.p99 = {dig(data, 'upload_queue_ms.p99'):.3f} ms")
    print(f"prepared_points = {data.get('prepared_points')}")

    for w in warnings:
        print(f"warning: {w}")
    if failures:
        for f in failures:
            print(f"FAIL: {f}")
        return 1
    print("gate: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
