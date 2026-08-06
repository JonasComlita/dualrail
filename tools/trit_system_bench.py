#!/usr/bin/env python3
"""Run and validate the deterministic system workload/release gate.

The native workload targets own the guest execution and metric collection. This
host-side command supplies source metadata, enforces BENCHMARK_SCHEMA's system
profile, and validates the staged release image plus headless smoke diagnostics.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / "BENCHMARK_SCHEMA.json"
WARMUPS = 2
ITERATIONS = 7
MAX_CV = 0.03
MAX_SAMPLE_SECONDS = 60.0
PROBE_TIMEOUT_SECONDS = 180
WORKLOAD_TIMEOUT_SECONDS = 900
WORKLOADS = {
    "doom": ("benchmark_doom_os", "doom-os.json", "doom-class-os"),
    "bitnet": ("benchmark_bitnet_os", "bitnet-os.json", "bitnet-class-os"),
}


def run_command(command: list[str], *, cwd: Path = ROOT, timeout: int = 900,
                env: dict[str, str] | None = None) -> dict[str, Any]:
    try:
        completed = subprocess.run(
            command, cwd=cwd, env=env, text=True, capture_output=True,
            timeout=timeout,
        )
        return {
            "command": command,
            "returncode": completed.returncode,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
        }
    except FileNotFoundError as exc:
        return {"command": command, "returncode": 127, "stdout": "", "stderr": str(exc)}
    except subprocess.TimeoutExpired as exc:
        return {
            "command": command,
            "returncode": 124,
            "stdout": exc.stdout or "",
            "stderr": exc.stderr or "command timed out",
        }


def build_dir(value: str | None) -> Path:
    if value:
        return Path(value).resolve()
    env_value = os.environ.get("TRIT_BUILD_DIR")
    return Path(env_value).resolve() if env_value else ROOT / "build"


def executable(build: Path, target: str) -> Path | None:
    candidates = [build / target, build / f"{target}.exe", build / "Release" / f"{target}.exe"]
    return next((path for path in candidates if path.exists()), None)


def git_metadata() -> tuple[str, bool]:
    commit = run_command(["git", "rev-parse", "HEAD"], timeout=20)
    status = run_command(["git", "status", "--porcelain"], timeout=20)
    return commit["stdout"].strip() if commit["returncode"] == 0 else "unknown", bool(status["stdout"].strip())


def load_json(path: Path) -> tuple[dict[str, Any] | None, str | None]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return None, str(exc)
    return value if isinstance(value, dict) else None, None


def validate_report(path: Path, expected_name: str) -> dict[str, Any]:
    report, error = load_json(path)
    issues: list[str] = []
    if error:
        return {"path": str(path), "ok": False, "issues": [error]}
    assert report is not None
    if report.get("schema") != "trit.benchmark_result.v1":
        issues.append("schema must be trit.benchmark_result.v1")
    for key in ("captured_at_utc", "source", "host", "build", "workload",
                "correctness", "timing", "instruction_mix", "memory", "tlb",
                "scheduler", "wal", "disk", "graphics", "compute"):
        if key not in report:
            issues.append(f"missing top-level metric: {key}")
    workload = report.get("workload", {})
    if workload.get("suite") != "system_benchmarks":
        issues.append("workload.suite must be system_benchmarks")
    if workload.get("name") != expected_name:
        issues.append(f"workload.name must be {expected_name}")
    correctness = report.get("correctness", {})
    if correctness.get("passed") is not True:
        issues.append("correctness.passed is false")
    if not correctness.get("expected_hash") or correctness.get("expected_hash") != correctness.get("observed_hash"):
        issues.append("correctness hashes do not match")
    if correctness.get("warmup_returncodes") != [0] * WARMUPS:
        issues.append("warmup return codes are not two zeroes")
    if correctness.get("measured_returncodes") != [0] * ITERATIONS:
        issues.append("measured return codes are not seven zeroes")
    timing = report.get("timing", {})
    if timing.get("unit") != "seconds":
        issues.append("timing.unit must be seconds")
    if timing.get("warmups") != WARMUPS or timing.get("iterations") != ITERATIONS:
        issues.append("timing must record two warmups and seven iterations")
    samples = timing.get("samples")
    if not isinstance(samples, list) or len(samples) != ITERATIONS or not all(isinstance(item, (int, float)) and item >= 0 for item in samples):
        issues.append("timing.samples must contain seven non-negative numbers")
    cv = timing.get("coefficient_of_variation")
    if not isinstance(cv, (int, float)) or cv >= MAX_CV or timing.get("stable") is not True:
        issues.append("timing is unstable or exceeds the 3% CV gate")
    if isinstance(samples, list) and any(
            isinstance(item, (int, float)) and item > MAX_SAMPLE_SECONDS
            for item in samples):
        issues.append(
            f"timing.samples exceed the {MAX_SAMPLE_SECONDS:.0f}-second per-sample budget")
    for section in ("instruction_mix", "memory", "tlb", "scheduler", "wal", "disk", "graphics", "compute"):
        if not isinstance(report.get(section), dict) or not report[section]:
            issues.append(f"{section} metrics must be a non-empty object")
    if expected_name == "doom-class-os":
        required_workload = ("profile", "frames", "asset_words", "asset_shards", "input_events")
        for key in required_workload:
            if key not in workload:
                issues.append(f"doom workload is missing {key}")
        graphics = report.get("graphics", {})
        scheduler = report.get("scheduler", {})
        correctness = report.get("correctness", {})
        if graphics.get("frames_presented") != workload.get("frames"):
            issues.append("doom graphics.frames_presented must match workload.frames")
        if graphics.get("draw_calls") != workload.get("frames"):
            issues.append("doom graphics.draw_calls must match workload.frames")
        if graphics.get("present_calls") != workload.get("frames"):
            issues.append("doom graphics.present_calls must match workload.frames")
        if scheduler.get("timer_ticks") != workload.get("frames"):
            issues.append("doom scheduler.timer_ticks must match workload.frames")
        if scheduler.get("input_events") != workload.get("input_events"):
            issues.append("doom scheduler.input_events must match workload.input_events")
        if correctness.get("asset_words") != workload.get("asset_words"):
            issues.append("doom correctness.asset_words must match workload.asset_words")
        if not isinstance(correctness.get("io_operations"), int) or correctness.get("io_operations", 0) < 2:
            issues.append("doom correctness.io_operations must record write and read operations")
    elif expected_name == "bitnet-class-os":
        required_workload = ("profile", "model_words", "model_shards", "shard_words", "tokens", "compute_repetitions")
        for key in required_workload:
            if key not in workload:
                issues.append(f"bitnet workload is missing {key}")
        correctness = report.get("correctness", {})
        memory = report.get("memory", {})
        disk = report.get("disk", {})
        instruction_mix = report.get("instruction_mix", {})
        compute = report.get("compute", {})
        if workload.get("model_words") != workload.get("model_shards", 0) * workload.get("shard_words", 0):
            issues.append("bitnet workload.model_words must equal model_shards * shard_words")
        if correctness.get("model_shards_read") != workload.get("model_shards"):
            issues.append("bitnet correctness.model_shards_read must match model_shards")
        if disk.get("read_words") != workload.get("model_words"):
            issues.append("bitnet disk.read_words must match model_words")
        if disk.get("read_shards") != workload.get("model_shards"):
            issues.append("bitnet disk.read_shards must match model_shards")
        if not isinstance(memory.get("pressure_pages"), int) or memory.get("pressure_pages", 0) < 1:
            issues.append("bitnet memory.pressure_pages must be positive")
        if not isinstance(instruction_mix.get("vector_kernel_invocations"), int) or instruction_mix.get("vector_kernel_invocations", 0) < 1:
            issues.append("bitnet instruction_mix.vector_kernel_invocations must be positive")
        if compute.get("sustained_tokens", 0) != workload.get("tokens", 0) * workload.get("compute_repetitions", 0):
            issues.append("bitnet compute.sustained_tokens must match tokens * compute_repetitions")
    return {"path": str(path), "ok": not issues, "issues": issues,
            "workload": workload, "timing": timing, "correctness": correctness}


def validate_probe_report(path: Path, expected_name: str) -> dict[str, Any]:
    """Validate the single-pass bounded probe without applying the 2+7 gate."""
    report, error = load_json(path)
    issues: list[str] = []
    if error:
        return {"path": str(path), "ok": False, "issues": [error]}
    assert report is not None
    if report.get("schema") != "trit.benchmark_result.v1":
        issues.append("schema must be trit.benchmark_result.v1")
    if report.get("probe") is not True:
        issues.append("report.probe must be true for a bounded probe")
    workload = report.get("workload", {})
    if workload.get("suite") != "system_benchmarks":
        issues.append("workload.suite must be system_benchmarks")
    if workload.get("name") != expected_name:
        issues.append(f"workload.name must be {expected_name}")
    if workload.get("profile") != "probe":
        issues.append("workload.profile must be probe")
    correctness = report.get("correctness", {})
    if correctness.get("passed") is not True:
        issues.append("correctness.passed is false")
    if not correctness.get("expected_hash") or correctness.get("expected_hash") != correctness.get("observed_hash"):
        issues.append("correctness hashes do not match")
    if correctness.get("warmup_returncodes") != []:
        issues.append("probe warmup return codes must be an empty array")
    if correctness.get("measured_returncodes") != [0]:
        issues.append("probe measured return codes must contain one zero")
    timing = report.get("timing", {})
    if timing.get("unit") != "seconds":
        issues.append("timing.unit must be seconds")
    if timing.get("warmups") != 0 or timing.get("iterations") != 1:
        issues.append("probe timing must record zero warmups and one iteration")
    samples = timing.get("samples")
    if not isinstance(samples, list) or len(samples) != 1 or not isinstance(samples[0], (int, float)):
        issues.append("probe timing.samples must contain one numeric sample")
    elif samples[0] < 0 or samples[0] > MAX_SAMPLE_SECONDS:
        issues.append(
            f"probe sample must be between 0 and {MAX_SAMPLE_SECONDS:.0f} seconds")
    return {"path": str(path), "ok": not issues, "issues": issues,
            "workload": workload, "timing": timing, "correctness": correctness}


def run_workload(name: str, build: Path, output_dir: Path, no_build: bool) -> dict[str, Any]:
    target, filename, expected_name = WORKLOADS[name]
    output_dir.mkdir(parents=True, exist_ok=True)
    binary = executable(build, target)
    build_result: dict[str, Any] | None = None
    if not no_build:
        build_result = run_command(["cmake", "--build", str(build), "--target", target], timeout=1200)
        if build_result["returncode"] != 0:
            return {"name": name, "ok": False, "build": build_result}
        binary = executable(build, target)
    if binary is None:
        return {"name": name, "ok": False, "issues": [f"missing executable for {target} in {build}"]}

    commit, dirty = git_metadata()
    env = os.environ.copy()
    env.update({
        "TRIT_BENCH_COMMIT": commit,
        "TRIT_BENCH_DIRTY": "true" if dirty else "false",
        "TRIT_BUILD_DIR": str(build),
    })
    report_path = output_dir / filename
    probe_path = output_dir / f"{Path(filename).stem}.probe.json"
    probe_path.unlink(missing_ok=True)
    report_path.unlink(missing_ok=True)
    probe_env = dict(env)
    probe_env["TRIT_BENCH_PROBE"] = "1"
    probe_result = run_command(
        [str(binary), str(probe_path)], timeout=PROBE_TIMEOUT_SECONDS, env=probe_env)
    probe_validation = validate_probe_report(probe_path, expected_name) if probe_path.exists() else {
        "path": str(probe_path), "ok": False, "issues": ["probe report was not written"]
    }
    probe_attempt = {
        "returncode": probe_result["returncode"],
        "stdout": probe_result["stdout"],
        "stderr": probe_result["stderr"],
        "validation": probe_validation,
    }
    if probe_result["returncode"] != 0 or not probe_validation["ok"]:
        return {
            "name": name,
            "ok": False,
            "binary": str(binary),
            "probe": probe_attempt,
            "issues": [
                "bounded probe failed; full 2-warmup/7-sample gate was not started",
                *probe_validation.get("issues", []),
            ],
        }
    env.pop("TRIT_BENCH_PROBE", None)
    attempts: list[dict[str, Any]] = []
    for attempt in range(1, 4):
        result = run_command([str(binary), str(report_path)],
                             timeout=WORKLOAD_TIMEOUT_SECONDS, env=env)
        validation = validate_report(report_path, expected_name) if report_path.exists() else {
            "path": str(report_path), "ok": False, "issues": ["report was not written"]
        }
        attempt_result = {
            "attempt": attempt,
            "returncode": result["returncode"],
            "stdout": result["stdout"],
            "stderr": result["stderr"],
            "validation": validation,
        }
        attempts.append(attempt_result)
        if result["returncode"] == 0 and validation["ok"]:
            return {"name": name, "ok": True, "binary": str(binary),
                    "probe": probe_attempt, "attempts": attempts,
                    "report": validation}
        retryable = any(("unstable" in issue.lower() or "timing" in issue.lower()) and
                        "budget" not in issue.lower()
                        for issue in validation.get("issues", []))
        if not retryable:
            break
    return {"name": name, "ok": False, "binary": str(binary),
            "probe": probe_attempt, "attempts": attempts,
            "report": attempts[-1]["validation"]}


def validate_release(build: Path, smoke_frames: int) -> dict[str, Any]:
    release = build / "release" / "TernaryOS"
    boot = release / "ternary-os.tboot"
    disk = release / "ternary-os.tdisk"
    runner = release / "TernaryOS.exe"
    issues: list[str] = []
    for path in (boot, disk, runner):
        if not path.exists():
            issues.append(f"missing release artifact: {path}")
    image_report: dict[str, Any] | None = None
    smoke: dict[str, Any] | None = None
    diagnostics: dict[str, Any] | None = None
    if boot.exists():
        inspected = run_command([sys.executable, str(ROOT / "tools" / "trit_tool.py"),
                                 "inspect-image", str(boot), "--json"], timeout=120)
        try:
            image_report = json.loads(inspected["stdout"])
        except json.JSONDecodeError:
            image_report = {"ok": False, "raw": inspected["stdout"], "stderr": inspected["stderr"]}
        if inspected["returncode"] != 0 or not image_report.get("ok"):
            issues.append("release tboot inspection failed")
        if image_report.get("format_version") != 3:
            issues.append("release tboot is not format version 3")
        if not image_report.get("apps"):
            issues.append("release tboot contains no bundled app entries")
    if runner.exists() and boot.exists() and disk.exists():
        diagnostic_dir = build / "release-diagnostics"
        smoke = run_command([
            str(runner), "--smoke-test", "--frames", str(smoke_frames),
            "--export-diagnostics", str(diagnostic_dir), str(boot), str(disk),
        ], cwd=release, timeout=600)
        if smoke["returncode"] != 0:
            issues.append("release host smoke test failed")
        required = ("vm_state.txt", "guest.log", "manifest.json", "process_table.json")
        for name in required:
            if not (diagnostic_dir / name).exists():
                issues.append(f"release diagnostics missing {name}")
        export = run_command([
            sys.executable, str(ROOT / "tools" / "trit_tool.py"), "export-diagnostics",
            "--build-dir", str(build), "--boot-image", str(boot), "--disk-image", str(disk),
            "--output", str(build / "diagnostics" / "release-gate"), "--no-smoke-run",
        ], timeout=180)
        diagnostics = {"command": export, "path": str(build / "diagnostics" / "release-gate")}
        if export["returncode"] != 0:
            issues.append("diagnostics export failed")
    return {"ok": not issues, "release_dir": str(release), "issues": issues,
            "image": image_report, "smoke": smoke, "diagnostics": diagnostics}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default=None)
    parser.add_argument("--reports-dir", default=None)
    parser.add_argument("--workload", choices=["doom", "bitnet", "all"], default="all")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--release", action="store_true", help="also validate staged release image and smoke diagnostics")
    parser.add_argument("--smoke-frames", type=int, default=10)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    build = build_dir(args.build_dir)
    reports_dir = Path(args.reports_dir).resolve() if args.reports_dir else build / "benchmarks"
    names = ["doom", "bitnet"] if args.workload == "all" else [args.workload]
    result: dict[str, Any] = {
        "schema": "trit.system_benchmark_gate.v1",
        "created_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "build_dir": str(build),
        "reports_dir": str(reports_dir),
        "workloads": {},
    }
    for name in names:
        result["workloads"][name] = run_workload(name, build, reports_dir, args.no_build)
    if args.release:
        result["release"] = validate_release(build, max(1, args.smoke_frames))
    result["ok"] = all(item.get("ok") for item in result["workloads"].values()) and result.get("release", {}).get("ok", True)
    gate_path = reports_dir / "system-benchmark-gate.json"
    gate_path.parent.mkdir(parents=True, exist_ok=True)
    gate_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print(f"system benchmark gate: {'PASS' if result['ok'] else 'FAIL'}")
        print(f"evidence: {gate_path}")
        for name, item in result["workloads"].items():
            print(f"[{ 'ok' if item.get('ok') else 'fail' }] {name}")
        if args.release:
            print(f"[{ 'ok' if result['release'].get('ok') else 'fail' }] release")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
