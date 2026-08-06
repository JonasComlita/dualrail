#!/usr/bin/env python3
"""Run and validate the deterministic SSA compiler corpus gate.

The C++ harness owns guest compilation/execution and emits the measured JSON.
This small wrapper makes the schema and acceptance policy executable for CMake
CTest and the production target; it never consumes a prior report from the
ignored build tree.
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
SCHEMA_PATH = ROOT / "COMPILER_CORPUS_SCHEMA.json"
REQUIRED_CATEGORIES = {
    "loops",
    "phis",
    "calls",
    "spills",
    "aggregates",
    "ownership",
    "atomics",
    "branches",
    "memory_alias",
}


def issue(issues: list[str], message: str) -> None:
    issues.append(message)


def validate_mode(mode: Any, path: str, issues: list[str]) -> None:
    if not isinstance(mode, dict):
        issue(issues, f"{path} must be an object")
        return
    for key in (
        "compiled",
        "linked",
        "correct",
        "deterministic",
        "repetitions",
        "median",
        "returns",
        "syscalls",
        "diagnostics",
    ):
        if key not in mode:
            issue(issues, f"{path}.{key} is missing")
    repetitions = mode.get("repetitions")
    if not isinstance(repetitions, list) or len(repetitions) != 2:
        issue(issues, f"{path}.repetitions must contain exactly two counts")
    elif any(not isinstance(value, int) or isinstance(value, bool) or value < 0 for value in repetitions):
        issue(issues, f"{path}.repetitions must contain non-negative integers")
    elif repetitions[0] != repetitions[1]:
        issue(issues, f"{path}.repetitions are not deterministic")
    if not isinstance(mode.get("median"), int) or isinstance(mode.get("median"), bool) or mode.get("median", -1) < 0:
        issue(issues, f"{path}.median must be a non-negative integer")
    for key in ("returns", "syscalls", "diagnostics"):
        if not isinstance(mode.get(key), list):
            issue(issues, f"{path}.{key} must be an array")


def validate_report(report: Any) -> list[str]:
    issues: list[str] = []
    try:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"cannot load {SCHEMA_PATH.name}: {exc}"]
    if schema.get("$id") != "trit.compiler_corpus_gate.v1":
        issue(issues, "COMPILER_CORPUS_SCHEMA.json has an unexpected $id")
    if not isinstance(report, dict):
        return ["report must be a JSON object"]
    if report.get("schema") != "trit.compiler_corpus_gate.v1":
        issue(issues, "schema must be trit.compiler_corpus_gate.v1")
    source = report.get("source")
    if not isinstance(source, dict):
        issue(issues, "source must be an object")
    else:
        for key in ("repository", "commit", "corpus_id", "corpus_hash", "pipeline"):
            if not isinstance(source.get(key), str) or not source[key]:
                issue(issues, f"source.{key} must be a non-empty string")
        if source.get("corpus_id") != "compiler-corpus.v1":
            issue(issues, "source.corpus_id must be compiler-corpus.v1")
        if not isinstance(source.get("dirty"), bool):
            issue(issues, "source.dirty must be boolean")
    policy = report.get("policy")
    expected_policy = {
        "o0": "none",
        "optimized": "default",
        "count_repetitions": 2,
        "minimum_median_reduction": 0.15,
        "maximum_workload_regression": 0.05,
    }
    if policy != expected_policy:
        issue(issues, f"policy mismatch: expected {expected_policy!r}")
    workloads = report.get("workloads")
    if not isinstance(workloads, list) or len(workloads) < 8:
        issue(issues, "workloads must contain at least eight entries")
        workloads = []
    seen_categories: set[str] = set()
    for index, workload in enumerate(workloads):
        path = f"workloads[{index}]"
        if not isinstance(workload, dict):
            issue(issues, f"{path} must be an object")
            continue
        for key in (
            "id",
            "source_hash",
            "categories",
            "expected_return",
            "expected_syscall",
            "correctness",
            "counts",
            "reduction",
            "passed",
        ):
            if key not in workload:
                issue(issues, f"{path}.{key} is missing")
        categories = workload.get("categories")
        if not isinstance(categories, list) or not categories:
            issue(issues, f"{path}.categories must be non-empty")
        else:
            seen_categories.update(value for value in categories if isinstance(value, str))
        correctness = workload.get("correctness")
        if not isinstance(correctness, dict):
            issue(issues, f"{path}.correctness must be an object")
        else:
            for mode_name in ("o0", "optimized"):
                status = correctness.get(mode_name)
                if not isinstance(status, dict):
                    issue(issues, f"{path}.correctness.{mode_name} must be an object")
                else:
                    for key in ("compiled", "linked", "correct"):
                        if not isinstance(status.get(key), bool):
                            issue(issues, f"{path}.correctness.{mode_name}.{key} must be boolean")
        counts = workload.get("counts")
        if not isinstance(counts, dict):
            issue(issues, f"{path}.counts must be an object")
        else:
            validate_mode(counts.get("o0"), f"{path}.counts.o0", issues)
            validate_mode(counts.get("optimized"), f"{path}.counts.optimized", issues)
        reduction = workload.get("reduction")
        if not isinstance(reduction, (int, float)) or isinstance(reduction, bool) or not math.isfinite(reduction):
            issue(issues, f"{path}.reduction must be finite numeric")
        if not isinstance(workload.get("passed"), bool):
            issue(issues, f"{path}.passed must be boolean")
    missing = REQUIRED_CATEGORIES - seen_categories
    if missing:
        issue(issues, "missing corpus categories: " + ", ".join(sorted(missing)))
    aggregate = report.get("aggregate")
    if not isinstance(aggregate, dict):
        issue(issues, "aggregate must be an object")
    else:
        for key in (
            "o0_median",
            "optimized_median",
            "median_reduction",
            "maximum_workload_regression",
        ):
            value = aggregate.get(key)
            if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value):
                issue(issues, f"aggregate.{key} must be finite numeric")
    gate = report.get("gate")
    if not isinstance(gate, dict):
        issue(issues, "gate must be an object")
    else:
        if not isinstance(gate.get("passed"), bool):
            issue(issues, "gate.passed must be boolean")
        if gate.get("verdict") not in {"pass", "fail"}:
            issue(issues, "gate.verdict must be pass or fail")
        if not isinstance(gate.get("failures"), list) or any(not isinstance(value, str) for value in gate.get("failures", [])):
            issue(issues, "gate.failures must be an array of strings")
        if gate.get("passed") != (gate.get("verdict") == "pass"):
            issue(issues, "gate.passed and gate.verdict disagree")
    return issues


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="compiled test_compiler_corpus_gate executable")
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    if not binary.exists():
        print(f"compiler corpus binary not found: {binary}", file=sys.stderr)
        return 127
    completed = subprocess.run(
        [str(binary)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.stderr:
        print(completed.stderr, file=sys.stderr, end="")
    try:
        report = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        print(f"compiler corpus emitted invalid JSON: {exc}", file=sys.stderr)
        if completed.stdout:
            print(completed.stdout, file=sys.stderr, end="")
        return 1
    issues = validate_report(report)
    if issues:
        print("compiler corpus JSON contract failed:", file=sys.stderr)
        for item in issues:
            print(f"- {item}", file=sys.stderr)
        return 1
    # Keep the machine-readable artifact on stdout.  It is generated on every
    # invocation and is never read back from the ignored build directory.
    print(json.dumps(report, separators=(",", ":"), sort_keys=False))
    return 0 if completed.returncode == 0 and report["gate"]["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
