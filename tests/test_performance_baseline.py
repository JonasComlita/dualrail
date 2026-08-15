"""Focused contract checks for the reproducible performance baseline harness."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
import performance_baseline as baseline  # noqa: E402


def test_schema_declares_required_surfaces_and_advisory_timing() -> None:
    schema = json.loads(
        (ROOT / "benchmarks" / "performance_baseline_schema.json").read_text(encoding="utf-8")
    )
    assert schema["$id"].endswith("performance_baseline.v1.schema.json")
    required = schema["properties"]["metrics"]["required"]
    for name in baseline.REQUIRED_METRICS:
        assert name in required
    assert schema["$defs"]["protocol"]["properties"]["timing_threshold_enforced"] == {"const": False}
    assert schema["$defs"]["timing_metric"]["properties"]["authority"] == {"const": "advisory"}


def test_reference_is_functional_only_and_matches_schema_contract() -> None:
    reference = json.loads(
        (ROOT / "benchmarks" / "reference" / "performance-baseline.v1.json").read_text(encoding="utf-8")
    )
    result = baseline.validate_report(reference, reference=reference)
    assert result["ok"], result
    assert reference["policy"]["reference"] == "functional_counts_only"
    for name in baseline.FUNCTIONAL_METRICS:
        metric = reference["metrics"][name]
        assert metric["deterministic"] is True
        assert metric["authority"] == "functional"
    for name in baseline.TIMING_METRICS:
        metric = reference["metrics"][name]
        assert metric["deterministic"] is False
        assert metric["authority"] == "advisory"


def test_collection_repeats_functional_trace_without_timing_gate(tmp_path: Path) -> None:
    reference = json.loads(
        (ROOT / "benchmarks" / "reference" / "performance-baseline.v1.json").read_text(encoding="utf-8")
    )
    report = baseline.collect_baseline(
        output=tmp_path / "baseline.json", warmups=1, iterations=3,
        frames=3, disk_words=27, app_words=9,
    )
    result = baseline.validate_report(report, reference=reference)
    # The small custom trace intentionally differs from the 7-frame reference;
    # validate the shape first and compare repeatability independently.
    assert baseline.validate_report(report)["ok"]
    assert result["ok"] is False
    assert report["reproducibility"]["functional_match"] is True
    assert report["reproducibility"]["trace_repeatable"] is True
    assert report["protocol"]["timing_threshold_enforced"] is False
    for name in baseline.TIMING_METRICS:
        metric = report["metrics"][name]
        assert len(metric["samples"]) == 3
        assert metric["authority"] == "advisory"
    assert report["metrics"]["frames_presented"]["value"] == 3
    assert report["metrics"]["disk_read_bytes"]["value"] == (27 + 9) * baseline.WORD_BYTES
    assert report["metrics"]["disk_write_operations"]["value"] == 2


def test_external_evidence_is_retained_without_overriding_functional_trace(tmp_path: Path) -> None:
    evidence = tmp_path / "doom.json"
    evidence.write_text(json.dumps({
        "schema": "trit.benchmark_result.v1",
        "timing": {"bootstrap_seconds": 0.25},
        "graphics": {"frames_presented": 7},
        "scheduler": {"context_switches": 12, "available": True},
        "disk": {"reads": 2, "writes": 1, "read_words": 810},
    }), encoding="utf-8")
    manifest = tmp_path / "runtime-manifest.json"
    manifest.write_text(json.dumps({
        "format_version": 1,
        "sections": [
            {"kind": "app", "name": "calculator", "word_count": 7894},
            {"kind": "app", "name": "desktop", "word_count": 8851},
        ],
    }), encoding="utf-8")
    report = baseline.collect_baseline(
        warmups=0, iterations=1, frames=7, evidence=[evidence, manifest],
    )
    item = report["external_evidence"][0]
    assert item["schema"] == "trit.benchmark_result.v1"
    assert item["counters"]["boot_time_ms"] == 250.0
    assert item["counters"]["context_switches"] == 12
    # External data is evidence, not a replacement for the fixed trace count.
    assert report["metrics"]["context_switches"]["value"] == 9
    manifest_item = report["external_evidence"][1]
    assert manifest_item["counters"]["bundled_app_count"] == 2
    assert manifest_item["counters"]["calculator_words"] == 7894


def test_cli_collect_and_validate(tmp_path: Path) -> None:
    output = tmp_path / "cli-baseline.json"
    command = [
        sys.executable,
        str(ROOT / "benchmarks" / "performance_baseline.py"),
        "collect",
        "--warmups",
        "0",
        "--iterations",
        "1",
        "--output",
        str(output),
    ]
    completed = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)
    assert completed.returncode == 0, completed.stderr or completed.stdout
    validated = subprocess.run([
        sys.executable,
        str(ROOT / "benchmarks" / "performance_baseline.py"),
        "validate",
        str(output),
        "--reference",
        str(ROOT / "benchmarks" / "reference" / "performance-baseline.v1.json"),
    ], cwd=ROOT, text=True, capture_output=True, check=False)
    # Default CLI parameters match the checked-in 7-frame functional reference.
    assert validated.returncode == 0, validated.stderr or validated.stdout


if __name__ == "__main__":
    test_schema_declares_required_surfaces_and_advisory_timing()
    test_reference_is_functional_only_and_matches_schema_contract()
    print("performance baseline contract: PASS")
