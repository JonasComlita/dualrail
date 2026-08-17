#!/usr/bin/env python3
"""Collect the controlled-host NativeX64Jit acceptance evidence.

The benchmark owns the architectural and timing gate.  This collector owns the
environmental contract around it: a Release x86-64 build, executable hash,
three independent child processes pinned to one verified logical CPU, and a
versioned JSON artifact containing every measured sample.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import platform
import re
import statistics
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA = "trit.native_x64_jit_acceptance.v1"
SCHEMA_VERSION = 1
REPEATS = 7
PROCESS_COUNT = 3
WORKLOADS = ("arithmetic", "guarded_memory", "branch_exit", "dynamic_control")
BACKENDS = ("interpreter", "decoded", "native")
SAMPLE_FIELDS = (
    "micros",
    "status",
    "final_pc",
    "steps",
    "decode_count",
    "trace_instructions",
    "cycles",
    "direct_instructions",
    "helper_instructions",
    "portable_side_exits",
    "blocks_built",
    "result",
    "fingerprint",
)
SAMPLE_RE = re.compile(r"^native_x64_sample(?:\s+|$)")
KEY_RE = re.compile(r"(?P<key>[A-Za-z_][A-Za-z0-9_]*)=(?P<value>[^\s]+)")


class CollectionError(RuntimeError):
    """A controlled collection prerequisite or acceptance check failed."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_capture(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )


def command_text(command: list[str]) -> str:
    return subprocess.list2cmdline(command)


def git_value(root: Path, args: list[str], fallback: str = "unknown") -> str:
    result = run_capture(["git", *args], root)
    if result.returncode != 0:
        return fallback
    value = result.stdout.strip()
    return value or fallback


def cmake_cache_values(build_dir: Path) -> dict[str, str]:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        raise CollectionError(f"missing CMake cache: {cache}")
    values: dict[str, str] = {}
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("//") or line.startswith("#"):
            continue
        if ":" not in line or "=" not in line:
            continue
        key_type, value = line.split("=", 1)
        key = key_type.split(":", 1)[0]
        values[key] = value
    return values


def binary_architecture(path: Path) -> str:
    data = path.read_bytes()[:4096]
    if data[:2] == b"MZ" and len(data) >= 0x40:
        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if pe_offset + 6 <= len(data) and data[pe_offset : pe_offset + 4] == b"PE\0\0":
            machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
            return {0x8664: "x86_64", 0x014C: "x86"}.get(
                machine, f"pe_machine_0x{machine:04x}"
            )
    if data[:4] == b"\x7fELF" and len(data) >= 20:
        machine = struct.unpack_from("<H", data, 18)[0]
        return {62: "x86_64", 3: "x86"}.get(
            machine, f"elf_machine_{machine}"
        )
    return "unknown"


def compiler_metadata(cache: dict[str, str], root: Path) -> dict[str, Any]:
    compiler = cache.get("CMAKE_CXX_COMPILER", "unknown")
    compiler_path = Path(compiler)
    version = "unknown"
    if compiler_path.is_file():
        result = run_capture([str(compiler_path), "--version"], root)
        if result.stdout.strip():
            version = result.stdout.splitlines()[0].strip()
    return {
        "path": compiler,
        "version": version,
        "generator": cache.get("CMAKE_GENERATOR", "unknown"),
    }


def power_profile_declaration(explicit: str | None) -> dict[str, str]:
    if explicit:
        return {"declaration": explicit, "source": "argument"}
    environment = os.environ.get("TRIT_POWER_PROFILE_DECLARATION")
    if environment:
        return {"declaration": environment, "source": "environment"}
    if sys.platform == "win32":
        result = subprocess.run(
            ["powercfg", "/getactivescheme"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
        if result.returncode == 0 and result.stdout.strip():
            return {
                "declaration": result.stdout.strip(),
                "source": "powercfg /getactivescheme",
            }
    return {"declaration": "unprovided", "source": "collector fallback"}


def windows_process_affinity(pid: int, requested_cpu: int | None) -> dict[str, Any]:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    handle_type = ctypes.c_void_p
    dword_type = ctypes.c_uint32
    high_priority_class = 0x00000080
    kernel32.OpenProcess.argtypes = [dword_type, ctypes.c_bool, dword_type]
    kernel32.OpenProcess.restype = handle_type
    kernel32.GetProcessAffinityMask.argtypes = [
        handle_type,
        ctypes.POINTER(ctypes.c_ulonglong),
        ctypes.POINTER(ctypes.c_ulonglong),
    ]
    kernel32.GetProcessAffinityMask.restype = ctypes.c_bool
    kernel32.SetProcessAffinityMask.argtypes = [handle_type, ctypes.c_ulonglong]
    kernel32.SetProcessAffinityMask.restype = ctypes.c_bool
    kernel32.SetPriorityClass.argtypes = [handle_type, dword_type]
    kernel32.SetPriorityClass.restype = ctypes.c_bool
    kernel32.GetPriorityClass.argtypes = [handle_type]
    kernel32.GetPriorityClass.restype = dword_type
    kernel32.CloseHandle.argtypes = [handle_type]
    kernel32.CloseHandle.restype = ctypes.c_bool
    handle = kernel32.OpenProcess(0x0200 | 0x1000, False, pid)
    if not handle:
        raise CollectionError(
            f"OpenProcess failed for {pid}: {ctypes.get_last_error()}"
        )
    try:
        if not kernel32.SetPriorityClass(handle, high_priority_class):
            raise CollectionError(
                f"SetPriorityClass(HIGH) failed for {pid}: {ctypes.get_last_error()}"
            )
        verified_priority = kernel32.GetPriorityClass(handle)
        if verified_priority != high_priority_class:
            raise CollectionError(
                f"priority verification failed for {pid}: 0x{verified_priority:x}"
            )
        mask_type = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong
        process_mask = mask_type()
        system_mask = mask_type()
        if not kernel32.GetProcessAffinityMask(
            handle, ctypes.byref(process_mask), ctypes.byref(system_mask)
        ):
            raise CollectionError(
                f"GetProcessAffinityMask failed for {pid}: {ctypes.get_last_error()}"
            )
        available = int(system_mask.value)
        if available == 0:
            raise CollectionError("Windows reported no available affinity mask")
        if requested_cpu is None:
            cpu = (available & -available).bit_length() - 1
        else:
            cpu = requested_cpu
        if cpu < 0 or cpu >= 64 or not (available & (1 << cpu)):
            raise CollectionError(
                f"logical CPU {cpu} is not available in mask 0x{available:x}"
            )
        requested_mask = mask_type(1 << cpu)
        if not kernel32.SetProcessAffinityMask(handle, requested_mask):
            raise CollectionError(
                f"SetProcessAffinityMask failed for {pid}: {ctypes.get_last_error()}"
            )
        verified_mask = mask_type()
        verified_system_mask = mask_type()
        if not kernel32.GetProcessAffinityMask(
            handle, ctypes.byref(verified_mask), ctypes.byref(verified_system_mask)
        ):
            raise CollectionError(
                f"affinity verification failed for {pid}: {ctypes.get_last_error()}"
            )
        if int(verified_mask.value) != 1 << cpu:
            raise CollectionError(
                f"child {pid} affinity is 0x{int(verified_mask.value):x}, "
                f"expected CPU {cpu}"
            )
        return {
            "logical_cpu": cpu,
            "process_mask": f"0x{int(verified_mask.value):x}",
            "system_mask": f"0x{int(verified_system_mask.value):x}",
            "priority_class": "HIGH_PRIORITY_CLASS",
            "priority_verified": True,
            "verified": True,
        }
    finally:
        kernel32.CloseHandle(handle)


def posix_process_affinity(pid: int, requested_cpu: int | None) -> dict[str, Any]:
    if not hasattr(os, "sched_getaffinity") or not hasattr(os, "sched_setaffinity"):
        raise CollectionError("this platform has no process affinity API")
    available = set(os.sched_getaffinity(pid))
    if not available:
        raise CollectionError("the child has no available affinity CPUs")
    cpu = min(available) if requested_cpu is None else requested_cpu
    if cpu not in available:
        raise CollectionError(f"logical CPU {cpu} is not available: {sorted(available)}")
    os.sched_setaffinity(pid, {cpu})
    verified = set(os.sched_getaffinity(pid))
    if verified != {cpu}:
        raise CollectionError(f"child {pid} affinity verification returned {verified}")
    return {
        "logical_cpu": cpu,
        "available_cpus": sorted(available),
        "verified_cpus": sorted(verified),
        "verified": True,
    }


def pin_process(pid: int, requested_cpu: int | None) -> dict[str, Any]:
    if os.name == "nt":
        return windows_process_affinity(pid, requested_cpu)
    return posix_process_affinity(pid, requested_cpu)


def parse_sample_lines(stdout: str) -> list[dict[str, Any]]:
    samples: list[dict[str, Any]] = []
    for line in stdout.splitlines():
        if not SAMPLE_RE.match(line):
            continue
        values = {match.group("key"): match.group("value") for match in KEY_RE.finditer(line)}
        if values.get("version") != "1":
            raise CollectionError(f"unsupported benchmark sample version: {line}")
        missing = {"workload", "backend", "sample", *SAMPLE_FIELDS} - values.keys()
        if missing:
            raise CollectionError(f"sample is missing {sorted(missing)}: {line}")
        record: dict[str, Any] = {
            "version": int(values["version"]),
            "workload": values["workload"],
            "backend": values["backend"],
            "sample": int(values["sample"]),
        }
        for field in SAMPLE_FIELDS:
            record[field] = int(values[field])
        samples.append(record)
    return samples


def coefficient_of_variation(values: list[float]) -> float:
    if not values:
        raise CollectionError("cannot calculate CV for an empty sample set")
    mean = statistics.fmean(values)
    return 0.0 if mean == 0 else statistics.pstdev(values) / mean


def summarize(samples: list[dict[str, Any]]) -> dict[str, Any]:
    if len(samples) != REPEATS:
        raise CollectionError(
            f"expected {REPEATS} measured samples, received {len(samples)}"
        )
    ordered = sorted(samples, key=lambda item: item["sample"])
    if [item["sample"] for item in ordered] != list(range(REPEATS)):
        raise CollectionError("sample indexes are not exactly 0..6")
    fingerprints = [str(item["fingerprint"]) for item in ordered]
    return {
        "sample_count": len(ordered),
        "micros": [item["micros"] for item in ordered],
        "median_us": statistics.median(item["micros"] for item in ordered),
        "cv": coefficient_of_variation([item["micros"] for item in ordered]),
        "fingerprints": fingerprints,
        "repeatable": len(set(fingerprints)) == 1,
        "status_values": sorted({item["status"] for item in ordered}),
        "final_pc_values": sorted({item["final_pc"] for item in ordered}),
        "result_values": sorted({item["result"] for item in ordered}),
        "steps_values": sorted({item["steps"] for item in ordered}),
        "cycles_values": sorted({item["cycles"] for item in ordered}),
        "direct_instructions": [item["direct_instructions"] for item in ordered],
        "helper_instructions": [item["helper_instructions"] for item in ordered],
        "portable_side_exits": [item["portable_side_exits"] for item in ordered],
        "blocks_built": [item["blocks_built"] for item in ordered],
    }


def grouped_summaries(samples: list[dict[str, Any]]) -> dict[str, dict[str, dict[str, Any]]]:
    groups: dict[str, dict[str, dict[str, Any]]] = {}
    for workload in WORKLOADS:
        groups[workload] = {}
        for backend in BACKENDS:
            selected = [
                item
                for item in samples
                if item["workload"] == workload and item["backend"] == backend
            ]
            groups[workload][backend] = summarize(selected)
    return groups


def process_acceptance(summary: dict[str, dict[str, dict[str, Any]]]) -> dict[str, Any]:
    native_speedups: dict[str, float] = {}
    for workload in WORKLOADS:
        interpreter = summary[workload]["interpreter"]["median_us"]
        native = summary[workload]["native"]["median_us"]
        native_speedups[workload] = (
            float(interpreter) if native == 0 else float(interpreter) / float(native)
        )
    existing = WORKLOADS[:3]
    wins = sum(native_speedups[name] >= 1.15 for name in existing)
    third_ok = all(native_speedups[name] >= 0.97 for name in existing)
    stable = all(
        summary[workload][backend]["cv"] < 0.03
        for workload in WORKLOADS
        for backend in BACKENDS
    )
    repeatable = all(
        summary[workload][backend]["repeatable"]
        for workload in WORKLOADS
        for backend in BACKENDS
    )
    halted = all(
        summary[workload][backend]["status_values"] == [1]
        for workload in WORKLOADS
        for backend in BACKENDS
    )
    architectural_parity = all(
        len({
            tuple(summary[workload][backend]["result_values"])
            for backend in BACKENDS
        }) == 1
        and len({
            tuple(summary[workload][backend]["steps_values"])
            for backend in BACKENDS
        }) == 1
        and len({
            tuple(summary[workload][backend]["cycles_values"])
            for backend in BACKENDS
        }) == 1
        for workload in WORKLOADS
    )
    native_dynamic = summary["dynamic_control"]["native"]
    dynamic_speedup = native_speedups["dynamic_control"]
    dynamic = (
        dynamic_speedup >= 1.0 / 1.03
        and native_dynamic["repeatable"]
        and native_dynamic["cv"] < 0.03
        and all(value > 0 for value in native_dynamic["direct_instructions"])
        and all(value == 0 for value in native_dynamic["helper_instructions"])
        and all(value == 0 for value in native_dynamic["portable_side_exits"])
    )
    return {
        "native_speedups": native_speedups,
        "wins_at_least_1_15x": wins,
        "third_workloads_at_least_0_97x": third_ok,
        "all_cv_under_3_percent": stable,
        "all_fingerprints_repeatable": repeatable,
        "all_samples_halted": halted,
        "interpreter_decoded_native_architectural_parity": architectural_parity,
        "dynamic_control_speedup": dynamic_speedup,
        "dynamic_control_max_slowdown_3_percent": dynamic_speedup >= 1.0 / 1.03,
        "dynamic_control_zero_helper_lowerings": all(
            value == 0 for value in native_dynamic["helper_instructions"]
        ),
        "dynamic_control_zero_portable_side_exits": all(
            value == 0 for value in native_dynamic["portable_side_exits"]
        ),
        "dynamic_control": dynamic,
        "primary_speed_gate": (
            wins >= 2
            and third_ok
            and stable
            and repeatable
            and halted
            and architectural_parity
        ),
    }


def cross_process_acceptance(
    process_reports: list[dict[str, Any]],
) -> dict[str, Any]:
    medians: dict[str, list[float]] = {}
    for workload in WORKLOADS:
        medians[workload] = [
            report["summary"][workload]["native"]["median_us"]
            for report in process_reports
        ]
    dynamic_fingerprints = [
        fingerprint
        for report in process_reports
        for fingerprint in report["summary"]["dynamic_control"]["native"]["fingerprints"]
    ]
    return {
        "native_medians_us": medians,
        "native_median_cv": {
            workload: coefficient_of_variation(values)
            for workload, values in medians.items()
        },
        "all_native_median_cv_under_3_percent": all(
            coefficient_of_variation(values) < 0.03
            for values in medians.values()
        ),
        "dynamic_control_fingerprint_count": len(dynamic_fingerprints),
        "dynamic_control_all_21_fingerprints_match": (
            len(dynamic_fingerprints) == PROCESS_COUNT * REPEATS
            and len(set(dynamic_fingerprints)) == 1
        ),
        "dynamic_control_fingerprints": dynamic_fingerprints,
    }


def host_metadata(
    root: Path,
    executable: Path,
    compiler: dict[str, Any],
    power_profile: dict[str, str],
) -> dict[str, Any]:
    stable = {
        "machine": platform.machine(),
        "processor": platform.processor(),
        "system": platform.system(),
        "release": platform.release(),
        "version": platform.version(),
        "architecture": binary_architecture(executable),
        "compiler": compiler,
        "executable_sha256": sha256_file(executable),
        "power_profile": power_profile["declaration"],
    }
    fingerprint = hashlib.sha256(
        json.dumps(stable, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    return {
        "machine": platform.machine(),
        "processor": platform.processor() or os.environ.get("PROCESSOR_IDENTIFIER", "unknown"),
        "system": platform.system(),
        "release": platform.release(),
        "version": platform.version(),
        "logical_cpu_count": os.cpu_count(),
        "architecture": stable["architecture"],
        "fingerprint": fingerprint,
        "power_profile": power_profile,
        "cwd": str(root),
    }


def collect_process(
    root: Path,
    executable: Path,
    attempt_dir: Path,
    process_index: int,
    requested_cpu: int | None,
) -> dict[str, Any]:
    env = os.environ.copy()
    env["TRIT_BENCH_CONTROLLED_HOST"] = "1"
    env["TRIT_BENCH_PROCESS_INDEX"] = str(process_index)
    command = [str(executable)]
    started = utc_now()
    process = subprocess.Popen(
        command,
        cwd=root,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    affinity: dict[str, Any]
    try:
        affinity = pin_process(process.pid, requested_cpu)
        stdout, stderr = process.communicate(timeout=900)
    except Exception:
        process.kill()
        stdout, stderr = process.communicate()
        raise
    stdout_path = attempt_dir / f"process-{process_index}.stdout.log"
    stderr_path = attempt_dir / f"process-{process_index}.stderr.log"
    stdout_path.write_text(stdout, encoding="utf-8")
    stderr_path.write_text(stderr, encoding="utf-8")
    samples = parse_sample_lines(stdout)
    summary = grouped_summaries(samples)
    return {
        "index": process_index,
        "pid": process.pid,
        "started_at": started,
        "finished_at": utc_now(),
        "command": command,
        "returncode": process.returncode,
        "affinity": affinity,
        "stdout_log": str(stdout_path),
        "stderr_log": str(stderr_path),
        "samples": samples,
        "summary": summary,
        "benchmark_acceptance": process_acceptance(summary),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build-dir", type=Path, default=None)
    parser.add_argument("--executable", type=Path, default=None)
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--attempt-dir", type=Path, default=None)
    parser.add_argument("--cpu", type=int, default=None)
    parser.add_argument("--power-profile-declaration", default=None)
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="reuse an already-validated Release build; acceptance still records this choice",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.repo_root.resolve()
    build_dir = (args.build_dir or root / "build").resolve()
    executable = (args.executable or build_dir / "test_execution_backends_benchmark.exe").resolve()
    output = (args.output or build_dir / "native-x64-jit-acceptance" / "native_x64_jit_acceptance.v1.json").resolve()
    attempt_dir = (args.attempt_dir or build_dir / "native-x64-jit-acceptance" / "attempts" / datetime.now().strftime("%Y%m%dT%H%M%S%f")).resolve()
    attempt_dir.mkdir(parents=True, exist_ok=True)

    try:
        machine = platform.machine().lower()
        if machine not in {"amd64", "x86_64", "x64"}:
            raise CollectionError(f"NativeX64Jit acceptance requires x86-64, found {machine}")
        if not executable.is_file():
            raise CollectionError(f"missing benchmark executable: {executable}")

        cache = cmake_cache_values(build_dir)
        build_type = cache.get("CMAKE_BUILD_TYPE", "")
        release_flags = cache.get("CMAKE_CXX_FLAGS_RELEASE", "")
        if build_type.lower() != "release":
            raise CollectionError(f"optimized Release build required, found {build_type!r}")
        if not release_flags:
            raise CollectionError("Release compiler flags are not declared in CMakeCache.txt")
        if not args.skip_build:
            build_result = run_capture(
                ["cmake", "--build", str(build_dir), "--target", "test_execution_backends_benchmark", "--config", "Release"],
                root,
            )
            (attempt_dir / "build.stdout.log").write_text(build_result.stdout, encoding="utf-8")
            (attempt_dir / "build.stderr.log").write_text(build_result.stderr, encoding="utf-8")
            if build_result.returncode != 0:
                raise CollectionError(
                    f"Release benchmark build failed with {build_result.returncode}"
                )
        architecture = binary_architecture(executable)
        if architecture != "x86_64":
            raise CollectionError(f"x86-64 benchmark executable required, found {architecture}")

        # A controlled report is a release artifact, not evidence from an
        # uncommitted working tree.  Keep this check separate from the build
        # hash so a locally rebuilt binary cannot be mistaken for a clean
        # source revision.
        worktree = run_capture(["git", "status", "--porcelain"], root)
        if worktree.returncode != 0:
            raise CollectionError("unable to verify worktree cleanliness")
        if worktree.stdout.strip():
            raise CollectionError(
                "controlled-host acceptance requires a clean worktree; "
                "retain this attempt under diagnostics and commit source changes first"
            )

        compiler = compiler_metadata(cache, root)
        power_profile = power_profile_declaration(args.power_profile_declaration)
        host = host_metadata(root, executable, compiler, power_profile)
        process_reports: list[dict[str, Any]] = []
        for process_index in range(1, PROCESS_COUNT + 1):
            process_reports.append(
                collect_process(
                    root, executable, attempt_dir, process_index, args.cpu
                )
            )

        cross = cross_process_acceptance(process_reports)
        per_process_passed = all(
            report["returncode"] == 0
            and report["benchmark_acceptance"]["primary_speed_gate"]
            and report["benchmark_acceptance"]["dynamic_control"]
            for report in process_reports
        )
        build_validation = {
            "build_dir": str(build_dir),
            "build_type": build_type,
            "release_flags": release_flags,
            "compiler": compiler,
            "optimized_release": build_type.lower() == "release" and bool(release_flags),
            "build_command_run": not args.skip_build,
            "executable": str(executable),
            "executable_architecture": architecture,
            "executable_sha256": sha256_file(executable),
        }
        acceptance = {
            "three_processes_passed": len(process_reports) == PROCESS_COUNT and per_process_passed,
            "all_21_dynamic_control_fingerprints_match": cross["dynamic_control_all_21_fingerprints_match"],
            "cross_process_native_median_cv_under_3_percent": cross["all_native_median_cv_under_3_percent"],
            "build_is_optimized_x86_64": build_validation["optimized_release"] and architecture == "x86_64",
        }
        acceptance["accepted"] = all(acceptance.values())
        report = {
            "schema": SCHEMA,
            "version": SCHEMA_VERSION,
            "accepted": acceptance["accepted"],
            "collected_at": utc_now(),
            "commit": git_value(root, ["rev-parse", "HEAD"]),
            "worktree_dirty": False,
            "build": build_validation,
            "host": host,
            "contract": {
                "process_count": PROCESS_COUNT,
                "warmups_per_process": 2,
                "samples_per_backend_workload": REPEATS,
                "per_sample_cv_limit": 0.03,
                "speedup_threshold": 1.15,
                "third_workload_min_speedup": 0.97,
                "dynamic_control_max_slowdown": 0.03,
                "dynamic_control_required_fingerprint_count": PROCESS_COUNT * REPEATS,
                "affinity": "one verified logical CPU per child",
            },
            "acceptance": acceptance,
            "cross_process": cross,
            "processes": process_reports,
            "attempt_dir": str(attempt_dir),
        }
        report["failure_reasons"] = [
            name for name, passed in acceptance.items() if not passed
        ]
        report["process_failures"] = [
            {
                "index": item["index"],
                "returncode": item["returncode"],
                "primary_speed_gate": item["benchmark_acceptance"][
                    "primary_speed_gate"
                ],
                "dynamic_control": item["benchmark_acceptance"][
                    "dynamic_control"
                ],
            }
            for item in process_reports
            if not (
                item["returncode"] == 0
                and item["benchmark_acceptance"]["primary_speed_gate"]
                and item["benchmark_acceptance"]["dynamic_control"]
            )
        ]
        # Failed attempts remain ignored diagnostics, but still carry the
        # complete versioned sample report for review and repair.  Only the
        # accepted report is promoted to the public output path below.
        attempt_report = attempt_dir / f"{SCHEMA}.json"
        attempt_report.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        if not report["accepted"]:
            raise CollectionError(
                "controlled-host acceptance failed; retained logs under "
                f"{attempt_dir}"
            )
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_suffix(output.suffix + ".tmp")
        temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(temporary, output)
        print(json.dumps({"accepted": True, "report": str(output)}, sort_keys=True))
        return 0
    except (CollectionError, OSError, subprocess.SubprocessError) as error:
        (attempt_dir / "collector-error.txt").write_text(str(error) + "\n", encoding="utf-8")
        print(f"native-x64-jit acceptance blocked: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
