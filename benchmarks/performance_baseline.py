#!/usr/bin/env python3
"""Collect and validate reproducible OS performance baseline evidence.

The existing Doom and BitNet targets provide useful guest workload counters,
but their wall-clock samples are host dependent.  This harness adds a small,
dependency-free baseline contract for boot, app launch, frame, disk, and
context-switch surfaces.  Functional counters are generated from a fixed
trace and are the reproducibility authority.  ``perf_counter_ns`` timings are
recorded as advisory observations and never decide pass/fail.

The collector is intentionally usable without SDL or a release image.  When
existing benchmark reports or runtime diagnostics are supplied with
``--evidence``, their counters are retained under ``external_evidence`` so a
controlled release run can be compared with the synthetic trace without
silently mixing the two sources.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
import platform
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parents[1]
SCHEMA_PATH = ROOT / "benchmarks" / "performance_baseline_schema.json"
REFERENCE_PATH = ROOT / "benchmarks" / "reference" / "performance-baseline.v1.json"
DEFAULT_OUTPUT = ROOT / "build" / "benchmarks" / "performance-baseline.json"

SCHEMA_ID = "trit.performance_baseline.v1"
PROTOCOL_ID = "p10.performance-baseline.v1"
DEFAULT_WARMUPS = 2
DEFAULT_ITERATIONS = 7
DEFAULT_FRAMES = 7
DEFAULT_SEED = 0x13579BDF
DEFAULT_DISK_WORDS = 729
DEFAULT_APP_WORDS = 81
WORD_BYTES = 8

FUNCTIONAL_METRICS = (
    "boot_events",
    "app_launch_events",
    "frames_presented",
    "disk_read_bytes",
    "disk_write_bytes",
    "disk_read_operations",
    "disk_write_operations",
    "context_switches",
)
TIMING_METRICS = (
    "boot_time_ms",
    "app_launch_time_ms",
    "frame_time_ms",
    "disk_io_time_ms",
)
REQUIRED_METRICS = FUNCTIONAL_METRICS + TIMING_METRICS


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def env_bool(name: str, default: bool = False) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def _git_metadata() -> dict[str, Any]:
    def run(*args: str) -> str:
        try:
            result = subprocess.run(
                ["git", *args], cwd=ROOT, text=True, capture_output=True,
                timeout=5, check=False,
            )
        except (OSError, subprocess.SubprocessError):
            return ""
        return result.stdout.strip() if result.returncode == 0 else ""

    commit = run("rev-parse", "HEAD") or os.environ.get("TRIT_BENCH_COMMIT", "unknown")
    status = run("status", "--porcelain")
    dirty = bool(status) or env_bool("TRIT_BENCH_DIRTY")
    return {
        "repository": "TernaryStack",
        "commit": commit,
        "dirty": dirty,
        "generator": "performance-baseline-v1",
    }


def _affinity() -> list[int] | None:
    get_affinity = getattr(os, "sched_getaffinity", None)
    if get_affinity is None:
        return None
    try:
        return sorted(int(value) for value in get_affinity(0))
    except OSError:
        return None


def host_metadata() -> dict[str, Any]:
    """Return host identity and controls without pretending it is deterministic."""

    affinity = _affinity()
    metadata: dict[str, Any] = {
        "system": platform.system() or "unknown",
        "release": platform.release() or "unknown",
        "version": platform.version() or "unknown",
        "machine": platform.machine() or "unknown",
        "processor": platform.processor() or "unknown",
        "python": platform.python_version(),
        "implementation": platform.python_implementation(),
        "cpu_count": os.cpu_count() or 1,
        "affinity": affinity if affinity is not None else "unavailable",
        "controlled": env_bool("TRIT_BENCH_CONTROLLED_HOST"),
        "control_profile": os.environ.get("TRIT_BENCH_CONTROL_PROFILE", "uncontrolled"),
        "timing_clock": "perf_counter_ns",
        "timing_authority": "advisory",
    }
    fingerprint_input = dict(metadata)
    metadata["fingerprint"] = "sha256:" + sha256_text(canonical_json(fingerprint_input))
    return metadata


def _payload(seed: int, words: int) -> bytes:
    # A fixed byte stream models packed T40 words while remaining portable.
    size = words * WORD_BYTES
    state = seed & 0xFFFFFFFF
    output = bytearray(size)
    for index in range(size):
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        output[index] = (state >> 24) & 0xFF
    return bytes(output)


def _functional_metrics(frames: int, disk_words: int, app_words: int) -> dict[str, dict[str, Any]]:
    """Build the deterministic metric side of the baseline contract."""

    # These counts describe the trace, not host implementation details.  They
    # deliberately correspond to visible OS surfaces: reset/kernel/VFS/
    # scheduler/desktop boot, executable lookup/read/verify/handoff, and one
    # scheduler handoff per frame.
    values = {
        "boot_events": 6,
        "app_launch_events": 5,
        "frames_presented": frames,
        "disk_read_bytes": (disk_words + app_words) * WORD_BYTES,
        "disk_write_bytes": disk_words * WORD_BYTES,
        "disk_read_operations": 2,
        "disk_write_operations": 2,
        "context_switches": 2 + frames,
    }
    units = {
        "boot_events": "events",
        "app_launch_events": "events",
        "frames_presented": "frames",
        "disk_read_bytes": "bytes",
        "disk_write_bytes": "bytes",
        "disk_read_operations": "operations",
        "disk_write_operations": "operations",
        "context_switches": "switches",
    }
    return {
        name: {
            "kind": "counter",
            "unit": units[name],
            "value": int(values[name]),
            "deterministic": True,
            "authority": "functional",
            "source": "fixed_trace",
        }
        for name in FUNCTIONAL_METRICS
    }


def _summary(samples: Iterable[float]) -> dict[str, Any]:
    values = [float(value) for value in samples]
    if not values:
        return {
            "count": 0,
            "median": None,
            "mean": None,
            "p95": None,
            "minimum": None,
            "maximum": None,
            "standard_deviation": None,
            "coefficient_of_variation": None,
        }
    ordered = sorted(values)
    mean = statistics.fmean(ordered)
    variance = statistics.pvariance(ordered) if len(ordered) > 1 else 0.0
    # Nearest-rank p95 avoids interpolating an unobserved wall-clock value.
    p95_index = max(0, math.ceil(0.95 * len(ordered)) - 1)
    return {
        "count": len(ordered),
        "median": statistics.median(ordered),
        "mean": mean,
        "p95": ordered[p95_index],
        "minimum": ordered[0],
        "maximum": ordered[-1],
        "standard_deviation": math.sqrt(variance),
        "coefficient_of_variation": (math.sqrt(variance) / mean) if mean else 0.0,
    }


def _timing_metric(samples: list[float], source: str = "host_monotonic") -> dict[str, Any]:
    return {
        "kind": "timing",
        "unit": "milliseconds",
        "samples": [round(float(value), 6) for value in samples],
        "summary": _summary(samples),
        "deterministic": False,
        "authority": "advisory",
        "source": source,
    }


def _context_switch_sample() -> tuple[int | None, str]:
    """Read process context switches where the host exposes them.

    POSIX ``resource`` reports voluntary and involuntary switches.  Windows
    does not expose an equivalent counter in the Python standard library, so
    the report records an explicit unavailable sample instead of fabricating a
    host count.  The deterministic ``context_switches`` trace metric remains
    available on every host.
    """

    try:
        import resource  # type: ignore

        usage = resource.getrusage(resource.RUSAGE_SELF)
        return int(getattr(usage, "ru_nvcsw", 0) + getattr(usage, "ru_nivcsw", 0)), "posix_getrusage"
    except (ImportError, AttributeError, OSError):
        return None, "unavailable"


def _run_once(
    *,
    seed: int,
    frames: int,
    disk_words: int,
    app_words: int,
    io_path: Path,
) -> tuple[dict[str, float], dict[str, Any], int | None, str]:
    """Run one fixed trace and return timings, digest inputs, and host switches."""

    # Boot is modelled as a fixed sequence of transitions.  The loops prevent
    # a zero-duration sample on fast hosts without introducing sleep jitter.
    boot_start = time.perf_counter_ns()
    boot_state = seed & 0xFFFFFFFFFFFFFFFF
    for event in range(6):
        boot_state = (boot_state ^ (event + 1) * 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        boot_state = ((boot_state << 7) | (boot_state >> 57)) & 0xFFFFFFFFFFFFFFFF
    boot_ms = (time.perf_counter_ns() - boot_start) / 1_000_000.0

    payload = _payload(seed, disk_words)
    app_payload = _payload(seed ^ 0xA5A5A5A5, app_words)

    app_start = time.perf_counter_ns()
    app_descriptor = {"path": "/bin/calculator", "words": app_words, "abi": "v2"}
    app_digest = hashlib.sha256(canonical_json(app_descriptor).encode("utf-8")).digest()
    # Keep executable descriptor and payload checks in the measured app handoff
    # path.  This is the second fixed disk operation; the data workload is
    # measured separately below.
    app_io_path = io_path.with_name("calculator.bin")
    app_io_path.write_bytes(app_payload)
    app_roundtrip = app_io_path.read_bytes()
    if len(app_digest) != 32 or app_roundtrip != app_payload:
        raise RuntimeError("app launch fixture unexpectedly empty")
    app_ms = (time.perf_counter_ns() - app_start) / 1_000_000.0

    io_start = time.perf_counter_ns()
    io_path.write_bytes(payload)
    roundtrip = io_path.read_bytes()
    if roundtrip != payload:
        raise RuntimeError("disk round-trip did not preserve fixed payload")
    io_ms = (time.perf_counter_ns() - io_start) / 1_000_000.0

    frame_samples: list[float] = []
    frame_hash = hashlib.sha256()
    for frame in range(frames):
        frame_start = time.perf_counter_ns()
        # Deterministic framebuffer work: every frame consumes the same fixed
        # input trace and emits an independently hashed frame marker.
        marker = (seed + frame * 0x45D9F3B) & 0xFFFFFFFFFFFFFFFF
        frame_hash.update(marker.to_bytes(8, "little"))
        frame_hash.update(app_digest[:8])
        for lane in range(27):
            marker = (marker ^ (lane + 1) * 0x27D4EB2D) & 0xFFFFFFFFFFFFFFFF
        frame_hash.update(marker.to_bytes(8, "little"))
        frame_samples.append((time.perf_counter_ns() - frame_start) / 1_000_000.0)

    switches, switch_source = _context_switch_sample()
    timings = {
        "boot_time_ms": boot_ms,
        "app_launch_time_ms": app_ms,
        "frame_time_ms": statistics.fmean(frame_samples) if frame_samples else 0.0,
        "disk_io_time_ms": io_ms,
    }
    digest = {
        "boot_state": boot_state,
        "app_sha256": app_digest.hex(),
        "app_payload_sha256": hashlib.sha256(app_payload).hexdigest(),
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "frame_sha256": frame_hash.hexdigest(),
        "frames": frames,
        "disk_words": disk_words,
        "app_words": app_words,
    }
    return timings, digest, switches, switch_source


def _load_evidence(paths: Iterable[Path]) -> list[dict[str, Any]]:
    evidence: list[dict[str, Any]] = []
    for path in paths:
        item: dict[str, Any] = {"path": str(path), "exists": path.exists()}
        if not path.exists():
            item["error"] = "missing"
            evidence.append(item)
            continue
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            item["error"] = str(exc)
            evidence.append(item)
            continue
        item["schema"] = data.get("schema") if isinstance(data, dict) else None
        if isinstance(data, dict):
            counters: dict[str, Any] = {}
            sections = data.get("sections")
            if isinstance(sections, list):
                apps = [item for item in sections if isinstance(item, dict) and item.get("kind") == "app"]
                counters["bundled_app_count"] = len(apps)
                calculator = next((item for item in apps if item.get("name") == "calculator"), None)
                if isinstance(calculator, dict) and isinstance(calculator.get("word_count"), int):
                    counters["calculator_words"] = calculator["word_count"]
            slots = data.get("slots")
            if isinstance(slots, list):
                counters["active_processes"] = sum(
                    1 for item in slots
                    if isinstance(item, dict) and isinstance(item.get("pid"), int) and item["pid"] > 0
                )
            if data.get("schema") == "trit.runtime_checkpoint.v1":
                counters["checkpoint_available"] = bool(data.get("available"))
            timing = data.get("timing")
            if isinstance(timing, dict) and isinstance(timing.get("bootstrap_seconds"), (int, float)):
                counters["boot_time_ms"] = float(timing["bootstrap_seconds"]) * 1000.0
            graphics = data.get("graphics")
            if isinstance(graphics, dict):
                for name in ("frames_presented", "draw_calls", "present_calls"):
                    if isinstance(graphics.get(name), int):
                        counters[name] = graphics[name]
            scheduler = data.get("scheduler")
            if isinstance(scheduler, dict):
                if isinstance(scheduler.get("context_switches"), int):
                    counters["context_switches"] = scheduler["context_switches"]
                if scheduler.get("available") is not False:
                    counters["scheduler"] = {
                        key: scheduler[key] for key in ("timer_ticks", "input_events")
                        if key in scheduler
                    }
            disk = data.get("disk")
            if isinstance(disk, dict):
                for name in ("reads", "writes", "read_words", "read_bytes", "write_bytes"):
                    if isinstance(disk.get(name), (int, float)):
                        counters[name] = disk[name]
            item["counters"] = counters
        evidence.append(item)
    return evidence


def collect_baseline(
    *,
    output: Path | None = None,
    warmups: int = DEFAULT_WARMUPS,
    iterations: int = DEFAULT_ITERATIONS,
    frames: int = DEFAULT_FRAMES,
    seed: int = DEFAULT_SEED,
    disk_words: int = DEFAULT_DISK_WORDS,
    app_words: int = DEFAULT_APP_WORDS,
    evidence: Iterable[Path] = (),
) -> dict[str, Any]:
    """Collect one baseline report without a wall-clock acceptance gate."""

    if warmups < 0:
        raise ValueError("warmups must be non-negative")
    if iterations < 1:
        raise ValueError("iterations must be positive")
    if frames < 1 or disk_words < 1 or app_words < 1:
        raise ValueError("frames, disk_words, and app_words must be positive")

    with tempfile.TemporaryDirectory(prefix="trit-performance-baseline-") as temp_dir:
        io_path = Path(temp_dir) / "fixture.tdisk"
        for _ in range(warmups):
            _run_once(seed=seed, frames=frames, disk_words=disk_words,
                      app_words=app_words, io_path=io_path)
        runs = [
            _run_once(seed=seed, frames=frames, disk_words=disk_words,
                      app_words=app_words, io_path=io_path)
            for _ in range(iterations)
        ]

    timing_samples = {
        name: [float(run[0][name]) for run in runs]
        for name in TIMING_METRICS
    }
    host_switch_samples = [run[2] for run in runs if run[2] is not None]
    switch_sources = sorted({run[3] for run in runs})
    digests = [run[1] for run in runs]
    expected_metrics = _functional_metrics(frames, disk_words, app_words)
    expected_digest = sha256_text(canonical_json({"functional": expected_metrics, "trace": digests[0]}))
    observed_digests = [sha256_text(canonical_json({"functional": expected_metrics, "trace": digest})) for digest in digests]
    functional_match = all(digest == digests[0] for digest in digests)

    metrics: dict[str, dict[str, Any]] = dict(expected_metrics)
    metrics.update({name: _timing_metric(samples) for name, samples in timing_samples.items()})
    metrics["host_context_switches"] = {
        "kind": "host_counter",
        "unit": "switches",
        "samples": host_switch_samples,
        "summary": _summary(host_switch_samples),
        "available": bool(host_switch_samples),
        "deterministic": False,
        "authority": "advisory",
        "source": switch_sources[0] if len(switch_sources) == 1 else switch_sources,
    }

    report: dict[str, Any] = {
        "schema": SCHEMA_ID,
        "protocol_id": PROTOCOL_ID,
        "captured_at_utc": utc_now(),
        "source": _git_metadata(),
        "host": host_metadata(),
        "workload": {
            "name": "synthetic-os-performance-baseline",
            "version": "v1",
            "seed": seed,
            "frames": frames,
            "disk_words": disk_words,
            "app_words": app_words,
            "app_path": "/bin/calculator",
        },
        "protocol": {
            "warmups": warmups,
            "iterations": iterations,
            "functional_counts_authoritative": True,
            "wall_clock_authority": "advisory",
            "timing_threshold_enforced": False,
        },
        "metrics": metrics,
        "reproducibility": {
            "input_hash": "sha256:" + sha256_text(canonical_json({
                "seed": seed, "frames": frames, "disk_words": disk_words,
                "app_words": app_words,
            })),
            "expected_hash": "sha256:" + expected_digest,
            "observed_hashes": ["sha256:" + value for value in observed_digests],
            "observed_hash": "sha256:" + observed_digests[0],
            "functional_match": functional_match,
            "trace_repeatable": len(set(observed_digests)) == 1,
        },
        "external_evidence": _load_evidence(evidence),
        "policy": {
            "functional_metrics": list(FUNCTIONAL_METRICS),
            "advisory_metrics": list(TIMING_METRICS) + ["host_context_switches"],
            "no_wall_clock_threshold": True,
        },
    }
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report


def reference_baseline(
    *,
    output: Path | None = None,
    frames: int = DEFAULT_FRAMES,
    seed: int = DEFAULT_SEED,
    disk_words: int = DEFAULT_DISK_WORDS,
    app_words: int = DEFAULT_APP_WORDS,
) -> dict[str, Any]:
    """Create a host-neutral reference containing functional counts only.

    Reference artifacts intentionally use zero-valued timing samples.  They
    satisfy the report shape while making it impossible to mistake a fixture
    captured on the maintainer's machine for an authoritative wall-clock
    baseline.  ``reference`` validation compares only functional metrics.
    """

    report = collect_baseline(
        warmups=0, iterations=1, frames=frames, seed=seed,
        disk_words=disk_words, app_words=app_words,
    )
    report["captured_at_utc"] = "1970-01-01T00:00:00Z"
    report["source"] = {
        "repository": "TernaryStack",
        "commit": "reference",
        "dirty": False,
        "generator": "performance-baseline-reference-v1",
    }
    report["host"] = {
        "system": "reference",
        "release": "reference",
        "machine": "reference",
        "processor": "reference",
        "python": "reference",
        "implementation": "reference",
        "cpu_count": 1,
        "affinity": "reference",
        "controlled": True,
        "control_profile": "reference",
        "timing_clock": "none",
        "timing_authority": "advisory",
        "fingerprint": "sha256:" + "0" * 64,
    }
    report["protocol"] = {
        "warmups": 0,
        "iterations": 1,
        "functional_counts_authoritative": True,
        "wall_clock_authority": "advisory",
        "timing_threshold_enforced": False,
    }
    for name in TIMING_METRICS:
        report["metrics"][name] = _timing_metric([0.0], source="reference_unmeasured")
    report["metrics"]["host_context_switches"] = {
        "kind": "host_counter",
        "unit": "switches",
        "samples": [],
        "summary": _summary([]),
        "available": False,
        "deterministic": False,
        "authority": "advisory",
        "source": "reference_unmeasured",
    }
    report["external_evidence"] = []
    report["policy"]["reference"] = "functional_counts_only"
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report


def _metric_error(message: str, **extra: Any) -> dict[str, Any]:
    return {"message": message, **extra}


def validate_report(report: Any, *, reference: dict[str, Any] | None = None) -> dict[str, Any]:
    """Validate semantic invariants; deliberately do not reject timing CV."""

    errors: list[dict[str, Any]] = []
    if not isinstance(report, dict):
        return {"schema": "trit.performance_baseline_validation.v1", "ok": False,
                "errors": [_metric_error("report must be an object")]}
    if report.get("schema") != SCHEMA_ID:
        errors.append(_metric_error("schema must be trit.performance_baseline.v1"))
    if report.get("protocol_id") != PROTOCOL_ID:
        errors.append(_metric_error("protocol_id must be p10.performance-baseline.v1"))
    for key in ("captured_at_utc", "source", "host", "workload", "protocol", "metrics", "reproducibility", "policy"):
        if key not in report:
            errors.append(_metric_error(f"missing top-level field: {key}"))
    metrics = report.get("metrics")
    if not isinstance(metrics, dict):
        metrics = {}
        errors.append(_metric_error("metrics must be an object"))
    for name in FUNCTIONAL_METRICS:
        metric = metrics.get(name)
        if not isinstance(metric, dict):
            errors.append(_metric_error(f"missing functional metric: {name}"))
            continue
        if metric.get("kind") != "counter" or metric.get("authority") != "functional" or metric.get("deterministic") is not True:
            errors.append(_metric_error(f"functional metric {name} must be deterministic and authoritative"))
        if not isinstance(metric.get("value"), int) or metric["value"] < 0:
            errors.append(_metric_error(f"functional metric {name}.value must be a non-negative integer"))
    for name in TIMING_METRICS:
        metric = metrics.get(name)
        if not isinstance(metric, dict):
            errors.append(_metric_error(f"missing timing metric: {name}"))
            continue
        if metric.get("kind") != "timing" or metric.get("authority") != "advisory" or metric.get("deterministic") is not False:
            errors.append(_metric_error(f"timing metric {name} must be advisory"))
        samples = metric.get("samples")
        if not isinstance(samples, list) or not samples or any(not isinstance(value, (int, float)) or value < 0 for value in samples):
            errors.append(_metric_error(f"timing metric {name}.samples must contain non-negative numbers"))
        summary = metric.get("summary")
        summary_count_matches = isinstance(samples, list) and isinstance(summary, dict) and summary.get("count") == len(samples)
        if not summary_count_matches:
            errors.append(_metric_error(f"timing metric {name}.summary count must match samples"))
    host_switches = metrics.get("host_context_switches")
    if isinstance(host_switches, dict):
        samples = host_switches.get("samples")
        if not isinstance(samples, list) or any(not isinstance(value, int) or value < 0 for value in samples):
            errors.append(_metric_error("host_context_switches.samples must be non-negative integers"))
        if host_switches.get("authority") != "advisory" or host_switches.get("deterministic") is not False:
            errors.append(_metric_error("host_context_switches must be advisory"))
    reproducibility = report.get("reproducibility")
    if not isinstance(reproducibility, dict) or reproducibility.get("functional_match") is not True or reproducibility.get("trace_repeatable") is not True:
        errors.append(_metric_error("functional trace must be repeatable"))
    protocol = report.get("protocol")
    if isinstance(protocol, dict) and protocol.get("timing_threshold_enforced") is not False:
        errors.append(_metric_error("timing_threshold_enforced must remain false"))
    if reference is not None:
        ref_metrics = reference.get("metrics", {}) if isinstance(reference, dict) else {}
        for name in FUNCTIONAL_METRICS:
            expected = ref_metrics.get(name, {}).get("value") if isinstance(ref_metrics.get(name), dict) else None
            actual = metrics.get(name, {}).get("value") if isinstance(metrics.get(name), dict) else None
            if expected is not None and actual != expected:
                errors.append(_metric_error(f"functional metric {name} differs from reference", expected=expected, actual=actual))
    return {
        "schema": "trit.performance_baseline_validation.v1",
        "ok": not errors,
        "errors": errors,
        "functional_metrics": {name: metrics.get(name, {}).get("value") for name in FUNCTIONAL_METRICS},
        "timing_metrics": {name: metrics.get(name, {}).get("summary") for name in TIMING_METRICS},
    }


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command")
    collect = sub.add_parser("collect", help="collect a synthetic deterministic trace")
    collect.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    collect.add_argument("--warmups", type=int, default=DEFAULT_WARMUPS)
    collect.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS)
    collect.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    collect.add_argument("--seed", type=lambda value: int(value, 0), default=DEFAULT_SEED)
    collect.add_argument("--disk-words", type=int, default=DEFAULT_DISK_WORDS)
    collect.add_argument("--app-words", type=int, default=DEFAULT_APP_WORDS)
    collect.add_argument("--evidence", type=Path, action="append", default=[])
    reference = sub.add_parser("reference", help="write the host-neutral functional reference")
    reference.add_argument("--output", type=Path, default=REFERENCE_PATH)
    reference.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    reference.add_argument("--seed", type=lambda value: int(value, 0), default=DEFAULT_SEED)
    reference.add_argument("--disk-words", type=int, default=DEFAULT_DISK_WORDS)
    reference.add_argument("--app-words", type=int, default=DEFAULT_APP_WORDS)
    validate = sub.add_parser("validate", help="validate a baseline report")
    validate.add_argument("report", type=Path)
    validate.add_argument("--reference", type=Path, default=None)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    raw = list(argv) if argv is not None else sys.argv[1:]
    if not raw or raw[0] not in {"collect", "reference", "validate"}:
        raw = ["collect", *raw]
    args = parser.parse_args(raw)
    command = args.command
    if command == "collect":
        report = collect_baseline(
            output=args.output, warmups=args.warmups, iterations=args.iterations,
            frames=args.frames, seed=args.seed, disk_words=args.disk_words,
            app_words=args.app_words, evidence=args.evidence,
        )
        result = validate_report(report)
        print(json.dumps({"schema": "trit.performance_baseline_run.v1", "ok": result["ok"],
                          "output": str(args.output), "validation": result}, indent=2, sort_keys=True))
        return 0 if result["ok"] else 1
    if command == "reference":
        report = reference_baseline(
            output=args.output, frames=args.frames, seed=args.seed,
            disk_words=args.disk_words, app_words=args.app_words,
        )
        result = validate_report(report)
        print(json.dumps({"schema": "trit.performance_baseline_reference_run.v1", "ok": result["ok"],
                          "output": str(args.output), "validation": result}, indent=2, sort_keys=True))
        return 0 if result["ok"] else 1
    if command == "validate":
        report = load_json(args.report)
        reference = load_json(args.reference) if args.reference else None
        result = validate_report(report, reference=reference)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if result["ok"] else 1
    parser.error(f"unknown command: {command}")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
