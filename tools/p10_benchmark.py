#!/usr/bin/env python3
"""Protocol-aware validation and comparison helpers for TreatCode P10.

The low-level C++ benchmark targets remain useful regression tests, but P10
needs a separate contract for comparing implementations.  This module keeps
that contract deliberately small and dependency-free so it can run in the
same pre-build environment as ``trit_tool.py``.
"""

from __future__ import annotations

import argparse
import copy
import datetime as dt
import hashlib
import json
import math
import platform
import statistics
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = REPO_ROOT / "BENCHMARK_MANIFEST.json"
PROTOCOL_SCHEMA_PATH = REPO_ROOT / "BENCHMARK_PROTOCOL_SCHEMA.json"
REFERENCE_PATH = REPO_ROOT / "benchmarks" / "reference" / "p10-reference.v1.json"
DEFAULT_EVIDENCE_PATH = REPO_ROOT / "build" / "treatcode-plan-evidence" / "P10" / "reference-verification.json"

REQUIRED_PROFILE_IDS = {"binary-host", "vm", "gpu", "fpga", "native-ternary"}
REQUIRED_METRIC_IDS = {
    "wall_time_ns",
    "vm_cycles",
    "instructions",
    "dispatches",
    "memory_read_bytes",
    "memory_write_bytes",
    "code_size_bytes",
    "max_register_pressure",
    "allocations",
}


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_json(path: Path) -> tuple[Any | None, str | None]:
    try:
        return json.loads(path.read_text(encoding="utf-8")), None
    except FileNotFoundError:
        return None, f"missing file: {path}"
    except json.JSONDecodeError as exc:
        return None, f"invalid JSON in {path}: line {exc.lineno}, column {exc.colno}: {exc.msg}"


def _error(code: str, message: str, **extra: Any) -> dict[str, Any]:
    return {"code": code, "message": message, **extra}


def _manifest_metrics(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        str(item.get("id")): item
        for item in manifest.get("metrics", [])
        if isinstance(item, dict) and item.get("id")
    }


def _workloads(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        str(item.get("id")): item
        for item in manifest.get("workloads", [])
        if isinstance(item, dict) and item.get("id")
    }


def _profiles(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        str(item.get("id")): item
        for item in manifest.get("target_profiles", [])
        if isinstance(item, dict) and item.get("id")
    }


def validate_manifest(manifest: Any) -> dict[str, Any]:
    """Validate the semantic invariants that JSON Schema cannot express."""

    errors: list[dict[str, Any]] = []
    if not isinstance(manifest, dict):
        return {"ok": False, "errors": [_error("manifest_type", "benchmark manifest must be an object")]}
    if manifest.get("version") != 1:
        errors.append(_error("manifest_version", "benchmark manifest version must be 1"))
    if manifest.get("schema") != "trit.benchmark_manifest.v1":
        errors.append(_error("manifest_schema", "benchmark manifest schema is not v1"))

    protocol = manifest.get("protocol")
    if not isinstance(protocol, dict):
        errors.append(_error("protocol_missing", "manifest protocol must be an object"))
    else:
        if protocol.get("correctness_gate") != "before_performance":
            errors.append(_error("correctness_order", "correctness must gate performance comparison"))
        if not isinstance(protocol.get("warmups"), int) or protocol["warmups"] < 0:
            errors.append(_error("warmups", "protocol warmups must be a non-negative integer"))
        if not isinstance(protocol.get("repetitions"), int) or protocol["repetitions"] < 1:
            errors.append(_error("repetitions", "protocol repetitions must be a positive integer"))
        variance = protocol.get("variance")
        if not isinstance(variance, dict) or not 0 < float(variance.get("maximum_coefficient_of_variation", 0)):
            errors.append(_error("variance", "protocol must declare a positive maximum coefficient of variation"))
        if not isinstance(protocol.get("limits"), dict):
            errors.append(_error("limits", "protocol must declare runner limits"))
        if not isinstance(protocol.get("same_run_fields"), list) or not protocol["same_run_fields"]:
            errors.append(_error("same_run_fields", "protocol must declare same-run comparison fields"))

    profiles = _profiles(manifest)
    missing_profiles = sorted(REQUIRED_PROFILE_IDS - set(profiles))
    if missing_profiles:
        errors.append(_error("target_profiles", "required target profiles are missing", missing=missing_profiles))
    for profile_id, profile in profiles.items():
        for field in ("execution", "authority", "runner_image"):
            if not str(profile.get(field, "")).strip():
                errors.append(_error("target_profile_field", f"profile {profile_id} is missing {field}"))
        if profile_id in {"binary-host", "vm"} and profile.get("estimate") is True:
            errors.append(_error("target_profile_authority", f"measured profile {profile_id} cannot be estimate-only"))

    metrics = _manifest_metrics(manifest)
    missing_metrics = sorted(REQUIRED_METRIC_IDS - set(metrics))
    if missing_metrics:
        errors.append(_error("metrics", "required P10 metrics are missing", missing=missing_metrics))

    workloads = _workloads(manifest)
    if not workloads:
        errors.append(_error("workloads", "manifest must define at least one workload"))
    kinds = {str(workload.get("kind")) for workload in workloads.values()}
    if "tritwise" not in kinds:
        errors.append(_error("tritwise_pilot", "manifest must define a tritwise pilot"))
    if "vector_matrix" not in kinds:
        errors.append(_error("vector_matrix_pilot", "manifest must define a vector or matrix pilot"))
    for workload_id, workload in workloads.items():
        correctness = workload.get("correctness")
        if not isinstance(correctness, dict) or not str(correctness.get("fixture_id", "")).strip():
            errors.append(_error("workload_correctness", f"workload {workload_id} has no correctness fixture"))
        required = set(workload.get("required_metrics", [])) if isinstance(workload.get("required_metrics"), list) else set()
        if not REQUIRED_METRIC_IDS.issubset(required):
            errors.append(_error("workload_metrics", f"workload {workload_id} omits required metrics", missing=sorted(REQUIRED_METRIC_IDS - required)))
        if workload.get("kind") == "tritwise":
            representation_ids = {
                str(item.get("id"))
                for item in workload.get("representations", [])
                if isinstance(item, dict) and item.get("id")
            }
            if not {"positional-numeric", "lane-rail", "implemented-hardware"}.issubset(representation_ids):
                errors.append(_error("tritwise_representations", f"workload {workload_id} must separate all three sign-inversion representations"))
        if workload.get("kind") == "vector_matrix":
            layout = workload.get("layout")
            if not isinstance(layout, dict) or not isinstance(layout.get("dimensions"), list) or not layout.get("precision"):
                errors.append(_error("vector_layout", f"workload {workload_id} must declare dimensions and precision"))

    return {"ok": not errors, "errors": errors, "profile_ids": sorted(profiles), "metric_ids": sorted(metrics), "workload_ids": sorted(workloads)}


def metric_distribution(samples: list[float | int]) -> dict[str, float]:
    values = [float(value) for value in samples]
    if not values:
        raise ValueError("metric sample set cannot be empty")
    ordered = sorted(values)
    mean = statistics.fmean(values)
    deviation = statistics.pstdev(values) if len(values) > 1 else 0.0
    coefficient = deviation / mean if mean else 0.0
    p95_index = (len(ordered) - 1) * 0.95
    lower = math.floor(p95_index)
    upper = math.ceil(p95_index)
    p95 = ordered[lower] if lower == upper else ordered[lower] + (ordered[upper] - ordered[lower]) * (p95_index - lower)
    return {
        "min": ordered[0],
        "p50": float(statistics.median(values)),
        "p95": float(p95),
        "max": ordered[-1],
        "mean": float(mean),
        "standard_deviation": float(deviation),
        "coefficient_of_variation": float(coefficient),
    }


def _run_context(run: dict[str, Any]) -> dict[str, Any]:
    return {
        "protocol_id": run.get("protocol_id"),
        "workload_id": run.get("workload_id"),
        "representation_id": run.get("representation_id"),
        "profile_id": run.get("profile_id"),
        "input_hash": run.get("input_hash"),
        "runner_image": run.get("runner_image"),
        "limits": run.get("limits"),
    }


def validate_reference(manifest: dict[str, Any], reference: Any) -> dict[str, Any]:
    errors: list[dict[str, Any]] = []
    if not isinstance(reference, dict):
        return {"ok": False, "errors": [_error("reference_type", "reference artifact must be an object")], "runs": []}
    if reference.get("schema") != "trit.benchmark_reference.v1":
        errors.append(_error("reference_schema", "reference artifact schema is not v1"))
    protocol = manifest.get("protocol", {})
    protocol_id = protocol.get("id")
    if reference.get("protocol_id") != protocol_id:
        errors.append(_error("protocol_mismatch", "reference artifact uses a different protocol"))
    if reference.get("manifest_id") != manifest.get("schema"):
        errors.append(_error("manifest_mismatch", "reference artifact uses a different benchmark manifest"))
    if not isinstance(reference.get("environment"), dict) or not reference["environment"].get("fingerprint"):
        errors.append(_error("environment_fingerprint", "reference artifact must include an environment fingerprint"))

    workloads = _workloads(manifest)
    profiles = _profiles(manifest)
    repetitions = int(protocol.get("repetitions", 0))
    max_cv = float(protocol.get("variance", {}).get("maximum_coefficient_of_variation", 0.0))
    runs = reference.get("runs")
    if not isinstance(runs, list) or not runs:
        errors.append(_error("reference_runs", "reference artifact must contain runs"))
        runs = []
    run_by_id: dict[str, dict[str, Any]] = {}
    distribution_records: list[dict[str, Any]] = []
    for position, run in enumerate(runs):
        if not isinstance(run, dict):
            errors.append(_error("run_type", f"reference run {position} must be an object"))
            continue
        run_id = str(run.get("id", ""))
        if not run_id or run_id in run_by_id:
            errors.append(_error("run_id", f"reference run {position} has a missing or duplicate id"))
        else:
            run_by_id[run_id] = run
        workload_id = str(run.get("workload_id", ""))
        profile_id = str(run.get("profile_id", ""))
        workload = workloads.get(workload_id)
        profile = profiles.get(profile_id)
        if workload is None:
            errors.append(_error("run_workload", f"run {run_id} references an unknown workload"))
            continue
        if profile is None:
            errors.append(_error("run_profile", f"run {run_id} references an unknown target profile"))
        if run.get("protocol_id") != protocol_id:
            errors.append(_error("run_protocol", f"run {run_id} uses a different protocol"))
        if profile and run.get("runner_image") != profile.get("runner_image"):
            errors.append(_error("runner_image", f"run {run_id} does not use its target profile runner image"))
        if run.get("input_hash") != workload.get("correctness", {}).get("input_hash"):
            errors.append(_error("input_hash", f"run {run_id} does not use the workload input hash"))
        expected_limits = protocol.get("limits")
        if run.get("limits") != expected_limits:
            errors.append(_error("limits", f"run {run_id} does not preserve the protocol limits"))
        correctness = run.get("correctness")
        if not isinstance(correctness, dict) or correctness.get("equivalent") is not True:
            errors.append(_error("correctness_gate", f"run {run_id} is not correctness-equivalent; performance evidence is rejected"))
        elif (
            correctness.get("fixture_id") != workload.get("correctness", {}).get("fixture_id")
            or correctness.get("reference_output_hash") != workload.get("correctness", {}).get("reference_output_hash")
            or correctness.get("observed_output_hash") != workload.get("correctness", {}).get("reference_output_hash")
        ):
            errors.append(_error("correctness_fixture", f"run {run_id} does not match the declared correctness fixture and output hash"))
        metrics = run.get("metrics")
        if not isinstance(metrics, dict):
            errors.append(_error("run_metrics", f"run {run_id} has no metrics"))
            continue
        required_metrics = set(workload.get("required_metrics", []))
        for metric_id in sorted(required_metrics):
            metric = metrics.get(metric_id)
            if not isinstance(metric, dict) or not isinstance(metric.get("samples"), list):
                errors.append(_error("metric_missing", f"run {run_id} is missing metric {metric_id}"))
                continue
            samples = metric["samples"]
            if len(samples) != repetitions:
                errors.append(_error("sample_count", f"run {run_id} metric {metric_id} has {len(samples)} samples; expected {repetitions}"))
                continue
            if any(not isinstance(value, (int, float)) or isinstance(value, bool) or value < 0 for value in samples):
                errors.append(_error("sample_value", f"run {run_id} metric {metric_id} contains an invalid sample"))
                continue
            distribution = metric_distribution(samples)
            distribution_records.append({"run_id": run_id, "metric_id": metric_id, "distribution": distribution})
            if distribution["coefficient_of_variation"] > max_cv:
                errors.append(_error("variance", f"run {run_id} metric {metric_id} exceeds the declared variance envelope", coefficient_of_variation=distribution["coefficient_of_variation"], maximum=max_cv))

        if workload.get("kind") == "tritwise":
            declared_representations = {str(item.get("id")) for item in workload.get("representations", []) if isinstance(item, dict) and item.get("id")}
            if not run.get("representation_id") or run.get("representation_id") not in declared_representations:
                errors.append(_error("representation", f"tritwise run {run_id} must identify a declared representation"))
        if workload.get("kind") == "vector_matrix":
            layout = workload.get("layout", {})
            context = run.get("context", {})
            for field in ("layout", "dimensions", "precision"):
                expected = layout.get("name") if field == "layout" else layout.get(field)
                if context.get(field) != expected:
                    errors.append(_error("vector_context", f"run {run_id} does not preserve vector layout field {field}"))

    expected_run_ids = {
        str(run_id)
        for workload in workloads.values()
        for run_id in workload.get("reference_runs", [])
    }
    missing_runs = sorted(expected_run_ids - set(run_by_id))
    if missing_runs:
        errors.append(_error("reference_coverage", "declared reference runs are missing", missing=missing_runs))
    observed_profiles = {str(run.get("profile_id")) for run in runs if isinstance(run, dict)}
    if observed_profiles != REQUIRED_PROFILE_IDS:
        errors.append(_error("profile_coverage", "reference artifact must distinguish all five target profiles", observed=sorted(observed_profiles), required=sorted(REQUIRED_PROFILE_IDS)))

    return {
        "ok": not errors,
        "errors": errors,
        "runs": runs,
        "run_count": len(runs),
        "observed_profiles": sorted(observed_profiles),
        "distributions": distribution_records,
        "required_repetitions": repetitions,
    }


def _runtime_environment() -> dict[str, Any]:
    value = {
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "implementation": platform.python_implementation(),
    }
    value["fingerprint"] = sha256_text(canonical_json(value))
    return value


def verify_reference(
    manifest_path: Path = MANIFEST_PATH,
    reference_path: Path = REFERENCE_PATH,
    output_path: Path = DEFAULT_EVIDENCE_PATH,
) -> dict[str, Any]:
    manifest, manifest_error = load_json(manifest_path)
    reference, reference_error = load_json(reference_path)
    report: dict[str, Any] = {
        "schema": "trit.p10_reference_verification.v1",
        "manifest": str(manifest_path),
        "reference": str(reference_path),
        "verification_environment": _runtime_environment(),
        "checks": [],
        "distributions": [],
        "errors": [],
        "correctness_before_performance": True,
    }
    if manifest_error:
        report["errors"].append(_error("manifest_load", manifest_error))
    if reference_error:
        report["errors"].append(_error("reference_load", reference_error))
    if isinstance(manifest, dict):
        manifest_report = validate_manifest(manifest)
        report["manifest_validation"] = manifest_report
        if manifest_report["ok"]:
            report["checks"].append("manifest declares protocol, metrics, pilots, and five target profiles")
        else:
            report["errors"].extend(manifest_report["errors"])
    if isinstance(manifest, dict) and isinstance(reference, dict):
        reference_report = validate_reference(manifest, reference)
        report["reference_validation"] = {key: value for key, value in reference_report.items() if key != "runs"}
        report["distributions"] = reference_report.get("distributions", [])
        if reference_report["ok"]:
            report["checks"].append("reference runs pass correctness equivalence before metric comparison")
            report["checks"].append("reference samples contain warmup-separated repetitions and distributions")
            report["checks"].append("reference repeats remain inside the declared coefficient-of-variation envelope")
            report["checks"].append("binary-host, VM, GPU, FPGA, and native-ternary profiles remain distinct")
        else:
            report["errors"].extend(reference_report["errors"])

        runs = reference_report.get("runs", [])
        first_digest = sha256_text(canonical_json(runs))
        second_digest = sha256_text(canonical_json(copy.deepcopy(runs)))
        report["repeatability"] = {
            "first_digest": first_digest,
            "second_digest": second_digest,
            "same_reference_result": first_digest == second_digest,
            "declared_envelope_ratio": manifest.get("protocol", {}).get("variance", {}).get("repeat_envelope_ratio"),
        }
        if first_digest != second_digest:
            report["errors"].append(_error("repeatability", "repeating the reference fixture changed its result digest"))

    report["ok"] = not report["errors"]
    report["captured_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    report["output"] = str(output_path)
    return report


def compare_candidate(manifest: dict[str, Any], baseline: dict[str, Any], candidate: dict[str, Any]) -> dict[str, Any]:
    """Compare one candidate run after a strict correctness and context gate."""

    errors: list[dict[str, Any]] = []
    baseline_correctness = baseline.get("correctness")
    candidate_correctness = candidate.get("correctness")
    baseline_correct = isinstance(baseline_correctness, dict) and baseline_correctness.get("equivalent") is True
    candidate_correct = isinstance(candidate_correctness, dict) and candidate_correctness.get("equivalent") is True
    if not baseline_correct or not candidate_correct:
        return {
            "schema": "trit.p10_candidate_comparison.v1",
            "passed": False,
            "stage": "correctness",
            "correctness_equivalent": False,
            "errors": [_error("correctness_gate", "candidate and baseline must be equivalent before performance comparison")],
            "comparisons": [],
        }

    protocol = manifest.get("protocol", {})
    workload_id = str(candidate.get("workload_id", ""))
    profile_id = str(candidate.get("profile_id", ""))
    workload = _workloads(manifest).get(workload_id)
    expected_correctness = workload.get("correctness", {}) if workload else {}
    for label, run in (("baseline", baseline), ("candidate", candidate)):
        correctness = run.get("correctness", {})
        if workload and (
            correctness.get("fixture_id") != expected_correctness.get("fixture_id")
            or correctness.get("reference_output_hash") != expected_correctness.get("reference_output_hash")
            or correctness.get("observed_output_hash") != expected_correctness.get("reference_output_hash")
        ):
            errors.append(_error("correctness_fixture", f"{label} does not match the declared correctness fixture and output hash"))
    if workload_id not in _workloads(manifest):
        errors.append(_error("workload_unknown", f"candidate references unknown workload {workload_id}"))
    if profile_id not in _profiles(manifest):
        errors.append(_error("profile_unknown", f"candidate references unknown target profile {profile_id}"))
    for field in protocol.get("same_run_fields", []):
        if _run_context(baseline).get(field) != _run_context(candidate).get(field):
            errors.append(_error("context_mismatch", f"candidate and baseline differ in {field}"))
    profile = _profiles(manifest).get(profile_id, {})
    if profile.get("authority") == "measured":
        baseline_environment = baseline.get("environment")
        candidate_environment = candidate.get("environment")
        baseline_env = baseline_environment.get("fingerprint") if isinstance(baseline_environment, dict) else None
        candidate_env = candidate_environment.get("fingerprint") if isinstance(candidate_environment, dict) else None
        if not baseline_env or not candidate_env or baseline_env != candidate_env:
            errors.append(_error("environment_mismatch", "measured target comparison requires the same environment fingerprint"))
    if any(error.get("code") == "correctness_fixture" for error in errors):
        return {
            "schema": "trit.p10_candidate_comparison.v1",
            "passed": False,
            "stage": "correctness",
            "correctness_equivalent": False,
            "errors": errors,
            "comparisons": [],
        }
    if errors:
        return {
            "schema": "trit.p10_candidate_comparison.v1",
            "passed": False,
            "stage": "context",
            "correctness_equivalent": True,
            "errors": errors,
            "comparisons": [],
        }

    thresholds = protocol.get("regression_thresholds", {})
    comparisons: list[dict[str, Any]] = []
    for metric_id, threshold in thresholds.items():
        base_metric = baseline.get("metrics", {}).get(metric_id)
        candidate_metric = candidate.get("metrics", {}).get(metric_id)
        if not isinstance(base_metric, dict) or not isinstance(candidate_metric, dict):
            continue
        base_samples = base_metric.get("samples", [])
        candidate_samples = candidate_metric.get("samples", [])
        if not base_samples or not candidate_samples:
            errors.append(_error("metric_missing", f"comparison metric {metric_id} has no samples"))
            continue
        base_distribution = metric_distribution(base_samples)
        candidate_distribution = metric_distribution(candidate_samples)
        expected_repetitions = int(protocol.get("repetitions", 0))
        maximum_cv = float(protocol.get("variance", {}).get("maximum_coefficient_of_variation", 0.0))
        if len(base_samples) != expected_repetitions:
            errors.append(_error("sample_count", f"baseline metric {metric_id} has {len(base_samples)} samples; expected {expected_repetitions}"))
        if len(candidate_samples) != expected_repetitions:
            errors.append(_error("sample_count", f"candidate metric {metric_id} has {len(candidate_samples)} samples; expected {expected_repetitions}"))
        if maximum_cv > 0 and base_distribution["coefficient_of_variation"] > maximum_cv:
            errors.append(_error("variance", f"baseline metric {metric_id} exceeds the declared variance envelope"))
        if maximum_cv > 0 and candidate_distribution["coefficient_of_variation"] > maximum_cv:
            errors.append(_error("variance", f"candidate metric {metric_id} exceeds the declared variance envelope"))
        metric_definition = _manifest_metrics(manifest).get(metric_id, {})
        direction = metric_definition.get("direction", "lower_is_better")
        limit = base_distribution["p50"] * (1 - float(threshold)) if direction == "higher_is_better" else base_distribution["p50"] * (1 + float(threshold))
        if direction == "higher_is_better":
            passed = candidate_distribution["p50"] >= limit if base_distribution["p50"] > 0 else candidate_distribution["p50"] >= 0
        else:
            passed = candidate_distribution["p50"] <= limit if base_distribution["p50"] > 0 else candidate_distribution["p50"] <= 0
        comparisons.append({
            "metric_id": metric_id,
            "baseline": base_distribution,
            "candidate": candidate_distribution,
            "threshold_ratio": float(threshold),
            "accepted_p50_limit": limit,
            "direction": direction,
            "passed": passed,
        })
        if not passed:
            errors.append(_error("regression", f"candidate regresses {metric_id} beyond the declared threshold"))

    return {
        "schema": "trit.p10_candidate_comparison.v1",
        "passed": not errors,
        "stage": "performance",
        "correctness_equivalent": True,
        "errors": errors,
        "comparisons": comparisons,
    }


def _cli() -> int:
    parser = argparse.ArgumentParser(description="P10 benchmark protocol tooling")
    sub = parser.add_subparsers(dest="command", required=True)
    verify = sub.add_parser("verify-reference", help="validate the checked-in P10 reference artifact")
    verify.add_argument("--manifest", type=Path, default=MANIFEST_PATH)
    verify.add_argument("--reference", type=Path, default=REFERENCE_PATH)
    verify.add_argument("--output", type=Path, default=DEFAULT_EVIDENCE_PATH)
    verify.add_argument("--json", action="store_true")
    validate = sub.add_parser("validate", help="validate the P10 manifest and reference artifact")
    validate.add_argument("--manifest", type=Path, default=MANIFEST_PATH)
    validate.add_argument("--reference", type=Path, default=REFERENCE_PATH)
    validate.add_argument("--json", action="store_true")
    compare = sub.add_parser("compare", help="compare one candidate run against a reference run")
    compare.add_argument("--manifest", type=Path, default=MANIFEST_PATH)
    compare.add_argument("--baseline", type=Path, required=True, help="JSON file containing a baseline run")
    compare.add_argument("--candidate", type=Path, required=True, help="JSON file containing a candidate run")
    compare.add_argument("--json", action="store_true")
    args = parser.parse_args()

    if args.command == "verify-reference":
        report = verify_reference(args.manifest.resolve(), args.reference.resolve(), args.output.resolve())
    elif args.command == "validate":
        manifest, manifest_error = load_json(args.manifest.resolve())
        reference, reference_error = load_json(args.reference.resolve())
        errors = []
        if manifest_error:
            errors.append(_error("manifest_load", manifest_error))
        if reference_error:
            errors.append(_error("reference_load", reference_error))
        if isinstance(manifest, dict):
            errors.extend(validate_manifest(manifest)["errors"])
        if isinstance(manifest, dict) and isinstance(reference, dict):
            errors.extend(validate_reference(manifest, reference)["errors"])
        report = {"schema": "trit.p10_validation.v1", "ok": not errors, "errors": errors}
    else:
        manifest, manifest_error = load_json(args.manifest.resolve())
        baseline, baseline_error = load_json(args.baseline.resolve())
        candidate, candidate_error = load_json(args.candidate.resolve())
        errors = []
        if manifest_error:
            errors.append(_error("manifest_load", manifest_error))
        if baseline_error:
            errors.append(_error("baseline_load", baseline_error))
        if candidate_error:
            errors.append(_error("candidate_load", candidate_error))
        if errors or not isinstance(manifest, dict) or not isinstance(baseline, dict) or not isinstance(candidate, dict):
            report = {"schema": "trit.p10_candidate_comparison.v1", "passed": False, "stage": "input", "errors": errors or [_error("input_type", "manifest, baseline, and candidate must be JSON objects")], "comparisons": []}
        else:
            report = compare_candidate(manifest, baseline, candidate)

    success = bool(report.get("ok", report.get("passed", False)))
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(f"P10 benchmark {args.command}: {'passed' if success else 'failed'}")
        if report.get("output"):
            print(f"evidence: {report['output']}")
        for error in report.get("errors", [])[:20]:
            print(f"- {error.get('code')}: {error.get('message')}")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(_cli())
