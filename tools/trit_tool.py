#!/usr/bin/env python3
"""Agent-operable host tooling for Trit/Ternary OS.

This script intentionally avoids third-party packages so it can run before the
project itself is fully built. The PowerShell wrappers beside it expose the
stable command names documented in AGENTS.md.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = Path(__file__).resolve().parent
BOOT_MAGIC = 0x31544F4F424F5354
BOOT_LEGACY_FORMAT_VERSION = 1
BOOT_FORMAT_VERSION = 2
MMU_PAGE_WORDS = 27
MANIFEST_FILES = [
    "ROADMAP_STATUS.json",
    "TEST_MANIFEST.json",
    "SYSCALL_MANIFEST.json",
    "IMAGE_FORMAT_MANIFEST.json",
    "APP_MANIFEST.json",
]


def rel(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return str(path)


def load_json_file(path: Path) -> tuple[dict[str, Any] | None, str | None]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            data = json.load(handle)
        if not isinstance(data, dict):
            return None, "top-level JSON value must be an object"
        return data, None
    except FileNotFoundError:
        return None, "file is missing"
    except json.JSONDecodeError as exc:
        return None, f"invalid JSON at line {exc.lineno}: {exc.msg}"


def run_command(
    args: list[str],
    cwd: Path = REPO_ROOT,
    capture: bool = True,
    timeout: int | None = None,
    env: dict[str, str] | None = None,
) -> dict[str, Any]:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            args,
            cwd=str(cwd),
            text=True,
            capture_output=capture,
            timeout=timeout,
            env=env,
        )
        return {
            "command": args,
            "cwd": str(cwd),
            "returncode": completed.returncode,
            "stdout": completed.stdout if capture else "",
            "stderr": completed.stderr if capture else "",
            "duration_seconds": round(time.monotonic() - started, 3),
        }
    except FileNotFoundError as exc:
        return {
            "command": args,
            "cwd": str(cwd),
            "returncode": 127,
            "stdout": "",
            "stderr": str(exc),
            "duration_seconds": round(time.monotonic() - started, 3),
        }
    except subprocess.TimeoutExpired as exc:
        return {
            "command": args,
            "cwd": str(cwd),
            "returncode": 124,
            "stdout": exc.stdout or "",
            "stderr": exc.stderr or "command timed out",
            "duration_seconds": round(time.monotonic() - started, 3),
        }


def default_build_dir(value: str | None = None) -> Path:
    if value:
        return Path(value).resolve()
    env_value = os.environ.get("TRIT_BUILD_DIR")
    if env_value:
        return Path(env_value).resolve()
    preferred = REPO_ROOT / "build"
    if (preferred / "CMakeCache.txt").exists():
        return preferred
    for cache in REPO_ROOT.glob("*/CMakeCache.txt"):
        return cache.parent
    return preferred


def parse_cmake_cache(build_dir: Path) -> dict[str, str]:
    cache = build_dir / "CMakeCache.txt"
    values: dict[str, str] = {}
    if not cache.exists():
        return values
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_type, value = line.split("=", 1)
        key = key_type.split(":", 1)[0]
        values[key] = value
    return values


def executable_names(target: str) -> list[str]:
    if platform.system().lower().startswith("windows"):
        return [target + ".exe", target]
    return [target, target + ".exe"]


def find_executable(build_dir: Path, target: str) -> Path | None:
    for name in executable_names(target):
        direct = build_dir / name
        if direct.exists():
            return direct
    names = set(executable_names(target))
    for item in build_dir.rglob("*"):
        if item.is_file() and item.name in names:
            return item
    return None


def load_test_manifest() -> dict[str, Any]:
    data, error = load_json_file(REPO_ROOT / "TEST_MANIFEST.json")
    if error:
        raise RuntimeError(f"TEST_MANIFEST.json: {error}")
    assert data is not None
    return data


def suites_by_name() -> dict[str, dict[str, Any]]:
    manifest = load_test_manifest()
    suites = manifest.get("suites", [])
    if not isinstance(suites, list):
        raise RuntimeError("TEST_MANIFEST.json suites must be a list")
    out: dict[str, dict[str, Any]] = {}
    for suite in suites:
        if isinstance(suite, dict) and isinstance(suite.get("name"), str):
            out[suite["name"]] = suite
    return out


def unique_targets(suite_names: list[str]) -> list[str]:
    suites = suites_by_name()
    targets: list[str] = []
    for name in suite_names:
        if name not in suites:
            raise RuntimeError(f"unknown suite '{name}'")
        for target in suites[name].get("targets", []):
            if target not in targets:
                targets.append(target)
    return targets


def print_json(data: Any) -> None:
    print(json.dumps(data, indent=2, sort_keys=True))


class BootReader:
    def __init__(self, payload: bytes) -> None:
        self.payload = payload
        self.offset = 0

    def _read(self, fmt: str) -> Any:
        size = struct.calcsize(fmt)
        if self.offset + size > len(self.payload):
            raise ValueError("boot image payload is truncated")
        value = struct.unpack_from(fmt, self.payload, self.offset)[0]
        self.offset += size
        return value

    def u32(self) -> int:
        return int(self._read("<I"))

    def i32(self) -> int:
        return int(self._read("<i"))

    def u64(self) -> int:
        return int(self._read("<Q"))

    def i64(self) -> int:
        return int(self._read("<q"))

    def string(self) -> str:
        size = self.u32()
        if self.offset + size > len(self.payload):
            raise ValueError("boot image string is truncated")
        raw = self.payload[self.offset : self.offset + size]
        self.offset += size
        return raw.decode("utf-8", errors="replace")

    def skip_words(self, count: int, word_size: int) -> None:
        size = count * word_size
        if self.offset + size > len(self.payload):
            raise ValueError("boot image segment is truncated")
        self.offset += size


def fnv1a(data: bytes) -> int:
    value = 1469598103934665603
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def inspect_boot_image(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {
        "path": str(path),
        "ok": False,
        "issues": [],
        "format": "tboot",
    }
    try:
        raw = path.read_bytes()
    except FileNotFoundError:
        result["issues"].append("image file is missing")
        return result

    if len(raw) < 24:
        result["issues"].append("boot image header is truncated")
        return result

    magic, checksum, payload_size = struct.unpack_from("<QQQ", raw, 0)
    payload = raw[24:]
    result["header"] = {
        "magic": magic,
        "checksum": checksum,
        "payload_size": payload_size,
        "file_size": len(raw),
    }
    if magic != BOOT_MAGIC:
        result["issues"].append("boot image magic is invalid")
    if payload_size != len(payload):
        result["issues"].append("boot image payload size does not match file size")
    if fnv1a(payload) != checksum:
        result["issues"].append("boot image checksum validation failed")
    if result["issues"]:
        return result

    try:
        reader = BootReader(payload)
        version = reader.u32()
        boot_entry = reader.i32()
        framebuffer_width = reader.i32()
        framebuffer_height = reader.i32()
        profile_name = reader.string()
        image_version = reader.string()
        sections = []
        if version == BOOT_FORMAT_VERSION:
            section_count = reader.u32()
            for _ in range(section_count):
                sections.append(
                    {
                        "name": reader.string(),
                        "path": reader.string(),
                        "kind": reader.string(),
                        "load_address": reader.i32(),
                        "entry_pc": reader.i32(),
                        "word_count": reader.i32(),
                        "page_count": reader.i32(),
                        "flags": reader.i32(),
                    }
                )
        app_count = reader.u32()
        apps = []
        for _ in range(app_count):
            apps.append(
                {
                    "name": reader.string(),
                    "path": reader.string(),
                    "text_ppn": reader.i32(),
                    "entry_pc": reader.i32(),
                    "text_pages": reader.i32(),
                    "data_pages": reader.i32(),
                    "stack_words": reader.i32(),
                }
            )
        program_words = reader.u32()
        reader.skip_words(program_words, 8)
        data_words = reader.u32()
        reader.skip_words(data_words, 8)
        rootfs_words = reader.u32() if version == BOOT_LEGACY_FORMAT_VERSION else 0
        rootfs_nonzero_words = 0
        rootfs_nonzero_blocks: set[int] = set()
        for index in range(rootfs_words):
            word = reader.i64()
            if word != 0:
                rootfs_nonzero_words += 1
                rootfs_nonzero_blocks.add(index // MMU_PAGE_WORDS)
        if reader.offset != len(payload):
            result["issues"].append("boot image has trailing payload bytes")
        if version not in {BOOT_LEGACY_FORMAT_VERSION, BOOT_FORMAT_VERSION}:
            result["issues"].append(f"unsupported boot image format version {version}")
        if boot_entry < 0 or boot_entry >= program_words:
            result["issues"].append("boot entry is outside text segment")
        if rootfs_words % MMU_PAGE_WORDS != 0:
            result["issues"].append("rootfs seed is not block aligned")
        if version == BOOT_FORMAT_VERSION and not sections:
            result["issues"].append("boot image section table is empty")
        result.update(
            {
                "format_version": version,
                "legacy_format": version == BOOT_LEGACY_FORMAT_VERSION,
                "image": {
                    "version": image_version,
                    "profile": profile_name,
                    "boot_entry": boot_entry,
                    "framebuffer_width": framebuffer_width,
                    "framebuffer_height": framebuffer_height,
                },
                "segments": {
                    "program_words": program_words,
                    "data_words": data_words,
                    "rootfs_words": rootfs_words,
                    "rootfs_blocks": rootfs_words // MMU_PAGE_WORDS,
                    "rootfs_nonzero_words": rootfs_nonzero_words,
                    "rootfs_nonzero_blocks": len(rootfs_nonzero_blocks),
                },
                "sections": sections,
                "apps": apps,
            }
        )
    except ValueError as exc:
        result["issues"].append(str(exc))

    result["ok"] = len(result["issues"]) == 0
    return result


def text_status(label: str, ok: bool, detail: str = "") -> None:
    prefix = "ok" if ok else "warn"
    suffix = f" - {detail}" if detail else ""
    print(f"[{prefix}] {label}{suffix}")


def cmd_doctor(args: argparse.Namespace) -> int:
    build_dir = default_build_dir(args.build_dir)
    report: dict[str, Any] = {
        "repo_root": str(REPO_ROOT),
        "host": {
            "platform": platform.platform(),
            "python": sys.version.split()[0],
        },
        "issues": [],
        "checks": {},
    }

    required = [
        "CMakeLists.txt",
        "kernel.trit",
        "ternary_host_runtime.h",
        "build_tos_image.cpp",
        "run_tos_sdl.cpp",
        "AGENTS.md",
        "DEBUGGING.md",
        "ACCEPTANCE_CRITERIA.md",
        "KNOWN_GAPS.md",
    ] + MANIFEST_FILES
    file_checks = []
    for name in required:
        exists = (REPO_ROOT / name).exists()
        file_checks.append({"path": name, "exists": exists})
        if not exists:
            report["issues"].append({"severity": "error", "message": f"missing {name}"})
    report["checks"]["required_files"] = file_checks

    manifest_checks = []
    for name in MANIFEST_FILES:
        data, error = load_json_file(REPO_ROOT / name)
        manifest_checks.append({"path": name, "ok": error is None, "error": error})
        if error:
            report["issues"].append({"severity": "error", "message": f"{name}: {error}"})
        elif data is not None and data.get("version") is None:
            report["issues"].append({"severity": "warning", "message": f"{name} has no version field"})
    report["checks"]["json_manifests"] = manifest_checks

    cache = parse_cmake_cache(build_dir)
    report["checks"]["build"] = {
        "build_dir": str(build_dir),
        "configured": bool(cache),
        "generator": cache.get("CMAKE_GENERATOR"),
        "build_type": cache.get("CMAKE_BUILD_TYPE"),
        "cxx_compiler": cache.get("CMAKE_CXX_COMPILER"),
        "sdl2_dir": cache.get("SDL2_DIR"),
        "vulkan_glslc": cache.get("Vulkan_GLSLC_EXECUTABLE") or cache.get("GLSLC_EXECUTABLE"),
    }
    if not cache:
        report["issues"].append({"severity": "warning", "message": f"build dir is not configured: {build_dir}"})

    tools = {}
    if not args.no_commands:
        for tool in ["cmake", "ctest", "git"]:
            tools[tool] = shutil.which(tool)
        report["checks"]["host_tools"] = tools
        for tool, path in tools.items():
            if not path:
                report["issues"].append({"severity": "warning", "message": f"{tool} is not on PATH"})

        git_status = run_command(["git", "status", "--short"], capture=True, timeout=10)
        report["checks"]["git_status"] = {
            "returncode": git_status["returncode"],
            "dirty_entries": [line for line in git_status["stdout"].splitlines() if line.strip()],
        }

    try:
        suites = suites_by_name()
        target_names = unique_targets(["smoke", "production"])
        target_checks = []
        for target in target_names:
            binary = find_executable(build_dir, target)
            target_checks.append(
                {
                    "target": target,
                    "binary": str(binary) if binary else None,
                    "built": binary is not None,
                }
            )
        report["checks"]["test_suites"] = sorted(suites.keys())
        report["checks"]["test_targets"] = target_checks
    except RuntimeError as exc:
        report["issues"].append({"severity": "error", "message": str(exc)})

    candidate_images = [
        build_dir / "ternary-os.tboot",
        build_dir / "release" / "TernaryOS" / "ternary-os.tboot",
    ]
    image_checks = []
    for image in candidate_images:
        if image.exists():
            inspected = inspect_boot_image(image)
            image_checks.append(inspected)
            if not inspected["ok"]:
                report["issues"].append({"severity": "warning", "message": f"{rel(image)} image validation failed"})
    report["checks"]["boot_images"] = image_checks

    errors = [issue for issue in report["issues"] if issue["severity"] == "error"]
    warnings = [issue for issue in report["issues"] if issue["severity"] == "warning"]
    report["ok"] = not errors

    if args.json:
        print_json(report)
    else:
        print("Trit doctor")
        print(f"repo: {REPO_ROOT}")
        text_status("required files", not any(not item["exists"] for item in file_checks))
        text_status("JSON manifests", not any(not item["ok"] for item in manifest_checks))
        text_status("CMake build directory", bool(cache), str(build_dir))
        if "test_targets" in report["checks"]:
            built_count = sum(1 for item in report["checks"]["test_targets"] if item["built"])
            text_status("smoke/production binaries", built_count > 0, f"{built_count} found")
        if warnings:
            print("\nWarnings:")
            for issue in warnings:
                print(f"- {issue['message']}")
        if errors:
            print("\nErrors:")
            for issue in errors:
                print(f"- {issue['message']}")
    if errors or (args.strict and warnings):
        return 1
    return 0


def cmd_test(args: argparse.Namespace) -> int:
    suites = suites_by_name()
    if args.list:
        for name, suite in sorted(suites.items()):
            print(f"{name}: {suite.get('description', '')}")
        return 0

    if args.all:
        suite_names = [
            name
            for name, suite in suites.items()
            if not suite.get("manual", False)
        ]
    else:
        suite_names = args.suites or ["smoke"]

    build_dir = default_build_dir(args.build_dir)
    targets = unique_targets(suite_names)
    results: list[dict[str, Any]] = []
    env = os.environ.copy()
    env["PATH"] = str(build_dir) + os.pathsep + env.get("PATH", "")

    for target in targets:
        target_result: dict[str, Any] = {"target": target}
        if not args.no_build:
            build = run_command(["cmake", "--build", str(build_dir), "--target", target], capture=True)
            target_result["build"] = build
            if build["returncode"] != 0:
                results.append(target_result)
                if not args.continue_on_fail:
                    break
                continue

        binary = find_executable(build_dir, target)
        if binary is None:
            target_result["run"] = {
                "returncode": 127,
                "stderr": f"could not find built executable for {target}",
            }
            results.append(target_result)
            if not args.continue_on_fail:
                break
            continue

        run = run_command([str(binary)], cwd=REPO_ROOT, capture=True, timeout=args.timeout, env=env)
        target_result["binary"] = str(binary)
        target_result["run"] = run
        results.append(target_result)
        if run["returncode"] != 0 and not args.continue_on_fail:
            break

    failed = [
        item for item in results
        if item.get("build", {}).get("returncode", 0) != 0 or item.get("run", {}).get("returncode", 0) != 0
    ]
    report = {
        "ok": not failed,
        "suites": suite_names,
        "build_dir": str(build_dir),
        "results": results,
    }
    if args.json:
        print_json(report)
    else:
        print(f"Suites: {', '.join(suite_names)}")
        for item in results:
            build_rc = item.get("build", {}).get("returncode")
            run_rc = item.get("run", {}).get("returncode")
            state = "ok" if (build_rc in (None, 0) and run_rc == 0) else "fail"
            print(f"[{state}] {item['target']} build={build_rc if build_rc is not None else 'skipped'} run={run_rc}")
            if state == "fail":
                stderr = item.get("build", {}).get("stderr") or item.get("run", {}).get("stderr") or ""
                stdout = item.get("run", {}).get("stdout") or ""
                if stderr.strip():
                    print(stderr.strip())
                elif stdout.strip():
                    print(stdout.strip())
    return 0 if not failed else 1


def cmd_inspect_image(args: argparse.Namespace) -> int:
    inspected = inspect_boot_image(Path(args.image).resolve())
    if args.json:
        print_json(inspected)
    else:
        if not inspected["ok"]:
            print(f"image: {inspected['path']}")
            for issue in inspected["issues"]:
                print(f"[warn] {issue}")
            return 1
        image = inspected["image"]
        segments = inspected["segments"]
        print(f"image: {inspected['path']}")
        print(
            f"format: {inspected['format_version']} "
            f"version: {image['version']} profile: {image['profile']}"
        )
        print(f"boot_entry: {image['boot_entry']}")
        print(
            "segments: "
            f"text={segments['program_words']} "
            f"data={segments['data_words']} "
            f"rootfs_blocks={segments['rootfs_blocks']} "
            f"nonzero_blocks={segments['rootfs_nonzero_blocks']}"
        )
        if inspected.get("sections"):
            print("sections:")
            for section in inspected["sections"]:
                print(
                    f"- {section['kind']} {section['name']} {section['path']} "
                    f"entry={section['entry_pc']} words={section['word_count']}"
                )
        print("apps:")
        for app in inspected["apps"]:
            print(f"- {app['path']} entry={app['entry_pc']} text_pages={app['text_pages']}")
    return 0 if inspected["ok"] else 1


def cmd_build_image(args: argparse.Namespace) -> int:
    build_dir = default_build_dir(args.build_dir)
    if not args.no_build:
        build = run_command(["cmake", "--build", str(build_dir), "--target", "build_tos_image"], capture=False)
        if build["returncode"] != 0:
            return int(build["returncode"])
    binary = find_executable(build_dir, "build_tos_image")
    if binary is None:
        print(f"could not find build_tos_image in {build_dir}", file=sys.stderr)
        return 127
    cmd = [str(binary), args.boot_image]
    if args.disk_image:
        cmd.append(args.disk_image)
    if args.image_version:
        if not args.disk_image:
            cmd.append("")
        cmd.append(args.image_version)
    result = run_command(cmd, cwd=REPO_ROOT, capture=False)
    return int(result["returncode"])


def cmd_export_diagnostics(args: argparse.Namespace) -> int:
    build_dir = default_build_dir(args.build_dir)
    out_dir = Path(args.output).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    report: dict[str, Any] = {
        "format_version": 1,
        "repo_root": str(REPO_ROOT),
        "build_dir": str(build_dir),
        "created_by": "tools/trit_tool.py export-diagnostics",
        "artifacts": {},
        "commands": [],
    }

    boot_image = Path(args.boot_image).resolve() if args.boot_image else None
    disk_image = Path(args.disk_image).resolve() if args.disk_image else None
    release_boot = (REPO_ROOT / "build" / "release" / "TernaryOS" / "ternary-os.tboot").resolve()
    release_disk = (REPO_ROOT / "build" / "release" / "TernaryOS" / "ternary-os.tdisk").resolve()
    fallback_boot = (build_dir / "ternary-os.tboot").resolve()
    fallback_disk = (build_dir / "ternary-os.tdisk").resolve()
    if boot_image == release_boot and not boot_image.exists() and fallback_boot.exists():
        boot_image = fallback_boot
    if disk_image == release_disk and not disk_image.exists() and fallback_disk.exists():
        disk_image = fallback_disk
    if boot_image and boot_image.exists():
        inspected = inspect_boot_image(boot_image)
        (out_dir / "image_manifest.json").write_text(
            json.dumps(inspected, indent=2, sort_keys=True),
            encoding="utf-8",
        )
        report["artifacts"]["image_manifest"] = str(out_dir / "image_manifest.json")
        report["image_ok"] = inspected["ok"]
    elif boot_image:
        report["image_ok"] = False
        report["image_issue"] = f"boot image missing: {boot_image}"

    if not args.no_commands:
        git_status = run_command(["git", "status", "--short"], capture=True, timeout=10)
        (out_dir / "git_status.txt").write_text(git_status["stdout"], encoding="utf-8")
        report["commands"].append(git_status)
        report["artifacts"]["git_status"] = str(out_dir / "git_status.txt")

    if args.smoke_run and boot_image and disk_image:
        runner = find_executable(build_dir, "run_tos_sdl")
        if runner is None:
            report["smoke_run"] = {"ok": False, "issue": "run_tos_sdl is not built"}
        else:
            runtime_dir = out_dir / "runtime"
            cmd = [
                str(runner),
                "--smoke-test",
                "--frames",
                str(args.frames),
                "--export-diagnostics",
                str(runtime_dir),
                str(boot_image),
                str(disk_image),
            ]
            smoke = run_command(cmd, cwd=REPO_ROOT, capture=True, timeout=args.timeout)
            report["commands"].append(smoke)
            report["smoke_run"] = {"ok": smoke["returncode"] == 0, "diagnostics_dir": str(runtime_dir)}
            (out_dir / "smoke_stdout.txt").write_text(smoke["stdout"], encoding="utf-8")
            (out_dir / "smoke_stderr.txt").write_text(smoke["stderr"], encoding="utf-8")

    report_path = out_dir / "agent_diagnostics.json"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
    print(f"wrote {report_path}")
    if report.get("smoke_run", {}).get("ok") is False:
        return 1
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    build_dir = default_build_dir(args.build_dir)
    runner = find_executable(build_dir, "run_tos_sdl")
    if runner is None:
        print(f"could not find run_tos_sdl in {build_dir}", file=sys.stderr)
        return 127
    boot_image = Path(args.boot_image).resolve()
    disk_image = Path(args.disk_image).resolve()
    release_boot = (REPO_ROOT / "build" / "release" / "TernaryOS" / "ternary-os.tboot").resolve()
    release_disk = (REPO_ROOT / "build" / "release" / "TernaryOS" / "ternary-os.tdisk").resolve()
    fallback_boot = (build_dir / "ternary-os.tboot").resolve()
    fallback_disk = (build_dir / "ternary-os.tdisk").resolve()
    if boot_image == release_boot and not boot_image.exists() and fallback_boot.exists():
        boot_image = fallback_boot
    if disk_image == release_disk and not disk_image.exists() and fallback_disk.exists():
        disk_image = fallback_disk
    cmd = [str(runner)]
    if args.smoke_test:
        cmd.extend(["--smoke-test", "--frames", str(args.frames)])
    if args.export_diagnostics:
        cmd.extend(["--export-diagnostics", args.export_diagnostics])
    cmd.extend([str(boot_image), str(disk_image)])
    result = run_command(cmd, cwd=REPO_ROOT, capture=False)
    return int(result["returncode"])


def profile_harness_binary(build_dir: Path) -> Path:
    name = "trit_profile_tos.exe" if platform.system().lower().startswith("windows") else "trit_profile_tos"
    return build_dir / name


def build_profile_harness(build_dir: Path) -> int:
    source = TOOLS_DIR / "trit_profile_tos.cpp"
    binary = profile_harness_binary(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)
    deps = [
        source,
        REPO_ROOT / "ternary_host_runtime.h",
        REPO_ROOT / "ternary_vm.h",
        REPO_ROOT / "ternary_vm_state.h",
        REPO_ROOT / "ternary_isa.h",
        REPO_ROOT / "ternary_asm.h",
    ]
    if binary.exists() and all(dep.exists() for dep in deps):
        newest_dep = max(dep.stat().st_mtime for dep in deps)
        if binary.stat().st_mtime >= newest_dep:
            return 0
    cache = parse_cmake_cache(build_dir)
    compiler = cache.get("CMAKE_CXX_COMPILER") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        print("could not find a C++ compiler for profile harness", file=sys.stderr)
        return 127

    compiler_name = Path(compiler).name.lower()
    if compiler_name.startswith("cl"):
        cmd = [
            compiler,
            "/nologo",
            "/std:c++17",
            "/EHsc",
            "/DNOMINMAX",
            "/DTERNARY_IGNORE_LONG_DOUBLE_ASSERT",
            f"/I{REPO_ROOT}",
            str(source),
            f"/Fe:{binary}",
        ]
    else:
        cmd = [
            compiler,
            "-std=c++17",
            "-O2",
            "-DNOMINMAX",
            "-DTERNARY_IGNORE_LONG_DOUBLE_ASSERT",
            "-I",
            str(REPO_ROOT),
            str(source),
            "-o",
            str(binary),
        ]
    result = run_command(cmd, cwd=REPO_ROOT, capture=True)
    if result["returncode"] != 0:
        if result["stdout"].strip():
            print(result["stdout"], file=sys.stderr)
        if result["stderr"].strip():
            print(result["stderr"], file=sys.stderr)
    return int(result["returncode"])


def default_boot_image(build_dir: Path, requested: str | None) -> Path:
    if requested:
        return Path(requested).resolve()
    release_boot = (REPO_ROOT / "build" / "release" / "TernaryOS" / "ternary-os.tboot").resolve()
    fallback_boot = (build_dir / "ternary-os.tboot").resolve()
    return release_boot if release_boot.exists() else fallback_boot


def cmd_profile(args: argparse.Namespace) -> int:
    build_dir = default_build_dir(args.build_dir)
    if not args.no_build:
        rc = build_profile_harness(build_dir)
        if rc != 0:
            return rc

    harness = profile_harness_binary(build_dir)
    if not harness.exists():
        print(f"could not find profile harness in {build_dir}", file=sys.stderr)
        return 127

    boot_image = default_boot_image(build_dir, args.boot_image)
    if args.workload == "os" and not boot_image.exists():
        print(f"boot image is missing: {boot_image}", file=sys.stderr)
        return 2

    cmd = [
        str(harness),
        "--workload",
        args.workload,
        "--steps",
        str(args.steps),
        "--top",
        str(args.top),
        "--format",
        args.format,
    ]
    if args.output:
        cmd.extend(["--output", args.output])
    if args.profile:
        cmd.extend(["--profile", args.profile])
    if args.disk_image:
        cmd.extend(["--disk", str(Path(args.disk_image).resolve())])
    if args.workload == "os":
        cmd.append(str(boot_image))

    result = run_command(cmd, cwd=REPO_ROOT, capture=True, timeout=args.timeout)
    if result["stdout"] and not args.output:
        print(result["stdout"], end="" if result["stdout"].endswith("\n") else "\n")
    if result["stderr"].strip():
        print(result["stderr"], file=sys.stderr)
    if args.output and result["returncode"] == 0:
        print(f"wrote {args.output}")
    return int(result["returncode"])


def cmd_bench(args: argparse.Namespace) -> int:
    args.suites = ["benchmark"]
    args.all = False
    args.list = False
    return cmd_test(args)


def cmd_replay(args: argparse.Namespace) -> int:
    print("deterministic replay traces are not implemented yet; see KNOWN_GAPS.md", file=sys.stderr)
    return 2


def cmd_fuzz(args: argparse.Namespace) -> int:
    print("dedicated fuzz harnesses are not implemented yet; running smoke tests instead")
    args.suites = ["smoke"]
    args.all = False
    args.list = False
    return cmd_test(args)


def add_build_dir(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--build-dir", default=None, help="CMake build directory; defaults to TRIT_BUILD_DIR or ./build")


def add_test_options(parser: argparse.ArgumentParser) -> None:
    add_build_dir(parser)
    parser.add_argument("suites", nargs="*", help="suite name(s) from TEST_MANIFEST.json")
    parser.add_argument("--all", action="store_true", help="run every non-manual suite")
    parser.add_argument("--list", action="store_true", help="list suites and exit")
    parser.add_argument("--no-build", action="store_true", help="do not build targets before running")
    parser.add_argument("--continue-on-fail", action="store_true", help="keep running after the first failure")
    parser.add_argument("--timeout", type=int, default=300, help="per-test timeout in seconds")
    parser.add_argument("--json", action="store_true", help="emit JSON report")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="trit-tool")
    sub = parser.add_subparsers(dest="command", required=True)

    doctor = sub.add_parser("doctor", help="check repo, manifests, build products, and images")
    add_build_dir(doctor)
    doctor.add_argument("--json", action="store_true")
    doctor.add_argument("--strict", action="store_true", help="return nonzero when warnings are present")
    doctor.add_argument("--no-commands", action="store_true", help="skip git/cmake PATH probes")
    doctor.set_defaults(func=cmd_doctor)

    test = sub.add_parser("test", help="build and run suites from TEST_MANIFEST.json")
    add_test_options(test)
    test.set_defaults(func=cmd_test)

    bench = sub.add_parser("bench", help="run benchmark suite")
    add_test_options(bench)
    bench.set_defaults(func=cmd_bench)

    inspect = sub.add_parser("inspect-image", help="validate and summarize a .tboot image")
    inspect.add_argument("image")
    inspect.add_argument("--json", action="store_true")
    inspect.set_defaults(func=cmd_inspect_image)

    build_image = sub.add_parser("build-image", help="build a boot image with build_tos_image")
    add_build_dir(build_image)
    build_image.add_argument("boot_image", nargs="?", default="build/ternary-os.tboot")
    build_image.add_argument("disk_image", nargs="?", default="build/ternary-os.tdisk")
    build_image.add_argument("image_version", nargs="?", default="dev")
    build_image.add_argument("--no-build", action="store_true")
    build_image.set_defaults(func=cmd_build_image)

    export = sub.add_parser("export-diagnostics", help="write agent-readable diagnostics")
    add_build_dir(export)
    export.add_argument("--boot-image", default=str(REPO_ROOT / "build" / "release" / "TernaryOS" / "ternary-os.tboot"))
    export.add_argument("--disk-image", default=str(REPO_ROOT / "build" / "release" / "TernaryOS" / "ternary-os.tdisk"))
    export.add_argument("--output", default=str(REPO_ROOT / "build" / "diagnostics" / "latest"))
    export.add_argument("--frames", type=int, default=10)
    export.add_argument("--timeout", type=int, default=120)
    export.add_argument("--no-smoke-run", dest="smoke_run", action="store_false")
    export.add_argument("--no-commands", action="store_true")
    export.set_defaults(func=cmd_export_diagnostics, smoke_run=True)

    run = sub.add_parser("run", help="launch or smoke-run the SDL host runtime")
    add_build_dir(run)
    run.add_argument("boot_image", nargs="?", default=str(REPO_ROOT / "build" / "release" / "TernaryOS" / "ternary-os.tboot"))
    run.add_argument("disk_image", nargs="?", default=str(REPO_ROOT / "build" / "release" / "TernaryOS" / "ternary-os.tdisk"))
    run.add_argument("--smoke-test", action="store_true")
    run.add_argument("--frames", type=int, default=120)
    run.add_argument("--export-diagnostics", default=None)
    run.set_defaults(func=cmd_run)

    profile = sub.add_parser("profile", help="boot a Ternary OS image and emit a deterministic VM profile")
    add_build_dir(profile)
    profile.add_argument("boot_image", nargs="?", default=None)
    profile.add_argument("--disk-image", default=None, help="optional mutable disk backing; omitted uses the image rootfs in memory")
    profile.add_argument("--steps", type=int, default=200000)
    profile.add_argument("--top", type=int, default=20)
    profile.add_argument("--format", choices=["text", "json"], default="text")
    profile.add_argument("--workload", choices=["os", "syscall-probe"], default="os")
    profile.add_argument("--output", default=None)
    profile.add_argument("--profile", choices=["minimum", "compact"], default=None)
    profile.add_argument("--timeout", type=int, default=120)
    profile.add_argument("--no-build", action="store_true", help="reuse an existing profile harness executable")
    profile.set_defaults(func=cmd_profile)

    replay = sub.add_parser("replay", help="placeholder for future deterministic replay")
    replay.add_argument("trace", nargs="?")
    replay.set_defaults(func=cmd_replay)

    fuzz = sub.add_parser("fuzz", help="run the current fuzz stand-in")
    add_test_options(fuzz)
    fuzz.set_defaults(func=cmd_fuzz)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.func(args))
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
