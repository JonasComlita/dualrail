#!/usr/bin/env python3
"""Agent-operable host tooling for Trit/Ternary OS.

This script intentionally avoids third-party packages so it can run before the
project itself is fully built. The PowerShell wrappers beside it expose the
stable command names documented in AGENTS.md.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime as _dt
import hashlib
import io
import json
import os
import platform
import random
import re
import shlex
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time
from urllib.parse import unquote
from pathlib import Path
from typing import Any
try:
    from treatcode_platform import cmd_website
except ModuleNotFoundError:  # package import used by unittest and other host tools
    from tools.treatcode_platform import cmd_website
try:
    from repository_ingestion import (
        build_index,
        compare_clean_incremental,
        generate_context_package,
        load_index,
        print_report as print_repository_index_report,
        verify_index,
    )
except ModuleNotFoundError:  # package import used by unittest and other host tools
    from tools.repository_ingestion import (
        build_index,
        compare_clean_incremental,
        generate_context_package,
        load_index,
        print_report as print_repository_index_report,
        verify_index,
    )

try:
    from p10_benchmark import (
        DEFAULT_EVIDENCE_PATH,
        MANIFEST_PATH as P10_MANIFEST_PATH,
        REFERENCE_PATH as P10_REFERENCE_PATH,
        verify_reference as verify_p10_reference,
    )
except ModuleNotFoundError:  # package import used by unittest and other host tools
    from tools.p10_benchmark import (
        DEFAULT_EVIDENCE_PATH,
        MANIFEST_PATH as P10_MANIFEST_PATH,
        REFERENCE_PATH as P10_REFERENCE_PATH,
        verify_reference as verify_p10_reference,
    )
try:
    from challenge_manifest import validate_challenge_manifest
except ModuleNotFoundError:  # package import used by unittest and other host tools
    from tools.challenge_manifest import validate_challenge_manifest


REPO_ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = Path(__file__).resolve().parent
DOCS_DIR = REPO_ROOT / "docs"
OBSIDIAN_DIR = DOCS_DIR / ".obsidian"
OBSIDIAN_CANVAS = DOCS_DIR / "trit-stack.canvas"
GRAPHIFY_OUT_DIR = REPO_ROOT / "graphify-out"
GRAPHIFY_ARCHIVE_DIR = DOCS_DIR / "_graphify"
TRIT_AST_DUMP_TARGET = "trit_ast_dump"
BOOT_MAGIC = 0x31544F4F424F5354
BOOT_LEGACY_FORMAT_VERSION = 1
BOOT_TRANSITION_FORMAT_VERSION = 2
BOOT_FORMAT_VERSION = 3
STORAGE_BLOCK_WORDS = 27
SPARSE_DISK_LEGACY_MAGIC = 0x54524954535031
SPARSE_DISK_MAGIC = 0x54524954535032
SPARSE_DISK_VERSION = 2
SPARSE_DISK_LEGACY_HEADER = struct.Struct("<Qi")
SPARSE_DISK_HEADER = struct.Struct("<QIIQQi")
SPARSE_DISK_RECORD_HEADER = struct.Struct("<i")
SPARSE_DISK_WORD = struct.Struct("<Q")
SPARSE_DISK_RECORD_SIZE = (
    SPARSE_DISK_RECORD_HEADER.size +
    STORAGE_BLOCK_WORDS * SPARSE_DISK_WORD.size
)
MANIFEST_FILES = [
    "ARCHITECTURE_MANIFEST.json",
    "COMPILER_CORPUS_SCHEMA.json",
    "BENCHMARK_SCHEMA.json",
    "BENCHMARK_MANIFEST.json",
    "BENCHMARK_PROTOCOL_SCHEMA.json",
    "ROADMAP_STATUS.json",
    "TEST_MANIFEST.json",
    "SYSCALL_MANIFEST.json",
    "IMAGE_FORMAT_MANIFEST.json",
    "APP_MANIFEST.json",
    "STACK_MANIFEST.json",
    "CAPABILITY_MANIFEST.json",
    "CONTRACT_MANIFEST.json",
    "DECISION_MANIFEST.json",
    "STACK_COVERAGE_REPORT.json",
    "CHALLENGE_MANIFEST.json",
    "CHALLENGE_MANIFEST_SCHEMA.json",
    "TREATCODE_PLAN_MANIFEST.json",
    "TREATCODE_PLAN_MANIFEST_SCHEMA.json",
]
REGISTRY_FILES = {
    "stack": "STACK_MANIFEST.json",
    "capability": "CAPABILITY_MANIFEST.json",
    "contract": "CONTRACT_MANIFEST.json",
    "decision": "DECISION_MANIFEST.json",
    "coverage": "STACK_COVERAGE_REPORT.json",
}
PLAN_INDEX_PATH = DOCS_DIR / "11_TreatCode_Platform" / "PLAN_INDEX.md"
PLAN_MANIFEST_PATH = REPO_ROOT / "TREATCODE_PLAN_MANIFEST.json"
PLAN_MANIFEST_SCHEMA_PATH = REPO_ROOT / "TREATCODE_PLAN_MANIFEST_SCHEMA.json"
PLAN_EVIDENCE_DIR = REPO_ROOT / "build" / "treatcode-plan-evidence"
PLAN_STATUSES = {"not_started", "in_progress", "blocked", "complete", "superseded"}
PLAN_GATE_TYPES = {"structural", "correctness", "security", "performance", "human"}
OBSIDIAN_REQUIRED_FILES = [
    "README.md",
    "INDEX.md",
    "STATUS.md",
    "AGENTS.md",
    ".obsidian/app.json",
    ".obsidian/core-plugins.json",
    ".obsidian/community-plugins.json",
    "trit-stack.canvas",
    "_graphify/README.md",
]
OBSIDIAN_APP_CONFIG = {
    "alwaysUpdateLinks": True,
    "attachmentFolderPath": "_attachments",
    "newFileLocation": "current",
    "promptDelete": False,
    "showInlineTitle": True,
}
OBSIDIAN_CORE_PLUGINS = [
    "file-explorer",
    "global-search",
    "switcher",
    "graph",
    "backlink",
    "canvas",
    "outgoing-link",
    "tag-pane",
    "page-preview",
    "properties",
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
    except OSError as exc:
        # Windows can report an installed script shim (for example npm.ps1)
        # as WinError 193 instead of FileNotFoundError.  Keep command
        # verification fail-closed while still writing its evidence record.
        return {
            "command": args,
            "cwd": str(cwd),
            "returncode": 126,
            "stdout": "",
            "stderr": str(exc),
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
        architecture = {}
        if version >= BOOT_FORMAT_VERSION:
            architecture = {
                "isa_version": reader.i32(),
                "required_features": reader.u64(),
                "scalar_word_trits": reader.i32(),
                "base_page_words": reader.i32(),
                "function_abi_version": reader.i32(),
                "syscall_abi_version": reader.i32(),
            }
        sections = []
        if version >= BOOT_TRANSITION_FORMAT_VERSION:
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
            app = {
                "name": reader.string(),
                "path": reader.string(),
                "text_ppn": reader.i32(),
                "entry_pc": reader.i32(),
                "text_pages": reader.i32(),
                "data_pages": reader.i32(),
                "stack_words": reader.i32(),
            }
            if version >= BOOT_FORMAT_VERSION:
                app.update(
                    {
                        "isa_version": reader.i32(),
                        "required_features": reader.u64(),
                        "function_abi_version": reader.i32(),
                        "syscall_abi_version": reader.i32(),
                    }
                )
            apps.append(app)
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
                rootfs_nonzero_blocks.add(index // STORAGE_BLOCK_WORDS)
        if reader.offset != len(payload):
            result["issues"].append("boot image has trailing payload bytes")
        if version not in {
            BOOT_LEGACY_FORMAT_VERSION,
            BOOT_TRANSITION_FORMAT_VERSION,
            BOOT_FORMAT_VERSION,
        }:
            result["issues"].append(f"unsupported boot image format version {version}")
        if boot_entry < 0 or boot_entry >= program_words:
            result["issues"].append("boot entry is outside text segment")
        if rootfs_words % STORAGE_BLOCK_WORDS != 0:
            result["issues"].append("rootfs seed is not block aligned")
        if version >= BOOT_TRANSITION_FORMAT_VERSION and not sections:
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
                    "rootfs_blocks": rootfs_words // STORAGE_BLOCK_WORDS,
                    "rootfs_nonzero_words": rootfs_nonzero_words,
                    "rootfs_nonzero_blocks": len(rootfs_nonzero_blocks),
                },
                "sections": sections,
                "apps": apps,
                "architecture": architecture,
            }
        )
    except ValueError as exc:
        result["issues"].append(str(exc))

    result["ok"] = len(result["issues"]) == 0
    return result


def raw_t40_to_long(raw: int) -> int:
    """Decode the canonical positional T40 float representation to an integer."""
    if raw == 0 or raw in {(1 << 64) - 1, (1 << 64) - 2}:
        return 0
    if raw > 3**40:
        raise ValueError(f"invalid raw T40 word 0x{raw:x}")
    trits: list[int] = []
    value = raw
    for _ in range(40):
        trits.append(value % 3 - 1)
        value //= 3
    mantissa = sum(trits[index] * (3**index) for index in range(33))
    exponent = sum(
        trits[33 + index] * (3**index) for index in range(7)
    )
    shift = exponent - 32
    if shift >= 0:
        return mantissa * (3**shift)
    divisor = 3 ** (-shift)
    return abs(mantissa) // divisor * (-1 if mantissa < 0 else 1)


def inspect_sparse_disk(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {
        "path": str(path),
        "ok": False,
        "issues": [],
        "format": "tdisk",
    }
    try:
        raw = path.read_bytes()
    except FileNotFoundError:
        result["issues"].append("disk image is missing")
        return result

    result["file_size"] = len(raw)
    if len(raw) < SPARSE_DISK_LEGACY_HEADER.size:
        result["issues"].append("sparse disk header is truncated")
        return result

    magic = struct.unpack_from("<Q", raw, 0)[0]
    legacy = magic == SPARSE_DISK_LEGACY_MAGIC
    if legacy:
        _, declared_records = SPARSE_DISK_LEGACY_HEADER.unpack_from(raw, 0)
        header_size = SPARSE_DISK_LEGACY_HEADER.size
        version = 1
        generation = 0
        expected_checksum = None
    elif magic == SPARSE_DISK_MAGIC:
        if len(raw) < SPARSE_DISK_HEADER.size:
            result["issues"].append("tDisk v2 header is truncated")
            return result
        (
            _,
            version,
            block_words,
            generation,
            expected_checksum,
            declared_records,
        ) = SPARSE_DISK_HEADER.unpack_from(raw, 0)
        header_size = SPARSE_DISK_HEADER.size
        if version != SPARSE_DISK_VERSION:
            result["issues"].append(
                f"unsupported sparse disk version {version}"
            )
        if block_words != STORAGE_BLOCK_WORDS:
            result["issues"].append(
                f"sparse disk block size is {block_words}, expected "
                f"{STORAGE_BLOCK_WORDS}"
            )
    else:
        result["issues"].append("sparse disk magic is invalid")
        return result

    result["header"] = {
        "magic": magic,
        "version": version,
        "block_words": STORAGE_BLOCK_WORDS,
        "generation": generation,
        "checksum": expected_checksum,
        "declared_records": declared_records,
        "legacy_read_only": legacy,
    }
    if declared_records < 0:
        result["issues"].append("sparse disk record count is negative")
        return result
    if result["issues"]:
        return result

    available_records = max(
        0, (len(raw) - header_size) // SPARSE_DISK_RECORD_SIZE
    )
    records_to_read = min(declared_records, available_records)
    recoverable_tail = declared_records > available_records
    blocks: dict[int, list[int]] = {}
    raw_blocks: dict[int, list[int]] = {}
    offset = header_size
    for _ in range(records_to_read):
        index = SPARSE_DISK_RECORD_HEADER.unpack_from(raw, offset)[0]
        offset += SPARSE_DISK_RECORD_HEADER.size
        words = []
        raw_words = []
        for _word in range(STORAGE_BLOCK_WORDS):
            if legacy:
                word = struct.unpack_from("<q", raw, offset)[0]
                words.append(word)
                raw_words.append(word)
            else:
                raw_word = SPARSE_DISK_WORD.unpack_from(raw, offset)[0]
                raw_words.append(raw_word)
                words.append(raw_t40_to_long(raw_word))
            offset += SPARSE_DISK_WORD.size
        if index >= 0:
            if any(word != 0 for word in words):
                blocks[index] = words
                raw_blocks[index] = raw_words
            else:
                blocks.pop(index, None)
                raw_blocks.pop(index, None)

    ignored_tail_bytes = max(0, len(raw) - offset)
    if not legacy and not recoverable_tail:
        actual_checksum = fnv1a(raw[header_size:offset])
        if actual_checksum != expected_checksum:
            result["issues"].append("sparse disk checksum validation failed")
    result.update(
        {
            "ok": len(result["issues"]) == 0,
            "declared_records": declared_records,
            "readable_records": records_to_read,
            "available_records": available_records,
            "recoverable_tail": recoverable_tail,
            "ignored_tail_bytes": ignored_tail_bytes,
            "live_blocks": len(blocks),
            "blocks": blocks,
            "raw_blocks": raw_blocks,
        }
    )
    return result


def write_sparse_disk(path: Path, blocks: dict[int, list[int]],
                      generation: int = 1) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    live_indices = sorted(
        index for index, words in blocks.items() if any(word != 0 for word in words)
    )
    records = bytearray()
    for index in live_indices:
        words = blocks[index]
        if len(words) != STORAGE_BLOCK_WORDS:
            raise ValueError(
                f"block {index} has {len(words)} words, expected "
                f"{STORAGE_BLOCK_WORDS}"
            )
        records.extend(SPARSE_DISK_RECORD_HEADER.pack(index))
        for word in words:
            records.extend(SPARSE_DISK_WORD.pack(int(word)))
    with path.open("wb") as handle:
        handle.write(
            SPARSE_DISK_HEADER.pack(
                SPARSE_DISK_MAGIC,
                SPARSE_DISK_VERSION,
                STORAGE_BLOCK_WORDS,
                max(1, generation),
                fnv1a(records),
                len(live_indices),
            )
        )
        handle.write(records)


def compact_sparse_disk(source: Path, output: Path | None = None) -> dict[str, Any]:
    inspected = inspect_sparse_disk(source)
    result: dict[str, Any] = {
        "source": str(source),
        "output": str(output or source),
        "ok": False,
        "before": {
            "file_size": inspected.get("file_size", 0),
            "declared_records": inspected.get("declared_records", 0),
            "readable_records": inspected.get("readable_records", 0),
            "live_blocks": inspected.get("live_blocks", 0),
            "recoverable_tail": inspected.get("recoverable_tail", False),
        },
        "after": {},
        "issues": list(inspected.get("issues", [])),
    }
    if not inspected.get("ok"):
        return result

    target = output or source
    temp = target.with_name(target.name + ".compact")
    blocks = inspected["raw_blocks"]
    try:
        write_sparse_disk(
            temp,
            blocks,
            inspected.get("header", {}).get("generation", 0) + 1,
        )
        if target == source:
            os.replace(temp, target)
        else:
            os.replace(temp, target)
        after = inspect_sparse_disk(target)
        result["after"] = {
            "file_size": after.get("file_size", 0),
            "declared_records": after.get("declared_records", 0),
            "readable_records": after.get("readable_records", 0),
            "live_blocks": after.get("live_blocks", 0),
            "recoverable_tail": after.get("recoverable_tail", False),
        }
        result["ok"] = bool(after.get("ok")) and after.get("live_blocks") == inspected.get("live_blocks")
    except OSError as exc:
        result["issues"].append(str(exc))
    finally:
        try:
            if temp.exists():
                temp.unlink()
        except OSError:
            pass
    return result


def text_status(label: str, ok: bool, detail: str = "") -> None:
    prefix = "ok" if ok else "warn"
    suffix = f" - {detail}" if detail else ""
    print(f"[{prefix}] {label}{suffix}")


def canonical_json(data: Any) -> str:
    return json.dumps(data, indent=2, sort_keys=True) + "\n"


REGISTRY_ID_RE = re.compile(r"^trit\.[a-z0-9]+(?:[.-][a-z0-9]+)*$")
REGISTRY_SOURCE_STATUSES = {"resolved", "missing"}
REGISTRY_COVERAGE_STATUSES = {"complete", "partial", "planned", "not_available"}
REGISTRY_DECISION_DISPOSITIONS = {"accepted", "superseded", "rejected", "open"}


def _registry_error(errors: list[str], message: str) -> None:
    errors.append(message)


def _registry_id(value: Any, location: str, errors: list[str]) -> bool:
    if not isinstance(value, str) or not REGISTRY_ID_RE.fullmatch(value):
        _registry_error(errors, f"{location} must be a namespaced trit.* id")
        return False
    return True


def _registry_ids(
    records: Any,
    key: str,
    location: str,
    errors: list[str],
) -> set[str]:
    if isinstance(records, dict):
        records = records.get(key)
    if not isinstance(records, list):
        _registry_error(errors, f"{location}.{key} must be an array")
        return set()
    found: set[str] = set()
    for index, record in enumerate(records):
        if not isinstance(record, dict):
            _registry_error(errors, f"{location}.{key}[{index}] must be an object")
            continue
        value = record.get("id")
        if not _registry_id(value, f"{location}.{key}[{index}].id", errors):
            continue
        if value in found:
            _registry_error(errors, f"duplicate id {value} in {location}.{key}")
        found.add(value)
    return found


def _registry_path(path_value: Any, location: str, errors: list[str]) -> None:
    if not isinstance(path_value, str) or not path_value.strip():
        _registry_error(errors, f"{location}.path must be a non-empty repository-relative path")
        return
    candidate = (REPO_ROOT / Path(path_value)).resolve()
    try:
        candidate.relative_to(REPO_ROOT.resolve())
    except ValueError:
        _registry_error(errors, f"{location}.path escapes the repository: {path_value}")
        return
    if not candidate.exists():
        _registry_error(errors, f"{location}.path does not resolve: {path_value}")


def _registry_source_refs(
    refs: Any,
    location: str,
    errors: list[str],
) -> None:
    if not isinstance(refs, list) or not refs:
        _registry_error(errors, f"{location} must be a non-empty array")
        return
    for index, ref in enumerate(refs):
        ref_location = f"{location}[{index}]"
        if not isinstance(ref, dict):
            _registry_error(errors, f"{ref_location} must be an object")
            continue
        for field in ("repository", "commit", "path", "status"):
            if not isinstance(ref.get(field), str) or not ref[field].strip():
                _registry_error(errors, f"{ref_location}.{field} must be a non-empty string")
        status = ref.get("status")
        if status not in REGISTRY_SOURCE_STATUSES:
            _registry_error(errors, f"{ref_location}.status must be resolved or missing")
        elif status == "resolved":
            _registry_path(ref.get("path"), ref_location, errors)
        elif not isinstance(ref.get("reason"), str) or not ref["reason"].strip():
            _registry_error(errors, f"{ref_location}.reason is required for an explicitly missing path")


def _registry_check_source_refs(
    data: dict[str, Any],
    location: str,
    errors: list[str],
) -> None:
    if "source_refs" in data:
        _registry_source_refs(data["source_refs"], f"{location}.source_refs", errors)


def _registry_string_refs(
    values: Any,
    location: str,
    errors: list[str],
    required: bool = False,
) -> set[str]:
    if not isinstance(values, list):
        _registry_error(errors, f"{location} must be an array")
        return set()
    if required and not values:
        _registry_error(errors, f"{location} must not be empty")
    result: set[str] = set()
    for index, value in enumerate(values):
        if not isinstance(value, str) or not value.strip():
            _registry_error(errors, f"{location}[{index}] must be a non-empty string")
        else:
            result.add(value)
    return result


def _registry_check_layer_refs(
    record: dict[str, Any],
    location: str,
    layer_ids: set[str],
    errors: list[str],
) -> None:
    for field in ("layer_ids",):
        refs = _registry_string_refs(record.get(field), f"{location}.{field}", errors, required=True)
        for ref in refs:
            if ref not in layer_ids:
                _registry_error(errors, f"{location}.{field} references unknown layer {ref}")


def _registry_check_path_strings(
    values: Any,
    location: str,
    errors: list[str],
) -> None:
    refs = _registry_string_refs(values, location, errors, required=True)
    for ref in refs:
        _registry_path(ref, location, errors)


def _registry_load_bundle() -> tuple[dict[str, dict[str, Any]], list[str]]:
    bundle: dict[str, dict[str, Any]] = {}
    errors: list[str] = []
    for kind, filename in REGISTRY_FILES.items():
        data, error = load_json_file(REPO_ROOT / filename)
        if error or data is None:
            _registry_error(errors, f"{filename}: {error or 'missing JSON object'}")
        else:
            bundle[kind] = data
    return bundle, errors


def _registry_validate_stack(
    stack: dict[str, Any],
    contracts: dict[str, Any],
    capabilities: dict[str, Any],
    decisions: dict[str, Any],
    errors: list[str],
) -> dict[str, Any]:
    if stack.get("schema") != "trit.stack_manifest.v1":
        _registry_error(errors, "STACK_MANIFEST.json has the wrong schema")
    _registry_check_source_refs(stack, "stack", errors)
    layers = stack.get("layers")
    if not isinstance(layers, list):
        _registry_error(errors, "stack.layers must be an array")
        layers = []
    layer_ids = _registry_ids(stack, "layers", "stack", errors)
    if len(layers) != 21:
        _registry_error(errors, f"stack.layers must contain exactly 21 phases, found {len(layers)}")
    ordered = stack.get("ordered_phase_ids")
    if not isinstance(ordered, list) or len(ordered) != 21:
        _registry_error(errors, "stack.ordered_phase_ids must contain exactly 21 ids")
        ordered = []
    if ordered and ordered != [record.get("id") for record in layers if isinstance(record, dict)]:
        _registry_error(errors, "stack.ordered_phase_ids must match the layer list order")
    expected_phases = list(range(21))
    actual_phases = [record.get("phase") for record in layers if isinstance(record, dict)]
    if actual_phases != expected_phases:
        _registry_error(errors, "stack phases must be the ordered integers 0 through 20")

    contract_ids = _registry_ids(contracts, "contracts", "contracts", errors)
    capability_ids = _registry_ids(capabilities, "capabilities", "capabilities", errors)
    decision_ids = _registry_ids(decisions, "decisions", "decisions", errors)
    gaps = stack.get("known_gaps", [])
    gap_ids = _registry_ids({"items": gaps}, "items", "stack.known_gaps", errors)
    releases = stack.get("releases", [])
    release_ids = _registry_ids({"items": releases}, "items", "stack.releases", errors)

    edge_pairs: set[tuple[str, str]] = set()
    edges = stack.get("dependency_edges")
    if not isinstance(edges, list) or not edges:
        _registry_error(errors, "stack.dependency_edges must be a non-empty array")
        edges = []
    phase_by_id: dict[str, int] = {}
    for index, layer in enumerate(layers):
        location = f"stack.layers[{index}]"
        if not isinstance(layer, dict):
            _registry_error(errors, f"{location} must be an object")
            continue
        _registry_check_source_refs(layer, location, errors)
        layer_id = layer.get("id")
        phase = layer.get("phase")
        if isinstance(layer_id, str) and isinstance(phase, int):
            phase_by_id[layer_id] = phase
        dependencies = _registry_string_refs(layer.get("depends_on"), f"{location}.depends_on", errors)
        for dependency in dependencies:
            if dependency not in layer_ids:
                _registry_error(errors, f"{location}.depends_on references unknown layer {dependency}")
            elif isinstance(phase, int) and phase_by_id.get(dependency, phase) >= phase:
                _registry_error(errors, f"{location}.depends_on is not ordered before phase {phase}: {dependency}")
        for field, known in (("contract_ids", contract_ids), ("capability_ids", capability_ids), ("gap_refs", gap_ids), ("release_refs", release_ids)):
            refs = _registry_string_refs(layer.get(field), f"{location}.{field}", errors)
            for ref in refs:
                if ref not in known:
                    _registry_error(errors, f"{location}.{field} references unknown id {ref}")
        _registry_string_refs(layer.get("test_refs"), f"{location}.test_refs", errors)
        _registry_string_refs(layer.get("benchmark_refs"), f"{location}.benchmark_refs", errors)
    for index, edge in enumerate(edges):
        location = f"stack.dependency_edges[{index}]"
        if not isinstance(edge, dict):
            _registry_error(errors, f"{location} must be an object")
            continue
        source = edge.get("from")
        target = edge.get("to")
        if source not in layer_ids or target not in layer_ids:
            _registry_error(errors, f"{location} references an unknown layer")
            continue
        if source == target or phase_by_id.get(source, 999) >= phase_by_id.get(target, -1):
            _registry_error(errors, f"{location} is not ordered from an earlier phase to a later phase")
        edge_pairs.add((source, target))
    for layer in layers:
        if not isinstance(layer, dict):
            continue
        target = layer.get("id")
        for source in layer.get("depends_on", []) if isinstance(layer.get("depends_on"), list) else []:
            if (source, target) not in edge_pairs:
                _registry_error(errors, f"missing dependency edge {source} -> {target}")

    return {
        "layer_ids": layer_ids,
        "contract_ids": contract_ids,
        "capability_ids": capability_ids,
        "decision_ids": decision_ids,
        "gap_ids": gap_ids,
        "release_ids": release_ids,
        "phase_by_id": phase_by_id,
    }


def _registry_validate_contracts(
    contracts: dict[str, Any],
    indexes: dict[str, Any],
    errors: list[str],
) -> None:
    if contracts.get("schema") != "trit.contract_manifest.v1":
        _registry_error(errors, "CONTRACT_MANIFEST.json has the wrong schema")
    _registry_check_source_refs(contracts, "contracts", errors)
    for index, contract in enumerate(contracts.get("contracts", []) if isinstance(contracts.get("contracts"), list) else []):
        location = f"contracts.contracts[{index}]"
        if not isinstance(contract, dict):
            _registry_error(errors, f"{location} must be an object")
            continue
        _registry_check_source_refs(contract, location, errors)
        _registry_check_layer_refs(contract, location, indexes["layer_ids"], errors)
        for field, known in (("test_refs", None), ("decision_ids", indexes["decision_ids"])):
            refs = _registry_string_refs(contract.get(field), f"{location}.{field}", errors)
            if known is not None:
                for ref in refs:
                    if ref not in known:
                        _registry_error(errors, f"{location}.{field} references unknown id {ref}")
        for field in ("contract_status", "maturity_status", "compatibility_status", "evidence_status"):
            if not isinstance(contract.get(field), str) or not contract[field].strip():
                _registry_error(errors, f"{location}.{field} must be explicit")


def _registry_validate_capabilities(
    capabilities: dict[str, Any],
    indexes: dict[str, Any],
    errors: list[str],
) -> None:
    if capabilities.get("schema") != "trit.capability_manifest.v1":
        _registry_error(errors, "CAPABILITY_MANIFEST.json has the wrong schema")
    _registry_check_source_refs(capabilities, "capabilities", errors)
    records = capabilities.get("capabilities", [])
    if not isinstance(records, list):
        _registry_error(errors, "capabilities.capabilities must be an array")
        records = []
    for index, capability in enumerate(records):
        location = f"capabilities.capabilities[{index}]"
        if not isinstance(capability, dict):
            _registry_error(errors, f"{location} must be an object")
            continue
        _registry_check_source_refs(capability, location, errors)
        _registry_check_layer_refs(capability, location, indexes["layer_ids"], errors)
        for field, known in (("contract_ids", indexes["contract_ids"]), ("depends_on_capability_ids", indexes["capability_ids"]), ("gap_refs", indexes["gap_ids"])):
            refs = _registry_string_refs(capability.get(field), f"{location}.{field}", errors)
            for ref in refs:
                if ref not in known:
                    _registry_error(errors, f"{location}.{field} references unknown id {ref}")
        for field in ("test_refs", "benchmark_refs"):
            _registry_string_refs(capability.get(field), f"{location}.{field}", errors)
        for field in ("status", "maturity_status", "compatibility_status", "evidence_status"):
            if not isinstance(capability.get(field), str) or not capability[field].strip():
                _registry_error(errors, f"{location}.{field} must be explicit")

    compatibility_names = {
        str(record.get("encoding_name", "")).lower()
        for record in records
        if isinstance(record, dict) and record.get("capability_class") == "symbolic_encoding" and record.get("encoding_family") == "compatibility"
    }
    required_compatibility = {"ascii", "utf-8", "hexadecimal"}
    if not required_compatibility <= compatibility_names:
        _registry_error(errors, "symbolic compatibility capabilities must separately cover ASCII, UTF-8, and hexadecimal")
    native_names = {
        str(record.get("encoding_name", "")).lower()
        for record in records
        if isinstance(record, dict) and record.get("capability_class") == "symbolic_encoding" and record.get("encoding_family") == "ternary_native"
    }
    required_native = {"tascii-81", "exact-trit-literals", "base-27-base-81-dump"}
    if not required_native <= native_names:
        _registry_error(errors, "ternary-native symbolic capabilities must separately cover TASCII-81, trit literals, and base-27/base-81 dumps")
    encrypted = [
        record for record in records
        if isinstance(record, dict) and record.get("design_family") == "encrypted_volume"
    ]
    encrypted_families = {record.get("encoding_family") for record in encrypted}
    if not {"compatibility", "ternary_native"} <= encrypted_families:
        _registry_error(errors, "encrypted-volume compatibility and ternary-native design capabilities must be separate records")
    storage_layer = "trit.stack.phase.14.storage-vfs"
    if encrypted and any(storage_layer not in record.get("layer_ids", []) for record in encrypted if isinstance(record, dict)):
        _registry_error(errors, "each encrypted-volume capability must depend on the storage/VFS layer")


def _registry_validate_decisions(
    decisions: dict[str, Any],
    indexes: dict[str, Any],
    errors: list[str],
) -> None:
    if decisions.get("schema") != "trit.decision_manifest.v1":
        _registry_error(errors, "DECISION_MANIFEST.json has the wrong schema")
    _registry_check_source_refs(decisions, "decisions", errors)
    records = decisions.get("decisions", [])
    if not isinstance(records, list):
        _registry_error(errors, "decisions.decisions must be an array")
        records = []
    groups: dict[str, list[dict[str, Any]]] = {}
    for index, decision in enumerate(records):
        location = f"decisions.decisions[{index}]"
        if not isinstance(decision, dict):
            _registry_error(errors, f"{location} must be an object")
            continue
        _registry_check_source_refs(decision, location, errors)
        disposition = decision.get("disposition")
        if disposition not in REGISTRY_DECISION_DISPOSITIONS:
            _registry_error(errors, f"{location}.disposition must be accepted, superseded, rejected, or open")
        conflict_set = decision.get("conflict_set")
        if conflict_set is not None:
            if not isinstance(conflict_set, str) or not conflict_set.strip():
                _registry_error(errors, f"{location}.conflict_set must be null or a non-empty string")
            else:
                groups.setdefault(conflict_set, []).append(decision)
        if not isinstance(decision.get("canonical"), bool):
            _registry_error(errors, f"{location}.canonical must be boolean")
        if disposition == "accepted" and decision.get("canonical") is not True:
            _registry_error(errors, f"{location}: an accepted decision must be canonical")
        if disposition in {"superseded", "rejected", "open"} and decision.get("canonical") is True:
            _registry_error(errors, f"{location}: {disposition} decisions cannot be canonical")
        current_truth = decision.get("current_truth")
        if current_truth is not None and current_truth not in indexes["contract_ids"]:
            _registry_error(errors, f"{location}.current_truth references unknown contract {current_truth}")
        for field, known in (("supersedes", indexes["decision_ids"]), ("affected_contract_ids", indexes["contract_ids"]), ("affected_capability_ids", indexes["capability_ids"])):
            refs = _registry_string_refs(decision.get(field), f"{location}.{field}", errors)
            for ref in refs:
                if ref not in known:
                    _registry_error(errors, f"{location}.{field} references unknown id {ref}")
    for conflict_set, group in groups.items():
        canonical = [decision for decision in group if decision.get("canonical") is True]
        if len(canonical) > 1:
            _registry_error(errors, f"conflict set {conflict_set} has simultaneous canonical decisions")
        if not canonical and not any(decision.get("disposition") == "open" for decision in group):
            _registry_error(errors, f"conflict set {conflict_set} has no canonical current truth or explicit open resolution")


def _registry_validate_coverage(
    coverage: dict[str, Any],
    indexes: dict[str, Any],
    errors: list[str],
    strict: bool,
) -> dict[str, Any]:
    if coverage.get("schema") != "trit.stack_coverage_report.v1":
        _registry_error(errors, "STACK_COVERAGE_REPORT.json has the wrong schema")
    _registry_check_source_refs(coverage, "coverage", errors)
    dimensions = coverage.get("dimensions")
    required_dimensions = ["specified", "implemented", "integrated", "tested", "benchmarked", "released"]
    if dimensions != required_dimensions:
        _registry_error(errors, "coverage.dimensions must distinguish specified, implemented, integrated, tested, benchmarked, and released")
    records = coverage.get("coverage", [])
    if not isinstance(records, list):
        _registry_error(errors, "coverage.coverage must be an array")
        records = []
    seen: set[str] = set()
    for index, record in enumerate(records):
        location = f"coverage.coverage[{index}]"
        if not isinstance(record, dict):
            _registry_error(errors, f"{location} must be an object")
            continue
        stack_id = record.get("stack_id")
        if stack_id not in indexes["layer_ids"]:
            _registry_error(errors, f"{location}.stack_id references unknown layer {stack_id}")
        elif stack_id in seen:
            _registry_error(errors, f"duplicate coverage record for {stack_id}")
        else:
            seen.add(stack_id)
        _registry_check_path_strings(record.get("source_refs"), f"{location}.source_refs", errors)
        for field, known in (("contract_ids", indexes["contract_ids"]), ("capability_ids", indexes["capability_ids"]), ("gap_refs", indexes["gap_ids"]), ("release_refs", indexes["release_ids"])):
            refs = _registry_string_refs(record.get(field), f"{location}.{field}", errors)
            for ref in refs:
                if ref not in known:
                    _registry_error(errors, f"{location}.{field} references unknown id {ref}")
        for field in ("test_refs", "benchmark_refs"):
            _registry_string_refs(record.get(field), f"{location}.{field}", errors)
        dimensions_data = record.get("coverage")
        if not isinstance(dimensions_data, dict):
            _registry_error(errors, f"{location}.coverage must be an object")
            continue
        for dimension in required_dimensions:
            entry = dimensions_data.get(dimension)
            entry_location = f"{location}.coverage.{dimension}"
            if not isinstance(entry, dict):
                _registry_error(errors, f"{entry_location} must be an object")
                continue
            status = entry.get("status")
            if status not in REGISTRY_COVERAGE_STATUSES:
                _registry_error(errors, f"{entry_location}.status is invalid")
            _registry_string_refs(entry.get("evidence_refs"), f"{entry_location}.evidence_refs", errors)
            if strict and status == "planned" and dimension in {"specified", "implemented", "integrated", "tested"}:
                _registry_error(errors, f"{entry_location} cannot be merely planned in strict coverage")
    missing = indexes["layer_ids"] - seen
    if missing:
        _registry_error(errors, f"coverage is missing stack layers: {', '.join(sorted(missing))}")
    return {"records": len(records), "covered_layers": len(seen), "required_layers": len(indexes["layer_ids"])}


def validate_registry(strict: bool = False) -> dict[str, Any]:
    bundle, errors = _registry_load_bundle()
    report: dict[str, Any] = {
        "schema": "trit.registry_validation_report.v1",
        "ok": False,
        "strict": strict,
        "files": {kind: REGISTRY_FILES[kind] for kind in REGISTRY_FILES},
        "errors": errors,
        "checks": {},
    }
    if errors:
        return report
    stack = bundle["stack"]
    capabilities = bundle["capability"]
    contracts = bundle["contract"]
    decisions = bundle["decision"]
    indexes = _registry_validate_stack(stack, contracts, capabilities, decisions, report["errors"])
    _registry_validate_contracts(contracts, indexes, report["errors"])
    _registry_validate_capabilities(capabilities, indexes, report["errors"])
    _registry_validate_decisions(decisions, indexes, report["errors"])
    coverage_summary = _registry_validate_coverage(bundle["coverage"], indexes, report["errors"], strict)
    report["checks"] = {
        "stack_layers": len(indexes["layer_ids"]),
        "contracts": len(indexes["contract_ids"]),
        "capabilities": len(indexes["capability_ids"]),
        "decisions": len(indexes["decision_ids"]),
        "known_gaps": len(indexes["gap_ids"]),
        "releases": len(indexes["release_ids"]),
        "coverage": coverage_summary,
    }
    report["ok"] = not report["errors"]
    return report


def _plan_index_report() -> dict[str, Any]:
    index_path = DOCS_DIR / "11_TreatCode_Platform" / "PLAN_INDEX.md"
    report: dict[str, Any] = {"path": rel(index_path), "ok": False, "errors": [], "plans": []}
    if not index_path.exists():
        report["errors"].append("plan index is missing")
        return report
    row_re = re.compile(r"^\|\s*(P\d+)\s*\|\s*\[([^]]+)\]\(([^)]+)\)\s*\|\s*([^|]+)\|\s*([^|]+)\|", re.MULTILINE)
    rows = row_re.findall(index_path.read_text(encoding="utf-8", errors="replace"))
    if len(rows) != 14:
        report["errors"].append(f"plan index must contain 14 plan rows, found {len(rows)}")
    seen: set[str] = set()
    status_values = {"not_started", "in_progress", "blocked", "complete", "superseded"}
    for plan_id, title, path_value, depends, status in rows:
        if plan_id in seen:
            report["errors"].append(f"duplicate plan id {plan_id}")
        seen.add(plan_id)
        status = status.strip()
        if status not in status_values:
            report["errors"].append(f"{plan_id} has invalid status {status}")
        plan_path = (index_path.parent / path_value).resolve()
        if not plan_path.exists():
            report["errors"].append(f"{plan_id} plan file is missing: {path_value}")
            continue
        text_value = plan_path.read_text(encoding="utf-8", errors="replace")
        metadata_id = re.search(r"\*\*Plan ID:\*\*\s*(P\d+)", text_value)
        metadata_status = re.search(r"\*\*Status:\*\s*`([^`]+)`", text_value)
        if not metadata_id or metadata_id.group(1) != plan_id:
            report["errors"].append(f"{plan_id} metadata id does not match the index")
        if not metadata_status or metadata_status.group(1) != status:
            report["errors"].append(f"{plan_id} metadata status does not match the index")
        dependencies = re.findall(r"P\d+", depends)
        report["plans"].append({
            "id": plan_id,
            "title": title,
            "path": rel(plan_path),
            "depends_on": dependencies,
            "status": status,
        })
    plan_ids = {item["id"] for item in report["plans"]}
    for plan in report["plans"]:
        for dependency in plan["depends_on"]:
            if dependency not in plan_ids:
                report["errors"].append(f"{plan['id']} depends on unknown plan {dependency}")
    report["ok"] = not report["errors"]
    return report


def _git_head() -> str:
    result = run_command(["git", "rev-parse", "HEAD"], capture=True, timeout=10)
    value = result.get("stdout", "").strip()
    return value if result.get("returncode") == 0 and value else "unknown"


def _artifact_record(path: Path) -> dict[str, Any]:
    record: dict[str, Any] = {"path": rel(path), "exists": path.exists()}
    if path.exists() and path.is_file():
        record["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    return record


def cmd_website_registry_validate(args: argparse.Namespace) -> int:
    report = validate_registry(strict=False)
    evidence_path = PLAN_EVIDENCE_DIR / "P02" / "registry-validation.json"
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    evidence_path.write_text(canonical_json(report), encoding="utf-8")
    if args.json:
        print_json(report)
    else:
        print("TreatCode registry validation")
        print(f"[{'ok' if report['ok'] else 'fail'}] registry files")
        if report.get("checks"):
            checks = report["checks"]
            print(
                "[ok] records "
                f"layers={checks.get('stack_layers', 0)} "
                f"capabilities={checks.get('capabilities', 0)} "
                f"contracts={checks.get('contracts', 0)} "
                f"decisions={checks.get('decisions', 0)}"
            )
        for error in report["errors"]:
            print(f"[fail] {error}")
    return 0 if report["ok"] else 1


def cmd_website_registry_coverage(args: argparse.Namespace) -> int:
    report = validate_registry(strict=bool(args.strict))
    coverage = report.get("checks", {}).get("coverage", {})
    evidence = {
        "schema": "trit.registry_coverage_result.v1",
        "strict": bool(args.strict),
        "ok": report["ok"],
        "coverage": coverage,
        "errors": report["errors"],
    }
    evidence_path = PLAN_EVIDENCE_DIR / "P02" / "registry-coverage.json"
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    evidence_path.write_text(canonical_json(evidence), encoding="utf-8")
    if args.json:
        print_json(evidence)
    else:
        print("TreatCode registry coverage")
        print(
            f"[{'ok' if report['ok'] else 'fail'}] "
            f"covered {coverage.get('covered_layers', 0)}/{coverage.get('required_layers', 21)} stack layers"
        )
        for error in report["errors"]:
            print(f"[fail] {error}")
    return 0 if report["ok"] else 1


def cmd_website_plans_validate(args: argparse.Namespace) -> int:
    report = _plan_index_report()
    if args.json:
        print_json(report)
    else:
        print("TreatCode plan validation")
        print(f"[{'ok' if report['ok'] else 'fail'}] {len(report['plans'])} plan entries")
        for error in report["errors"]:
            print(f"[fail] {error}")
    return 0 if report["ok"] else 1


def cmd_website_plan_verify(args: argparse.Namespace) -> int:
    plan_id = args.plan_id.upper()
    plan_report = _plan_index_report()
    target = next((plan for plan in plan_report["plans"] if plan["id"] == plan_id), None)
    registry_report = validate_registry(strict=True) if plan_id == "P02" else {"ok": False, "errors": [f"no verifier implemented for {plan_id}"]}
    dependency_status: dict[str, str] = {}
    if target:
        for dependency in target["depends_on"]:
            dependency_record = next((plan for plan in plan_report["plans"] if plan["id"] == dependency), None)
            dependency_status[dependency] = dependency_record["status"] if dependency_record else "missing"
    required_artifacts = [REPO_ROOT / REGISTRY_FILES[key] for key in ("stack", "capability", "contract", "decision", "coverage")]
    artifacts = [_artifact_record(path) for path in required_artifacts]
    dependency_ok = bool(target) and all(status == "complete" for status in dependency_status.values())
    human_approval_ok = False
    if target and target["status"] == "complete":
        plan_path = DOCS_DIR / "11_TreatCode_Platform" / "plans" / "P02_stack_capabilities_decisions.md"
        if plan_path.exists():
            plan_text = plan_path.read_text(encoding="utf-8", errors="replace")
            human_approval_ok = bool(re.search(r"Architecture owner[^\n]*approved", plan_text, re.IGNORECASE))
    checks = {
        "plan_index": plan_report["ok"],
        "registry": registry_report["ok"],
        "required_artifacts": all(item["exists"] for item in artifacts),
        "dependencies_complete": dependency_ok,
        "human_approval": human_approval_ok,
    }
    result: dict[str, Any] = {
        "schema": "trit.plan_verification_result.v1",
        "plan_id": plan_id,
        "plan_version": 1,
        "verified_commit": _git_head(),
        "ok": all(checks.values()),
        "completion_eligible": all(checks.values()),
        "checks": checks,
        "dependency_status": dependency_status,
        "registry_errors": registry_report.get("errors", []),
        "artifacts": artifacts,
        "commands": [
            {"command": "python tools/trit_tool.py website plans validate", "exit_code": 0 if plan_report["ok"] else 1},
            {"command": "python tools/trit_tool.py website registry validate", "exit_code": 0 if validate_registry(strict=False)["ok"] else 1},
            {"command": "python tools/trit_tool.py website registry coverage --strict", "exit_code": 0 if registry_report["ok"] else 1},
        ],
        "environment": {
            "platform": platform.platform(),
            "python": sys.version.split()[0],
            "repository": str(REPO_ROOT),
        },
        "human_approvals": {
            "required": ["Architecture owner"],
            "recorded": human_approval_ok,
        },
        "verification_date_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(),
    }
    evidence_path = REPO_ROOT / "build" / "treatcode-plan-evidence" / plan_id / "result.json"
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    evidence_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.json:
        print_json(result)
    else:
        print(f"TreatCode plan verification: {plan_id}")
        print(f"[{'ok' if result['ok'] else 'blocked'}] completion eligibility")
        for name, value in checks.items():
            print(f"[{'ok' if value else 'fail'}] {name}")
        print(f"evidence: {evidence_path}")
        if not result["ok"]:
            print("Plan completion remains blocked until all dependency and human gates are recorded.")
    return 0 if result["ok"] else 1


def write_text_if_changed(path: Path, text: str) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return False
    path.write_text(text, encoding="utf-8")
    return True


def obsidian_agents_text() -> str:
    return """# Docs Vault Agent Guide

Open `docs/` as the Obsidian vault for Trit.

## Authority

- Root manifests, source files, and tests remain authoritative.
- Pages in this vault are curated navigation and explanation.
- Generated Graphify artifacts are advisory and must not override source code,
  manifests, or failing tests.

## Workflow

1. Start at `README.md`, then follow `INDEX.md` and `STATUS.md`.
2. Use `python ../tools/trit_tool.py knowledge status` to validate the vault.
3. Use `python ../tools/trit_tool.py knowledge canvas` after changing the docs map.
4. Use `python ../tools/trit_tool.py knowledge graph` only when Graphify is installed
   and a structural code report would help.
5. Keep manually written docs concise and source-linked; put generated Graphify
   runs under `_graphify/runs/`.

## Obsidian Conventions

- Prefer stable Markdown links for repo portability.
- Use wikilinks sparingly for important concepts that benefit from graph view.
- Keep `trit-stack.canvas` as the high-level navigation canvas.
- Do not commit Obsidian workspace layout files.
"""


def graphify_readme_text() -> str:
    return """# Graphify Runs

This directory documents optional Graphify integration.

Graphify output is generated analysis, not a source of truth. The default raw
output directory is the repo-root `graphify-out/`, which is ignored by git.
Archived report snapshots may be written under `_graphify/runs/` by:

```powershell
python ..\\tools\\trit_tool.py knowledge graph
```

If Graphify is not installed, the command exits with guidance and leaves the
repo unchanged.

By default, `.graphifyignore` keeps Markdown, images, and the Obsidian vault out
of Graphify so extraction can run without an LLM API key. Obsidian remains the
docs/wiki layer; Graphify is the code graph layer.

Graphify does not natively parse `.trit` sources yet. Trit's `knowledge graph`
command therefore runs Graphify first, then augments `graphify-out/graph.json`
with a deterministic Trit adapter that extracts `.trit` file, function,
constant, syscall, and call edges. When the CMake `trit_ast_dump` target is
available, the adapter uses the compiler parser's `ModuleAst`; otherwise it
falls back to a lightweight text scan.
"""


def graphify_ignore_text() -> str:
    return """# Trit Graphify inputs
.git/
.obsidian/
docs/
docs/.obsidian/
build/
build_fresh/
build_cuda/
build_sycl/
graphify-out/
docs/_graphify/runs/
docs/.trash/
treatcode/node_modules/
node_modules/
scratch/
bitnet_weights/converted_t40/
bitnet_weights/model/
qwen3.627b_weights/converted_qwen/
qwen3.627b_weights/raw/
*.exe
*.img
*.tboot
*.tdisk
*.md
*.pdf
*.png
*.jpg
*.jpeg
*.gif
*.webp
*.svg
*.html
*.txt
*.canvas
CMakeLists.txt
"""


def obsidian_file_specs() -> dict[Path, str]:
    return {
        OBSIDIAN_DIR / "app.json": canonical_json(OBSIDIAN_APP_CONFIG),
        OBSIDIAN_DIR / "core-plugins.json": canonical_json(OBSIDIAN_CORE_PLUGINS),
        OBSIDIAN_DIR / "community-plugins.json": canonical_json([]),
        DOCS_DIR / "AGENTS.md": obsidian_agents_text(),
        GRAPHIFY_ARCHIVE_DIR / "README.md": graphify_readme_text(),
        REPO_ROOT / ".graphifyignore": graphify_ignore_text(),
        OBSIDIAN_CANVAS: canonical_json(build_obsidian_canvas()),
    }


def build_obsidian_canvas() -> dict[str, Any]:
    node_specs = [
        ("home", "README.md", 0, 0, 360, 240, "1"),
        ("index", "INDEX.md", 440, 0, 360, 240, "2"),
        ("status", "STATUS.md", 880, 0, 360, 240, "3"),
        ("quick_ref", "00_Quick_Ref/opcode_table.md", 0, 340, 320, 210, "4"),
        ("logic", "01_Logic_Level/gates.md", 380, 340, 320, 210, "5"),
        ("isa", "02_Hardware_ISA/encoding.md", 760, 340, 320, 210, "6"),
        ("vm", "03_Execution_Engine/vm_state.md", 1140, 340, 320, 210, "1"),
        ("abi", "04_Binary_Contract/abi_spec.md", 0, 650, 320, 210, "2"),
        ("symbols", "04_Binary_Contract/symbolic_encodings.md", 0, 960, 320, 210, "3"),
        ("compiler", "05_Compiler_Infra/ternary_ir.md", 380, 650, 320, 210, "3"),
        ("language", "06_Language/tcl_language.md", 760, 650, 320, 210, "4"),
        ("kernel", "07_OS_Substrate/kernel_overview.md", 1140, 650, 320, 210, "5"),
        ("apps", "08_Applications/app_sdk.md", 380, 960, 320, 210, "6"),
        ("host", "09_Host_Runtime/build_and_test.md", 760, 960, 320, 210, "1"),
        ("graphify", "_graphify/README.md", 1140, 960, 320, 210, "2"),
        ("benchmarks", "10_Benchmarks/system_benchmark_plan.md", 760, 1270, 320, 210, "3"),
    ]
    edge_specs = [
        ("home", "index", "navigation"),
        ("home", "status", "health"),
        ("index", "quick_ref", "cheatsheets"),
        ("index", "logic", "layer 0"),
        ("logic", "isa", "encodes"),
        ("isa", "vm", "executes"),
        ("vm", "abi", "calls"),
        ("abi", "symbols", "encodes"),
        ("symbols", "compiler", "literals"),
        ("abi", "compiler", "targets"),
        ("compiler", "language", "fronts"),
        ("language", "kernel", "boots"),
        ("kernel", "apps", "serves"),
        ("apps", "host", "bundles"),
        ("status", "graphify", "analysis"),
        ("graphify", "index", "reports"),
        ("host", "benchmarks", "measures"),
        ("benchmarks", "status", "gates"),
    ]
    return {
        "nodes": [
            {
                "id": node_id,
                "type": "file",
                "file": file_name,
                "x": x,
                "y": y,
                "width": width,
                "height": height,
                "color": color,
            }
            for node_id, file_name, x, y, width, height, color in node_specs
        ],
        "edges": [
            {
                "id": f"{from_node}_to_{to_node}",
                "fromNode": from_node,
                "fromSide": "right",
                "toNode": to_node,
                "toSide": "left",
                "label": label,
            }
            for from_node, to_node, label in edge_specs
        ],
    }


def read_json_file_any(path: Path) -> tuple[Any | None, str | None]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle), None
    except FileNotFoundError:
        return None, "file is missing"
    except json.JSONDecodeError as exc:
        return None, f"invalid JSON at line {exc.lineno}: {exc.msg}"


def docs_markdown_files() -> list[Path]:
    if not DOCS_DIR.exists():
        return []
    return sorted(
        path
        for path in DOCS_DIR.rglob("*.md")
        if ".obsidian" not in path.parts and ".trash" not in path.parts
    )


def is_external_doc_link(target: str) -> bool:
    lowered = target.lower()
    return (
        "://" in target
        or lowered.startswith("mailto:")
        or lowered.startswith("tel:")
        or lowered.startswith("obsidian:")
    )


def markdown_link_target(raw: str) -> str:
    target = raw.strip()
    if " " in target and target.split()[0].lower().endswith(".md"):
        target = target.split()[0]
    target = target.split("#", 1)[0].strip()
    return unquote(target)


def check_docs_markdown_links() -> list[dict[str, str]]:
    broken: list[dict[str, str]] = []
    files = docs_markdown_files()
    stem_index: dict[str, list[Path]] = {}
    for path in files:
        stem_index.setdefault(path.stem.lower(), []).append(path)

    markdown_re = re.compile(r"(?<!!)\[[^\]]+\]\(([^)]+)\)")
    wikilink_re = re.compile(r"\[\[([^\]|#]+)(?:#[^\]|]+)?(?:\|[^\]]+)?\]\]")
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in markdown_re.finditer(text):
            target = markdown_link_target(match.group(1))
            if not target or target.startswith("#") or is_external_doc_link(target):
                continue
            if not target.lower().endswith(".md"):
                continue
            resolved = (path.parent / target).resolve()
            if not resolved.exists():
                broken.append({"file": rel(path), "target": target, "kind": "markdown"})
        for match in wikilink_re.finditer(text):
            target = markdown_link_target(match.group(1))
            if not target or is_external_doc_link(target):
                continue
            if "/" in target or "\\" in target:
                candidate = (DOCS_DIR / target).with_suffix(".md") if not target.endswith(".md") else (DOCS_DIR / target)
                if not candidate.exists():
                    broken.append({"file": rel(path), "target": target, "kind": "wikilink"})
            elif target.lower() not in stem_index:
                broken.append({"file": rel(path), "target": target, "kind": "wikilink"})
    return broken


def canvas_status() -> dict[str, Any]:
    data, error = read_json_file_any(OBSIDIAN_CANVAS)
    if error:
        return {"ok": False, "error": error}
    if not isinstance(data, dict):
        return {"ok": False, "error": "canvas JSON must be an object"}
    nodes = data.get("nodes")
    edges = data.get("edges")
    if not isinstance(nodes, list) or not isinstance(edges, list):
        return {"ok": False, "error": "canvas must contain node and edge arrays"}
    node_ids = {node.get("id") for node in nodes if isinstance(node, dict)}
    missing_files = []
    for node in nodes:
        if not isinstance(node, dict) or node.get("type") != "file":
            continue
        file_name = node.get("file")
        if isinstance(file_name, str) and not (DOCS_DIR / file_name).exists():
            missing_files.append(file_name)
    broken_edges = []
    for edge in edges:
        if not isinstance(edge, dict):
            broken_edges.append("<non-object edge>")
            continue
        if edge.get("fromNode") not in node_ids or edge.get("toNode") not in node_ids:
            broken_edges.append(str(edge.get("id", "<unnamed edge>")))
    return {
        "ok": not missing_files and not broken_edges,
        "nodes": len(nodes),
        "edges": len(edges),
        "missing_files": missing_files,
        "broken_edges": broken_edges,
    }


def find_tool_executable(name: str) -> str | None:
    found = shutil.which(name)
    if found:
        return found
    suffixes = [".exe", ".cmd", ".bat", ""]
    candidate_dirs = [
        Path.home() / ".local" / "bin",
        Path.home() / "AppData" / "Roaming" / "Python" / f"Python{sys.version_info.major}{sys.version_info.minor}" / "Scripts",
    ]
    for directory in candidate_dirs:
        for suffix in suffixes:
            candidate = directory / f"{name}{suffix}"
            if candidate.exists():
                return str(candidate)
    return None


TRIT_FUNCTION_RE = re.compile(r"^\s*fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*(?:->\s*([A-Za-z0-9_\[\]<>:]+))?")
TRIT_CONST_RE = re.compile(r"^\s*const\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([^=;]+)")
TRIT_CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
TRIT_SKIP_CALLS = {
    "if",
    "while",
    "match",
    "return",
    "unsafe",
    "fn",
    "const",
    "var",
}


def graph_slug(value: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9]+", "_", value).strip("_").lower()
    if not slug:
        return "unnamed"
    if slug[0].isdigit():
        return "n_" + slug
    return slug


def trit_file_node_id(path: Path) -> str:
    rel_path = rel(path.with_suffix(""))
    return "trit_" + graph_slug(rel_path)


def trit_symbol_node_id(path: Path, name: str, kind: str) -> str:
    return f"{trit_file_node_id(path)}_{graph_slug(kind)}_{graph_slug(name)}"


def trit_source_files() -> list[Path]:
    excluded = {
        ".git",
        ".obsidian",
        ".trash",
        "build",
        "build_fresh",
        "build_cuda",
        "build_sycl",
        "graphify-out",
        "node_modules",
    }
    out = []
    for path in REPO_ROOT.rglob("*.trit"):
        parts = set(path.relative_to(REPO_ROOT).parts)
        if excluded.intersection(parts):
            continue
        out.append(path)
    return sorted(out)


def strip_trit_comment(line: str) -> str:
    return line.split("//", 1)[0]


def extract_trit_graph_regex() -> dict[str, Any]:
    nodes: list[dict[str, Any]] = []
    edges: list[dict[str, Any]] = []
    symbol_by_name: dict[str, list[str]] = {}
    functions: dict[str, dict[str, Any]] = {}
    pending_calls: list[dict[str, Any]] = []
    files = trit_source_files()

    def add_node(node: dict[str, Any]) -> None:
        nodes.append(node)

    def add_edge(edge: dict[str, Any]) -> None:
        edges.append(edge)

    for path in files:
        rel_path = rel(path)
        file_id = trit_file_node_id(path)
        add_node(
            {
                "id": file_id,
                "label": path.name,
                "file_type": "code",
                "source_file": rel_path,
                "source_location": "L1",
                "_origin": "trit-adapter",
            }
        )
        active_function: dict[str, Any] | None = None
        brace_depth = 0
        for line_no, raw_line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
            clean = strip_trit_comment(raw_line)
            fn_match = TRIT_FUNCTION_RE.match(clean)
            const_match = TRIT_CONST_RE.match(clean)
            if fn_match:
                name = fn_match.group(1)
                params = fn_match.group(2).strip()
                return_type = (fn_match.group(3) or "").strip()
                node_id = trit_symbol_node_id(path, name, "fn")
                node = {
                    "id": node_id,
                    "label": f"{name}()",
                    "file_type": "code",
                    "source_file": rel_path,
                    "source_location": f"L{line_no}",
                    "_origin": "trit-adapter",
                    "kind": "trit_function",
                    "signature": clean.strip().rstrip("{").strip(),
                    "parameters": params,
                    "return_type": return_type,
                }
                add_node(node)
                add_edge(
                    {
                        "source": file_id,
                        "target": node_id,
                        "relation": "contains",
                        "confidence": "EXTRACTED",
                        "source_file": rel_path,
                        "source_location": f"L{line_no}",
                        "weight": 1.0,
                        "context": "trit_function",
                    }
                )
                symbol_by_name.setdefault(name, []).append(node_id)
                functions[node_id] = {"name": name, "file": rel_path}
                active_function = {"id": node_id, "name": name}
                brace_depth = clean.count("{") - clean.count("}")
            elif const_match:
                name = const_match.group(1)
                value_type = const_match.group(2).strip()
                node_id = trit_symbol_node_id(path, name, "const")
                add_node(
                    {
                        "id": node_id,
                        "label": name,
                        "file_type": "code",
                        "source_file": rel_path,
                        "source_location": f"L{line_no}",
                        "_origin": "trit-adapter",
                        "kind": "trit_const",
                        "value_type": value_type,
                    }
                )
                add_edge(
                    {
                        "source": file_id,
                        "target": node_id,
                        "relation": "contains",
                        "confidence": "EXTRACTED",
                        "source_file": rel_path,
                        "source_location": f"L{line_no}",
                        "weight": 1.0,
                        "context": "trit_const",
                    }
                )
                symbol_by_name.setdefault(name, []).append(node_id)

            if active_function is not None:
                for call_match in TRIT_CALL_RE.finditer(clean):
                    call_name = call_match.group(1)
                    if call_name in TRIT_SKIP_CALLS or call_name == active_function["name"]:
                        continue
                    pending_calls.append(
                        {
                            "source": active_function["id"],
                            "name": call_name,
                            "source_file": rel_path,
                            "source_location": f"L{line_no}",
                        }
                    )
                brace_depth += clean.count("{") - clean.count("}") if not fn_match else 0
                if brace_depth <= 0 and "}" in clean:
                    active_function = None
                    brace_depth = 0

    external_nodes: dict[str, dict[str, Any]] = {}
    seen_edges: set[tuple[str, str, str, str]] = set()
    for call in pending_calls:
        targets = symbol_by_name.get(call["name"], [])
        if not targets and call["name"].startswith("sys_"):
            target = "trit_external_" + graph_slug(call["name"])
            external_nodes.setdefault(
                target,
                {
                    "id": target,
                    "label": f"{call['name']}()",
                    "file_type": "code",
                    "source_file": "",
                    "source_location": "",
                    "_origin": "trit-adapter",
                    "kind": "trit_external_syscall",
                },
            )
            targets = [target]
        for target in targets:
            key = (call["source"], target, call["source_file"], call["source_location"])
            if key in seen_edges:
                continue
            seen_edges.add(key)
            add_edge(
                {
                    "source": call["source"],
                    "target": target,
                    "relation": "calls",
                    "confidence": "EXTRACTED",
                    "source_file": call["source_file"],
                    "source_location": call["source_location"],
                    "weight": 1.0,
                    "context": "trit_call",
                }
            )

    nodes.extend(external_nodes.values())
    return {
        "extractor": "regex",
        "nodes": nodes,
        "edges": edges,
        "files": len(files),
        "functions": sum(1 for node in nodes if node.get("kind") == "trit_function"),
        "constants": sum(1 for node in nodes if node.get("kind") == "trit_const"),
        "external_syscalls": len(external_nodes),
        "call_edges": sum(1 for edge in edges if edge.get("relation") == "calls"),
    }


def find_trit_ast_dump(build_dir: Path | None = None) -> Path | None:
    return find_executable(build_dir or default_build_dir(), TRIT_AST_DUMP_TARGET)


def ensure_trit_ast_dump(build_dir: Path | None = None) -> tuple[Path | None, dict[str, Any]]:
    build_dir = build_dir or default_build_dir()
    existing = find_trit_ast_dump(build_dir)
    if existing:
        return existing, {"available": True, "built": False, "path": str(existing)}
    if not (build_dir / "CMakeCache.txt").exists() or not shutil.which("cmake"):
        return None, {"available": False, "built": False, "path": None}

    result = run_command(
        ["cmake", "--build", str(build_dir), "--target", TRIT_AST_DUMP_TARGET],
        cwd=REPO_ROOT,
        capture=True,
        timeout=180,
    )
    built = find_trit_ast_dump(build_dir)
    status = {
        "available": built is not None,
        "built": built is not None,
        "path": str(built) if built else None,
        "build": result,
    }
    return built, status


def load_trit_ast_modules(files: list[Path]) -> tuple[dict[str, Any] | None, dict[str, Any]]:
    dumper, status = ensure_trit_ast_dump()
    if not dumper:
        return None, status
    command = [str(dumper), *[rel(path) for path in files]]
    result = run_command(command, cwd=REPO_ROOT, capture=True, timeout=180)
    status = {
        **status,
        "dump": {
            "command": result["command"],
            "cwd": result["cwd"],
            "returncode": result["returncode"],
            "stderr": result["stderr"][:4000],
            "duration_seconds": result["duration_seconds"],
            "stdout_bytes": len(result["stdout"].encode("utf-8", errors="replace")),
        },
    }
    if result["returncode"] != 0:
        status["available"] = False
        return None, status
    try:
        data = json.loads(result["stdout"])
    except json.JSONDecodeError as exc:
        status["available"] = False
        status["error"] = f"invalid AST JSON at line {exc.lineno}: {exc.msg}"
        return None, status
    if not isinstance(data, dict) or data.get("schema") != "trit-ast-v1":
        status["available"] = False
        status["error"] = "AST dump did not use schema trit-ast-v1"
        return None, status
    return data, status


def ast_source_path(source_file: str) -> Path:
    path = Path(source_file)
    if path.is_absolute():
        return path
    return REPO_ROOT / path


def ast_line(span: Any) -> int:
    if isinstance(span, dict):
        try:
            return max(1, int(span.get("line", 1)))
        except (TypeError, ValueError):
            return 1
    return 1


def ast_source_location(span: Any) -> str:
    return f"L{ast_line(span)}"


def ast_signature(fn: dict[str, Any]) -> str:
    width_params = fn.get("width_params", [])
    width = ""
    if isinstance(width_params, list) and width_params:
        width = "<" + ", ".join(f"{param}: TritWidth" for param in width_params if isinstance(param, str)) + ">"
    params = fn.get("params", [])
    rendered_params = []
    if isinstance(params, list):
        for param in params:
            if not isinstance(param, dict):
                continue
            rendered_params.append(f"{param.get('name', '?')}: {param.get('type', 'unknown')}")
    return_type = str(fn.get("return_type", "void"))
    return f"fn {fn.get('name', '?')}{width}({', '.join(rendered_params)}) -> {return_type}"


def extract_trit_graph_from_ast(ast_data: dict[str, Any], files: list[Path]) -> dict[str, Any]:
    nodes_by_id: dict[str, dict[str, Any]] = {}
    edges: list[dict[str, Any]] = []
    symbol_by_name: dict[str, list[str]] = {}
    pending_calls: list[dict[str, Any]] = []
    modules = ast_data.get("modules", [])
    if not isinstance(modules, list):
        modules = []

    def add_node(node: dict[str, Any]) -> None:
        node.setdefault("_origin", "trit-adapter")
        node.setdefault("extractor", "ast")
        nodes_by_id.setdefault(node["id"], node)

    def add_edge(edge: dict[str, Any]) -> None:
        edges.append(edge)

    for module in modules:
        if not isinstance(module, dict):
            continue
        source_file = str(module.get("source_file", ""))
        path = ast_source_path(source_file)
        rel_path = rel(path)
        file_id = trit_file_node_id(path)
        add_node(
            {
                "id": file_id,
                "label": path.name,
                "file_type": "code",
                "source_file": rel_path,
                "source_location": "L1",
                "kind": "trit_file",
            }
        )

        for imported in module.get("imports", []):
            if not isinstance(imported, dict):
                continue
            name = str(imported.get("name", ""))
            if not name:
                continue
            node_id = "trit_import_" + graph_slug(name)
            add_node(
                {
                    "id": node_id,
                    "label": name,
                    "file_type": "code",
                    "source_file": "",
                    "source_location": "",
                    "kind": "trit_import",
                }
            )
            add_edge(
                {
                    "source": file_id,
                    "target": node_id,
                    "relation": "imports",
                    "confidence": "EXTRACTED",
                    "source_file": rel_path,
                    "source_location": ast_source_location(imported.get("span")),
                    "weight": 1.0,
                    "context": "trit_import",
                }
            )

        for struct in module.get("structs", []):
            if not isinstance(struct, dict):
                continue
            name = str(struct.get("name", ""))
            if not name:
                continue
            node_id = trit_symbol_node_id(path, name, "struct")
            add_node(
                {
                    "id": node_id,
                    "label": name,
                    "file_type": "code",
                    "source_file": rel_path,
                    "source_location": ast_source_location(struct.get("span")),
                    "kind": "trit_struct",
                    "fields": struct.get("fields", []),
                }
            )
            add_edge(
                {
                    "source": file_id,
                    "target": node_id,
                    "relation": "contains",
                    "confidence": "EXTRACTED",
                    "source_file": rel_path,
                    "source_location": ast_source_location(struct.get("span")),
                    "weight": 1.0,
                    "context": "trit_struct",
                }
            )

        for const in module.get("consts", []):
            if not isinstance(const, dict):
                continue
            name = str(const.get("name", ""))
            if not name:
                continue
            node_id = trit_symbol_node_id(path, name, "const")
            add_node(
                {
                    "id": node_id,
                    "label": name,
                    "file_type": "code",
                    "source_file": rel_path,
                    "source_location": ast_source_location(const.get("span")),
                    "kind": "trit_const",
                    "value_type": const.get("type", ""),
                    "scope": "file",
                }
            )
            add_edge(
                {
                    "source": file_id,
                    "target": node_id,
                    "relation": "contains",
                    "confidence": "EXTRACTED",
                    "source_file": rel_path,
                    "source_location": ast_source_location(const.get("span")),
                    "weight": 1.0,
                    "context": "trit_const",
                }
            )

        for fn in module.get("functions", []):
            if not isinstance(fn, dict):
                continue
            name = str(fn.get("name", ""))
            if not name:
                continue
            node_id = trit_symbol_node_id(path, name, "fn")
            source_location = ast_source_location(fn.get("span"))
            add_node(
                {
                    "id": node_id,
                    "label": f"{name}()",
                    "file_type": "code",
                    "source_file": rel_path,
                    "source_location": source_location,
                    "kind": "trit_function",
                    "signature": ast_signature(fn),
                    "parameters": fn.get("params", []),
                    "return_type": fn.get("return_type", ""),
                    "width_params": fn.get("width_params", []),
                }
            )
            add_edge(
                {
                    "source": file_id,
                    "target": node_id,
                    "relation": "contains",
                    "confidence": "EXTRACTED",
                    "source_file": rel_path,
                    "source_location": source_location,
                    "weight": 1.0,
                    "context": "trit_function",
                }
            )
            symbol_by_name.setdefault(name, []).append(node_id)

            for local_const in fn.get("local_consts", []):
                if not isinstance(local_const, dict):
                    continue
                const_name = str(local_const.get("name", ""))
                if not const_name:
                    continue
                const_line = ast_line(local_const.get("span"))
                const_id = trit_symbol_node_id(path, f"{name}_{const_name}_L{const_line}", "const")
                add_node(
                    {
                        "id": const_id,
                        "label": const_name,
                        "file_type": "code",
                        "source_file": rel_path,
                        "source_location": f"L{const_line}",
                        "kind": "trit_const",
                        "value_type": local_const.get("type", ""),
                        "scope": name,
                    }
                )
                add_edge(
                    {
                        "source": node_id,
                        "target": const_id,
                        "relation": "contains",
                        "confidence": "EXTRACTED",
                        "source_file": rel_path,
                        "source_location": f"L{const_line}",
                        "weight": 1.0,
                        "context": "trit_const",
                    }
                )

            for call in fn.get("calls", []):
                if not isinstance(call, dict):
                    continue
                call_name = str(call.get("name", ""))
                if not call_name or call_name == name:
                    continue
                pending_calls.append(
                    {
                        "source": node_id,
                        "name": call_name,
                        "source_file": rel_path,
                        "source_location": ast_source_location(call.get("span")),
                    }
                )

    external_nodes: dict[str, dict[str, Any]] = {}
    seen_edges: set[tuple[str, str, str, str]] = set()
    for call in pending_calls:
        targets = symbol_by_name.get(call["name"], [])
        if not targets and call["name"].startswith("sys_"):
            target = "trit_external_" + graph_slug(call["name"])
            external_nodes.setdefault(
                target,
                {
                    "id": target,
                    "label": f"{call['name']}()",
                    "file_type": "code",
                    "source_file": "",
                    "source_location": "",
                    "_origin": "trit-adapter",
                    "extractor": "ast",
                    "kind": "trit_external_syscall",
                },
            )
            targets = [target]
        for target in targets:
            key = (call["source"], target, call["source_file"], call["source_location"])
            if key in seen_edges:
                continue
            seen_edges.add(key)
            add_edge(
                {
                    "source": call["source"],
                    "target": target,
                    "relation": "calls",
                    "confidence": "EXTRACTED",
                    "source_file": call["source_file"],
                    "source_location": call["source_location"],
                    "weight": 1.0,
                    "context": "trit_call",
                }
            )

    for node in external_nodes.values():
        add_node(node)

    diagnostics = [
        diagnostic
        for module in modules
        if isinstance(module, dict)
        for diagnostic in module.get("diagnostics", [])
        if isinstance(diagnostic, dict)
    ]
    nodes = list(nodes_by_id.values())
    return {
        "extractor": "ast",
        "nodes": nodes,
        "edges": edges,
        "files": len(files),
        "modules": len(modules),
        "functions": sum(1 for node in nodes if node.get("kind") == "trit_function"),
        "constants": sum(1 for node in nodes if node.get("kind") == "trit_const"),
        "structs": sum(1 for node in nodes if node.get("kind") == "trit_struct"),
        "imports": sum(1 for edge in edges if edge.get("relation") == "imports"),
        "external_syscalls": len(external_nodes),
        "call_edges": sum(1 for edge in edges if edge.get("relation") == "calls"),
        "parse_diagnostics": len(diagnostics),
        "parse_errors": sum(1 for diagnostic in diagnostics if diagnostic.get("severity") == "error"),
    }


def extract_trit_graph(prefer_ast: bool = True) -> dict[str, Any]:
    files = trit_source_files()
    ast_status: dict[str, Any] = {"available": False}
    if prefer_ast:
        ast_data, ast_status = load_trit_ast_modules(files)
        if ast_data:
            graph = extract_trit_graph_from_ast(ast_data, files)
            graph["ast_dump"] = ast_status
            if graph["functions"] > 0:
                return graph
    graph = extract_trit_graph_regex()
    graph["ast_dump"] = ast_status
    return graph


def augment_graph_with_trit(graph_path: Path) -> dict[str, Any]:
    graph_path.parent.mkdir(parents=True, exist_ok=True)
    if graph_path.exists():
        graph, error = read_json_file_any(graph_path)
        if error:
            raise RuntimeError(f"{rel(graph_path)}: {error}")
        if not isinstance(graph, dict):
            raise RuntimeError(f"{rel(graph_path)}: graph must be a JSON object")
    else:
        graph = {"nodes": [], "edges": [], "hyperedges": [], "input_tokens": 0, "output_tokens": 0}

    graph["nodes"] = [
        node for node in graph.get("nodes", [])
        if not (isinstance(node, dict) and node.get("_origin") == "trit-adapter")
    ]
    edge_keys = [key for key in ("edges", "links") if isinstance(graph.get(key), list)]
    if not edge_keys:
        graph["edges"] = []
        edge_keys = ["edges"]
    for key in edge_keys:
        graph[key] = [
            edge for edge in graph.get(key, [])
            if not (
                isinstance(edge, dict)
                and edge.get("confidence") == "EXTRACTED"
                and str(edge.get("context", "")).startswith("trit_")
            )
        ]
    edge_key = "links" if "links" in graph else "edges"
    graph.setdefault("hyperedges", [])
    graph.setdefault("input_tokens", 0)
    graph.setdefault("output_tokens", 0)

    trit_graph = extract_trit_graph()
    existing_node_ids = {node.get("id") for node in graph["nodes"] if isinstance(node, dict)}
    added_nodes = []
    for node in trit_graph["nodes"]:
        if node["id"] in existing_node_ids:
            continue
        existing_node_ids.add(node["id"])
        added_nodes.append(node)
    graph["nodes"].extend(added_nodes)

    existing_edges = {
        (
            edge.get("source"),
            edge.get("target"),
            edge.get("relation"),
            edge.get("source_file"),
            edge.get("source_location"),
            edge.get("context"),
        )
        for edge in graph.get(edge_key, [])
        if isinstance(edge, dict)
    }
    added_edges = []
    for edge in trit_graph["edges"]:
        key = (
            edge.get("source"),
            edge.get("target"),
            edge.get("relation"),
            edge.get("source_file"),
            edge.get("source_location"),
            edge.get("context"),
        )
        if key in existing_edges:
            continue
        existing_edges.add(key)
        added_edges.append(edge)
    graph[edge_key].extend(added_edges)
    graph_path.write_text(canonical_json(graph), encoding="utf-8")

    summary = {
        "ok": True,
        "graph": str(graph_path),
        "extractor": trit_graph.get("extractor", "unknown"),
        "files": trit_graph["files"],
        "edge_key": edge_key,
        "nodes_added": len(added_nodes),
        "edges_added": len(added_edges),
        "functions": trit_graph["functions"],
        "constants": trit_graph["constants"],
        "structs": trit_graph.get("structs", 0),
        "imports": trit_graph.get("imports", 0),
        "external_syscalls": trit_graph["external_syscalls"],
        "call_edges": trit_graph["call_edges"],
        "parse_diagnostics": trit_graph.get("parse_diagnostics", 0),
        "parse_errors": trit_graph.get("parse_errors", 0),
        "ast_dump": trit_graph.get("ast_dump", {}),
    }
    (graph_path.parent / "trit-symbols.json").write_text(canonical_json(summary), encoding="utf-8")
    return summary


def knowledge_status_report() -> dict[str, Any]:
    issues: list[dict[str, str]] = []
    warnings: list[dict[str, str]] = []
    required = []
    for name in OBSIDIAN_REQUIRED_FILES:
        path = DOCS_DIR / name
        exists = path.exists()
        required.append({"path": rel(path), "exists": exists})
        if not exists:
            issues.append({"severity": "error", "message": f"missing {rel(path)}"})

    graphify_ignore = REPO_ROOT / ".graphifyignore"
    if not graphify_ignore.exists():
        issues.append({"severity": "error", "message": "missing .graphifyignore"})

    link_issues = check_docs_markdown_links()
    for item in link_issues:
        issues.append(
            {
                "severity": "error",
                "message": f"{item['file']} has broken {item['kind']} link to {item['target']}",
            }
        )

    canvas = canvas_status()
    if not canvas.get("ok", False):
        issues.append({"severity": "error", "message": f"canvas invalid: {canvas.get('error', canvas)}"})

    graphify_path = find_tool_executable("graphify")
    if not graphify_path:
        warnings.append({"severity": "warning", "message": "graphify CLI is not installed; knowledge graph runs are optional"})

    report = {
        "ok": not issues,
        "docs_dir": str(DOCS_DIR),
        "obsidian": {
            "vault": str(DOCS_DIR),
            "config_dir": str(OBSIDIAN_DIR),
            "required_files": required,
            "canvas": canvas,
        },
        "graphify": {
            "cli": graphify_path,
            "ignore": str(graphify_ignore),
            "output_dir": str(GRAPHIFY_OUT_DIR),
            "archive_dir": str(GRAPHIFY_ARCHIVE_DIR / "runs"),
        },
        "markdown": {
            "files": len(docs_markdown_files()),
            "broken_links": link_issues,
        },
        "issues": issues,
        "warnings": warnings,
    }
    return report


def setup_status_report() -> dict[str, Any]:
    mismatches = []
    for path, expected in obsidian_file_specs().items():
        if not path.exists():
            mismatches.append({"path": rel(path), "state": "missing"})
        elif path.read_text(encoding="utf-8") != expected:
            actual_hash = hashlib.sha256(path.read_bytes()).hexdigest()
            expected_hash = hashlib.sha256(expected.encode("utf-8")).hexdigest()
            mismatches.append(
                {
                    "path": rel(path),
                    "state": "outdated",
                    "actual_sha256": actual_hash,
                    "expected_sha256": expected_hash,
                }
            )
    return {"ok": not mismatches, "mismatches": mismatches}


def cmd_knowledge_setup(args: argparse.Namespace) -> int:
    if args.check:
        report = setup_status_report()
        if args.json:
            print_json(report)
        elif report["ok"]:
            print("knowledge setup is current")
        else:
            print("knowledge setup is not current")
            for mismatch in report["mismatches"]:
                print(f"- {mismatch['path']}: {mismatch['state']}")
        return 0 if report["ok"] else 1

    changed = []
    for path, expected in obsidian_file_specs().items():
        if write_text_if_changed(path, expected):
            changed.append(rel(path))
    report = {"ok": True, "changed": changed}
    if args.json:
        print_json(report)
    else:
        if changed:
            print("updated knowledge integration files:")
            for path in changed:
                print(f"- {path}")
        else:
            print("knowledge integration files are already current")
    return 0


def cmd_knowledge_status(args: argparse.Namespace) -> int:
    report = knowledge_status_report()
    if args.json:
        print_json(report)
    else:
        print("Trit knowledge status")
        text_status("docs vault", DOCS_DIR.exists(), str(DOCS_DIR))
        text_status("Obsidian config", OBSIDIAN_DIR.exists(), str(OBSIDIAN_DIR))
        text_status("canvas", bool(report["obsidian"]["canvas"].get("ok")), str(OBSIDIAN_CANVAS))
        text_status("Markdown links", not report["markdown"]["broken_links"], f"{report['markdown']['files']} files")
        graphify_cli = report["graphify"]["cli"]
        text_status("Graphify CLI", bool(graphify_cli), graphify_cli or "optional")
        if report["issues"]:
            print("\nIssues:")
            for issue in report["issues"]:
                print(f"- {issue['message']}")
        if report["warnings"]:
            print("\nWarnings:")
            for warning in report["warnings"]:
                print(f"- {warning['message']}")
    return 0 if report["ok"] else 1


def cmd_knowledge_canvas(args: argparse.Namespace) -> int:
    expected = canonical_json(build_obsidian_canvas())
    if args.check:
        ok = OBSIDIAN_CANVAS.exists() and OBSIDIAN_CANVAS.read_text(encoding="utf-8") == expected
        report = {"ok": ok, "path": str(OBSIDIAN_CANVAS)}
        if args.json:
            print_json(report)
        else:
            print("canvas is current" if ok else "canvas is not current")
        return 0 if ok else 1
    changed = write_text_if_changed(OBSIDIAN_CANVAS, expected)
    if args.json:
        print_json({"ok": True, "path": str(OBSIDIAN_CANVAS), "changed": changed})
    else:
        print(("updated " if changed else "current ") + rel(OBSIDIAN_CANVAS))
    return 0


def current_git_short_sha() -> str:
    result = run_command(["git", "rev-parse", "--short", "HEAD"], capture=True, timeout=10)
    if result["returncode"] == 0 and result["stdout"].strip():
        return result["stdout"].strip()
    return "nogit"


def archive_graphify_artifacts(command_result: dict[str, Any]) -> dict[str, Any]:
    stamp = _dt.datetime.now(_dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_dir = GRAPHIFY_ARCHIVE_DIR / "runs" / f"{stamp}-{current_git_short_sha()}"
    run_dir.mkdir(parents=True, exist_ok=True)
    copied = []
    if GRAPHIFY_OUT_DIR.exists():
        candidates = [
            path
            for path in GRAPHIFY_OUT_DIR.rglob("*")
            if path.is_file() and path.suffix.lower() in {".md", ".json", ".jsonl", ".txt"}
            and "cache" not in path.relative_to(GRAPHIFY_OUT_DIR).parts
        ]
        for path in candidates[:40]:
            relative = path.relative_to(GRAPHIFY_OUT_DIR)
            target = run_dir / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            copied.append(rel(target))
    summary = {
        "created_at": stamp,
        "command": command_result.get("command", []),
        "returncode": command_result.get("returncode"),
        "graphify_out": str(GRAPHIFY_OUT_DIR),
        "copied": copied,
    }
    (run_dir / "run.json").write_text(canonical_json(summary), encoding="utf-8")
    return {"run_dir": str(run_dir), "copied": copied}


def cmd_knowledge_graph(args: argparse.Namespace) -> int:
    graphify = find_tool_executable("graphify")
    command = [graphify or "graphify", "update", ".", "--force", "--no-cluster"]
    if args.dry_run:
        report = {
            "ok": bool(graphify),
            "command": command,
            "cwd": str(REPO_ROOT),
            "graphify": graphify,
            "trit_adapter": not args.no_trit,
        }
        if args.json:
            print_json(report)
        else:
            print(" ".join(command))
        return 0 if graphify else 2
    if not graphify:
        print("graphify CLI is not installed. Install Graphify, then rerun `python tools/trit_tool.py knowledge graph`.", file=sys.stderr)
        return 2
    result = run_command(command, cwd=REPO_ROOT, capture=True, timeout=args.timeout)
    if result["stdout"].strip():
        print(result["stdout"], end="" if result["stdout"].endswith("\n") else "\n")
    if result["stderr"].strip():
        print(result["stderr"], file=sys.stderr)
    archive = None
    trit = None
    if result["returncode"] == 0 and not args.no_trit:
        trit = augment_graph_with_trit(GRAPHIFY_OUT_DIR / "graph.json")
        print(
            f"augmented Graphify graph with Trit sources via {trit['extractor']}: "
            f"{trit['files']} files, {trit['functions']} functions, "
            f"{trit['constants']} constants, {trit['call_edges']} call edges"
        )
    if result["returncode"] == 0 and not args.no_archive:
        archive = archive_graphify_artifacts(result)
        print(f"archived Graphify artifacts to {archive['run_dir']}")
    if args.json:
        print_json({"ok": result["returncode"] == 0, "run": result, "trit": trit, "archive": archive})
    return int(result["returncode"])


def _plan_error(code: str, message: str, **details: Any) -> dict[str, Any]:
    error = {"code": code, "message": message}
    error.update(details)
    return error


def _normalise_plan_status(value: Any) -> str:
    return str(value or "").strip().strip("`").strip().lower()


def _parse_plan_dependencies(value: Any) -> tuple[list[str], list[str]]:
    if value is None:
        return [], []
    text_value = str(value).strip().strip("`")
    if not text_value or text_value.lower() in {"none", "-", "—", "–"}:
        return [], []
    dependencies: list[str] = []
    invalid: list[str] = []
    tokens = [token.strip() for token in re.split(r"[,;]", text_value) if token.strip()]
    for token in tokens:
        range_match = re.fullmatch(r"(P\d+)\s*[-–—]\s*(P\d+)", token, flags=re.IGNORECASE)
        if range_match:
            first = int(range_match.group(1)[1:])
            last = int(range_match.group(2)[1:])
            if first > last:
                invalid.append(token)
                continue
            dependencies.extend(f"P{number:02d}" for number in range(first, last + 1))
            continue
        if re.fullmatch(r"P\d+", token, flags=re.IGNORECASE):
            dependencies.append(token.upper())
        else:
            invalid.append(token)
    return dependencies, invalid


def parse_treatcode_plan_index(index_path: Path = PLAN_INDEX_PATH) -> dict[str, Any]:
    """Parse the authoritative TreatCode plan index table."""

    errors: list[dict[str, Any]] = []
    rows: list[dict[str, Any]] = []
    try:
        text_value = index_path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return {
            "ok": False,
            "path": str(index_path),
            "rows": [],
            "errors": [_plan_error("missing_plan_index", f"plan index is missing: {index_path}")],
        }
    except OSError as exc:
        return {
            "ok": False,
            "path": str(index_path),
            "rows": [],
            "errors": [_plan_error("unreadable_plan_index", f"could not read plan index: {exc}")],
        }

    link_re = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")
    for line_number, line in enumerate(text_value.splitlines(), 1):
        if not line.lstrip().startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) < 5 or not re.fullmatch(r"P\d+", cells[0], flags=re.IGNORECASE):
            continue
        link = link_re.search(cells[1])
        if not link:
            errors.append(
                _plan_error(
                    "invalid_plan_index_row",
                    f"plan index row {line_number} has no plan link",
                    line=line_number,
                )
            )
            continue
        dependencies, invalid_dependencies = _parse_plan_dependencies(cells[2])
        if invalid_dependencies:
            errors.append(
                _plan_error(
                    "invalid_dependency_reference",
                    f"plan index row {line_number} has invalid dependencies: {', '.join(invalid_dependencies)}",
                    plan_id=cells[0].upper(),
                    line=line_number,
                )
            )
        rows.append(
            {
                "id": cells[0].upper(),
                "title": link.group(1).strip(),
                "path": link.group(2).strip(),
                "depends_on": dependencies,
                "status": _normalise_plan_status(cells[3]),
                "completion_evidence": cells[4].strip(),
                "line": line_number,
            }
        )
    if not rows:
        errors.append(_plan_error("empty_plan_index", f"no plan rows found in {index_path}"))
    return {"ok": not errors, "path": str(index_path), "rows": rows, "errors": errors}


def parse_treatcode_plan_document(plan_path: Path) -> dict[str, Any]:
    """Extract the machine-relevant sections from one plan document."""

    try:
        text_value = plan_path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return {
            "ok": False,
            "path": str(plan_path),
            "errors": [_plan_error("missing_plan_document", f"plan document is missing: {plan_path}")],
        }
    except OSError as exc:
        return {
            "ok": False,
            "path": str(plan_path),
            "errors": [_plan_error("unreadable_plan_document", f"could not read plan document: {exc}")],
        }

    def metadata(pattern: str, default: Any = None) -> Any:
        match = re.search(pattern, text_value, flags=re.IGNORECASE | re.MULTILINE)
        return match.group(1).strip() if match else default

    version_value = metadata(r"^\s*-\s*\*\*Version:\*\*\s*`?([^`\s]+)", "")
    try:
        version: int | None = int(version_value)
    except (TypeError, ValueError):
        version = None
    depends_text = metadata(r"^\s*-\s*\*\*Depends on:\*\*\s*(.+)$", "")
    dependencies, invalid_dependencies = _parse_plan_dependencies(depends_text)

    verification_commands: list[str] = []
    verification_match = re.search(
        r"^##\s+Verification\s*$([\s\S]*?)(?=^##\s+|\Z)",
        text_value,
        flags=re.IGNORECASE | re.MULTILINE,
    )
    if verification_match:
        fence_match = re.search(r"```[^\r\n]*\r?\n([\s\S]*?)```", verification_match.group(1))
        if fence_match:
            verification_commands = [
                line.strip()
                for line in fence_match.group(1).splitlines()
                if line.strip() and not line.strip().startswith("#")
            ]

    evidence: list[str] = []
    evidence_match = re.search(
        r"^##\s+Required Evidence\s*$([\s\S]*?)(?=^##\s+|\Z)",
        text_value,
        flags=re.IGNORECASE | re.MULTILINE,
    )
    if evidence_match:
        current: str | None = None
        for line in evidence_match.group(1).splitlines():
            bullet = re.match(r"^\s*-\s+(.+?)\s*$", line)
            if bullet:
                if current:
                    evidence.append(current)
                current = bullet.group(1).strip()
            elif current and line.strip():
                current += " " + line.strip()
        if current:
            evidence.append(current)

    errors: list[dict[str, Any]] = []
    if invalid_dependencies:
        errors.append(
            _plan_error(
                "invalid_dependency_reference",
                f"{rel(plan_path)} has invalid dependencies: {', '.join(invalid_dependencies)}",
            )
        )
    return {
        "ok": not errors,
        "path": str(plan_path),
        "plan_id": metadata(r"^\s*-\s*\*\*Plan ID:\*\*\s*`?([^`\s]+)", "").upper(),
        "version": version,
        "status": _normalise_plan_status(metadata(r"^\s*-\s*\*\*Status:\*\*\s*`?([^`\s]+)", "")),
        "depends_on": dependencies,
        "scope_owner": metadata(r"^\s*-\s*\*\*Scope owner:\*\*\s*(.+)$", ""),
        "verification_commands": verification_commands,
        "required_evidence": evidence,
        "errors": errors,
    }


def _plan_command_text(command: Any) -> str:
    if isinstance(command, str):
        return command.strip()
    if isinstance(command, dict):
        display = command.get("display")
        if isinstance(display, str) and display.strip():
            return display.strip()
        return _plan_command_text(command.get("command", ""))
    if isinstance(command, list):
        return " ".join(str(item) for item in command).strip()
    return ""


def _plan_evidence_reference(evidence: Any) -> str:
    if isinstance(evidence, str):
        return evidence.strip()
    if not isinstance(evidence, dict):
        return ""
    for key in ("path", "uri", "artifact", "reference", "content_hash"):
        value = evidence.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    roles = evidence.get("approval_roles")
    if isinstance(roles, list) and any(str(role).strip() for role in roles):
        return "approval_roles:" + ",".join(str(role).strip() for role in roles if str(role).strip())
    return ""


def _plan_resolve_path(value: str | Path, repo_root: Path) -> Path:
    path = Path(value)
    return path.resolve() if path.is_absolute() else (repo_root / path).resolve()


def _plan_dependency_cycles(entries: dict[str, dict[str, Any]]) -> list[list[str]]:
    cycles: list[list[str]] = []
    visiting: list[str] = []
    visited: set[str] = set()

    def visit(plan_id: str) -> None:
        if plan_id in visiting:
            start = visiting.index(plan_id)
            cycle = visiting[start:] + [plan_id]
            if cycle not in cycles:
                cycles.append(cycle)
            return
        if plan_id in visited or plan_id not in entries:
            return
        visiting.append(plan_id)
        for dependency in entries[plan_id].get("depends_on", []):
            visit(str(dependency).upper())
        visiting.pop()
        visited.add(plan_id)

    for plan_id in entries:
        visit(plan_id)
    return cycles


def validate_treatcode_plan_manifest(
    manifest: Any,
    *,
    repo_root: Path = REPO_ROOT,
    manifest_path: Path | None = None,
    index_path: Path | None = None,
) -> dict[str, Any]:
    """Validate the plan manifest and its relationship to PLAN_INDEX.md."""

    errors: list[dict[str, Any]] = []
    if not isinstance(manifest, dict):
        return {
            "ok": False,
            "errors": [_plan_error("invalid_manifest", "plan manifest must be a JSON object")],
            "plan_ids": [],
            "index_plan_ids": [],
        }

    if manifest.get("version") != 1:
        errors.append(_plan_error("invalid_manifest_version", "plan manifest version must be 1"))
    if manifest.get("schema") != "trit.treatcode_plan_manifest.v1":
        errors.append(_plan_error("invalid_manifest_schema", "plan manifest schema must be trit.treatcode_plan_manifest.v1"))

    manifest_file = manifest_path or PLAN_MANIFEST_PATH
    schema_value = manifest.get("schema_file", "TREATCODE_PLAN_MANIFEST_SCHEMA.json")
    if not isinstance(schema_value, str) or not schema_value.strip():
        errors.append(_plan_error("missing_manifest_schema", "manifest schema_file is missing"))
    else:
        schema_path = _plan_resolve_path(schema_value, manifest_file.parent if manifest_path else repo_root)
        if not schema_path.exists():
            errors.append(_plan_error("missing_manifest_schema", f"manifest schema is missing: {schema_value}"))

    index_value = index_path
    if index_value is None:
        index_value = manifest.get("index", str(PLAN_INDEX_PATH.relative_to(repo_root)))
    if not isinstance(index_value, Path):
        index_value = _plan_resolve_path(str(index_value), manifest_file.parent if manifest_path else repo_root)
    index_report = parse_treatcode_plan_index(index_value)
    errors.extend(index_report.get("errors", []))
    index_rows = index_report.get("rows", [])
    index_ids = [str(row.get("id", "")).upper() for row in index_rows]
    index_id_counts: dict[str, int] = {}
    for plan_id in index_ids:
        index_id_counts[plan_id] = index_id_counts.get(plan_id, 0) + 1
        if index_id_counts[plan_id] > 1:
            errors.append(_plan_error("duplicate_plan_id", f"plan index contains duplicate plan ID {plan_id}", plan_id=plan_id))

    plans = manifest.get("plans")
    if not isinstance(plans, list):
        errors.append(_plan_error("missing_plans", "manifest plans must be a list"))
        plans = []

    entries: dict[str, dict[str, Any]] = {}
    manifest_ids: list[str] = []
    for position, entry in enumerate(plans):
        if not isinstance(entry, dict):
            errors.append(_plan_error("invalid_plan_entry", f"manifest plan entry {position} must be an object", position=position))
            continue
        plan_id = str(entry.get("id", "")).strip().upper()
        manifest_ids.append(plan_id)
        if not plan_id or not re.fullmatch(r"P\d+", plan_id):
            errors.append(_plan_error("invalid_plan_id", f"manifest plan entry {position} has an invalid ID", position=position))
            continue
        if plan_id in entries:
            errors.append(_plan_error("duplicate_plan_id", f"manifest contains duplicate plan ID {plan_id}", plan_id=plan_id))
        entries[plan_id] = entry

        status = _normalise_plan_status(entry.get("status"))
        if status not in PLAN_STATUSES:
            errors.append(_plan_error("invalid_status", f"{plan_id} has invalid status {entry.get('status')!r}", plan_id=plan_id))
        if not isinstance(entry.get("version"), int) or entry.get("version") < 1:
            errors.append(_plan_error("invalid_plan_version", f"{plan_id} has an invalid version", plan_id=plan_id))
        if "depends_on" not in entry or not isinstance(entry.get("depends_on"), list):
            errors.append(_plan_error("missing_dependencies", f"{plan_id} must declare depends_on as a list", plan_id=plan_id))
        dependencies = [str(item).strip().upper() for item in entry.get("depends_on", [])] if isinstance(entry.get("depends_on"), list) else []
        if len(dependencies) != len(set(dependencies)):
            errors.append(_plan_error("duplicate_dependency", f"{plan_id} declares a dependency more than once", plan_id=plan_id))
        entry["depends_on"] = dependencies
        if not isinstance(entry.get("plan_file"), str) or not entry.get("plan_file", "").strip():
            errors.append(_plan_error("missing_plan_file", f"{plan_id} must reference its plan_file", plan_id=plan_id))

        commands = entry.get("verification_commands")
        if not isinstance(commands, list) or not commands:
            errors.append(_plan_error("absent_verification_commands", f"{plan_id} has no verification commands", plan_id=plan_id))
        else:
            command_ids: set[str] = set()
            for command_position, command in enumerate(commands):
                command_text = _plan_command_text(command)
                if not command_text:
                    errors.append(
                        _plan_error(
                            "absent_verification_command",
                            f"{plan_id} verification command {command_position} is empty",
                            plan_id=plan_id,
                        )
                    )
                if isinstance(command, dict):
                    command_id = str(command.get("id", "")).strip()
                    if not command_id:
                        errors.append(_plan_error("missing_verification_command_id", f"{plan_id} has an unnamed verification command", plan_id=plan_id))
                    elif command_id in command_ids:
                        errors.append(_plan_error("duplicate_verification_command_id", f"{plan_id} repeats command ID {command_id}", plan_id=plan_id))
                    command_ids.add(command_id)

        evidence = entry.get("evidence")
        if not isinstance(evidence, list) or not evidence:
            errors.append(_plan_error("absent_evidence_references", f"{plan_id} has no evidence references", plan_id=plan_id))
        else:
            for evidence_position, reference in enumerate(evidence):
                if not _plan_evidence_reference(reference):
                    errors.append(
                        _plan_error(
                            "absent_evidence_reference",
                            f"{plan_id} evidence reference {evidence_position} is empty",
                            plan_id=plan_id,
                        )
                    )

        gates = entry.get("gates")
        if not isinstance(gates, list) or not gates:
            errors.append(_plan_error("missing_gates", f"{plan_id} has no gates", plan_id=plan_id))
        else:
            human_gate = False
            for gate_position, gate in enumerate(gates):
                if not isinstance(gate, dict) or str(gate.get("type", "")).lower() not in PLAN_GATE_TYPES:
                    errors.append(_plan_error("invalid_gate", f"{plan_id} has an invalid gate at position {gate_position}", plan_id=plan_id))
                elif str(gate.get("type", "")).lower() == "human" and gate.get("required", True):
                    human_gate = True
            approvals = entry.get("required_approvals")
            if human_gate and (not isinstance(approvals, list) or not any(str(item).strip() for item in approvals)):
                errors.append(_plan_error("missing_required_approvals", f"{plan_id} has a required human gate but no required_approvals", plan_id=plan_id))

        if status == "complete" and not isinstance(entry.get("completion_record"), dict):
            errors.append(_plan_error("missing_completion_record", f"{plan_id} is complete but has no completion_record", plan_id=plan_id))

    manifest_id_set = set(manifest_ids)
    index_id_set = set(index_ids)
    for plan_id in sorted(index_id_set - manifest_id_set):
        errors.append(_plan_error("missing_plan_entry", f"plan index entry {plan_id} is missing from manifest", plan_id=plan_id))
    for plan_id in sorted(manifest_id_set - index_id_set):
        errors.append(_plan_error("extra_plan_entry", f"manifest entry {plan_id} is missing from plan index", plan_id=plan_id))

    for row in index_rows:
        plan_id = str(row.get("id", "")).upper()
        entry = entries.get(plan_id)
        if not entry:
            continue
        if _normalise_plan_status(entry.get("status")) != row.get("status"):
            errors.append(_plan_error("status_mismatch", f"{plan_id} status differs between manifest and plan index", plan_id=plan_id))
        manifest_dependencies = [str(item).upper() for item in entry.get("depends_on", [])]
        if manifest_dependencies != row.get("depends_on", []):
            errors.append(_plan_error("dependency_mismatch", f"{plan_id} dependencies differ between manifest and plan index", plan_id=plan_id))
        expected_path = (index_value.parent / str(row.get("path", ""))).resolve()
        actual_path = _plan_resolve_path(str(entry.get("plan_file", "")), repo_root)
        if expected_path != actual_path:
            errors.append(_plan_error("plan_file_mismatch", f"{plan_id} plan_file differs from plan index", plan_id=plan_id))
        if expected_path.exists():
            document = parse_treatcode_plan_document(expected_path)
            errors.extend(document.get("errors", []))
            if document.get("plan_id") and document.get("plan_id") != plan_id:
                errors.append(_plan_error("plan_document_id_mismatch", f"{plan_id} document metadata has a different ID", plan_id=plan_id))
            if document.get("version") is not None and document.get("version") != entry.get("version"):
                errors.append(_plan_error("plan_document_version_mismatch", f"{plan_id} document version differs from manifest", plan_id=plan_id))
            if document.get("status") and document.get("status") != _normalise_plan_status(entry.get("status")):
                errors.append(_plan_error("plan_document_status_mismatch", f"{plan_id} document status differs from manifest", plan_id=plan_id))
            if document.get("depends_on", []) != manifest_dependencies:
                errors.append(_plan_error("plan_document_dependency_mismatch", f"{plan_id} document dependencies differ from manifest", plan_id=plan_id))
            manifest_commands = [_plan_command_text(command) for command in entry.get("verification_commands", [])]
            if document.get("verification_commands", []) != manifest_commands:
                errors.append(_plan_error("verification_command_mismatch", f"{plan_id} verification commands differ from plan document", plan_id=plan_id))

    for plan_id, entry in entries.items():
        for dependency in entry.get("depends_on", []):
            if dependency not in entries:
                errors.append(_plan_error("missing_dependency", f"{plan_id} depends on unknown plan {dependency}", plan_id=plan_id, dependency=dependency))
    for cycle in _plan_dependency_cycles(entries):
        errors.append(_plan_error("dependency_cycle", f"dependency cycle detected: {' -> '.join(cycle)}", cycle=cycle))

    return {
        "ok": not errors,
        "errors": errors,
        "manifest": str(manifest_file),
        "index": str(index_value),
        "plan_ids": manifest_ids,
        "index_plan_ids": index_ids,
        "plan_count": len(manifest_ids),
    }


def load_treatcode_plan_manifest(path: Path = PLAN_MANIFEST_PATH) -> tuple[Any | None, str | None]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle), None
    except FileNotFoundError:
        return None, "file is missing"
    except json.JSONDecodeError as exc:
        return None, f"invalid JSON at line {exc.lineno}: {exc.msg}"


def _plan_current_commit() -> str:
    result = run_command(["git", "rev-parse", "HEAD"], capture=True, timeout=10)
    if result["returncode"] == 0 and result["stdout"].strip():
        return result["stdout"].strip()
    return "nogit"


def _plan_environment() -> dict[str, Any]:
    environment = {
        "platform": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "python_implementation": platform.python_implementation(),
        "tool": "tools/trit_tool.py",
    }
    environment["fingerprint"] = hashlib.sha256(canonical_json(environment).encode("utf-8")).hexdigest()
    return environment


def _resolve_plan_executable(argv: list[str]) -> list[str]:
    """Resolve extensionless commands to executable shims on Windows.

    Plan manifests intentionally use portable commands such as ``npm`` and
    ``python``.  PowerShell's command lookup prefers ``npm.ps1`` on some
    Windows installations, but ``subprocess`` cannot execute that script
    directly.  Prefer the corresponding ``.cmd``/``.exe`` shim and retain the
    portable command when no resolution is available.
    """

    if not argv or os.name != "nt":
        return argv
    executable = argv[0]
    if not executable or Path(executable).suffix or any(separator in executable for separator in ("/", "\\")):
        return argv
    for suffix in (".cmd", ".exe", ".bat"):
        candidate = f"{executable}{suffix}"
        if shutil.which(candidate):
            return [candidate, *argv[1:]]
    return argv


def _plan_command_argv(command: Any) -> list[str]:
    if isinstance(command, dict):
        command = command.get("command", "")
    if isinstance(command, list):
        argv = [str(item) for item in command]
        if argv and argv[0].lower().endswith(".ps1"):
            powershell = shutil.which("pwsh") or shutil.which("powershell") or "powershell"
            return [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", *argv]
        return _resolve_plan_executable(argv)
    text_value = str(command or "").strip()
    if not text_value:
        return []
    if text_value.lower().startswith("tools/") and text_value.lower().endswith(".cmd"):
        return ["cmd", "/c", text_value]
    try:
        argv = shlex.split(text_value, posix=True)
    except ValueError:
        return [text_value]
    if argv and argv[0].lower().endswith(".ps1"):
        powershell = shutil.which("pwsh") or shutil.which("powershell") or "powershell"
        return [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", *argv]
    return _resolve_plan_executable(argv)


def _plan_is_self_verification(command: Any, plan_id: str) -> bool:
    text_value = _plan_command_text(command)
    return bool(re.search(rf"\bwebsite\s+plan\s+verify\s+{re.escape(plan_id)}(?:\s|$)", text_value, flags=re.IGNORECASE))


def _plan_approval_entries(entry: dict[str, Any]) -> list[dict[str, Any]]:
    record = entry.get("completion_record")
    if not isinstance(record, dict):
        return []
    raw = record.get("human_approvals", [])
    if not isinstance(raw, list):
        return []
    approvals: list[dict[str, Any]] = []
    for approval in raw:
        if isinstance(approval, str):
            approvals.append({"role": approval})
        elif isinstance(approval, dict):
            approvals.append(approval)
    return approvals


def _plan_approval_status(entry: dict[str, Any]) -> dict[str, Any]:
    required = [str(item).strip() for item in entry.get("required_approvals", []) if str(item).strip()]
    actual = _plan_approval_entries(entry)
    missing: list[str] = []
    accepted: list[dict[str, Any]] = []
    accepted_decisions = {"approve", "approved", "accept", "accepted", "pass", "passed"}
    for role in required:
        found = None
        for approval in actual:
            if str(approval.get("role", "")).strip().casefold() != role.casefold():
                continue
            fields_present = all(str(approval.get(field, "")).strip() for field in ("reviewer", "decision", "date", "commit"))
            decision = str(approval.get("decision", "")).strip().casefold()
            if fields_present and decision in accepted_decisions:
                found = approval
                break
        if found is None:
            missing.append(role)
        else:
            accepted.append(found)
    return {"required": required, "accepted": accepted, "missing": missing, "ok": not missing}


def _plan_evidence_status(entry: dict[str, Any], output_dir: Path, result_path: Path) -> list[dict[str, Any]]:
    statuses: list[dict[str, Any]] = []
    for reference in entry.get("evidence", []):
        item = reference if isinstance(reference, dict) else {"description": str(reference)}
        path_value = item.get("path") if isinstance(item, dict) else None
        path = _plan_resolve_path(path_value, REPO_ROOT) if isinstance(path_value, str) and path_value.strip() else None
        is_result = path is not None and path.resolve() == result_path.resolve()
        exists = bool(path and path.exists()) or is_result
        digest = None
        if path and path.exists() and not is_result:
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
        artifact = item.get("artifact") if isinstance(item, dict) else None
        uri = item.get("uri") if isinstance(item, dict) else None
        approval_roles = item.get("approval_roles", []) if isinstance(item, dict) else []
        statuses.append(
            {
                "id": item.get("id", "") if isinstance(item, dict) else "",
                "description": item.get("description", "") if isinstance(item, dict) else str(reference),
                "reference": _plan_evidence_reference(reference),
                "path": str(path) if path else None,
                "exists": exists,
                "sha256": digest,
                "artifact": artifact,
                "uri": uri,
                "approval_roles": approval_roles,
                "self_reference": is_result,
            }
        )
    return statuses


def _plan_stable_result_view(value: Any) -> Any:
    """Return the reproducible portion of a verification result.

    Evidence paths are useful to humans but are not part of the verification
    claim, and command duration/timestamps are expected to vary between runs.
    Keeping the content hash over this projection makes same-commit reruns
    directly comparable without hiding the full diagnostic result.
    """

    volatile_keys = {
        "content_hash",
        "duration_seconds",
        "log",
        "output",
        "path",
        "result_sha256",
        "sha256",
        "stderr_sha256",
        "stdout_sha256",
        "verification_date",
    }
    if isinstance(value, dict):
        return {
            key: _plan_stable_result_view(item)
            for key, item in value.items()
            if key not in volatile_keys
        }
    if isinstance(value, list):
        return [_plan_stable_result_view(item) for item in value]
    return value


def verify_treatcode_plan(
    plan_id: str,
    *,
    manifest_path: Path = PLAN_MANIFEST_PATH,
    output_dir: Path | None = None,
    repo_root: Path = REPO_ROOT,
    index_path: Path | None = None,
    run_commands: bool = True,
    timeout: int = 300,
) -> dict[str, Any]:
    """Run one plan's declared commands and write its reproducible evidence."""

    normalized_id = str(plan_id).strip().upper()
    manifest, load_error = load_treatcode_plan_manifest(manifest_path)
    output_root = output_dir or (repo_root / "build" / "treatcode-plan-evidence" / normalized_id)
    output_root = output_root if output_root.is_absolute() else (repo_root / output_root)
    output_root.mkdir(parents=True, exist_ok=True)
    result_path = output_root / "result.json"
    started_at = _dt.datetime.now(_dt.timezone.utc)

    result: dict[str, Any] = {
        "schema": "trit.treatcode_plan_verification_result.v1",
        "plan": {"id": normalized_id, "version": None, "status": None},
        "complete": False,
        "verification_ok": False,
        "verified_commit": _plan_current_commit(),
        "dependencies": {},
        "verifier": {"tool": "tools/trit_tool.py", "command": f"website plan verify {normalized_id}"},
        "verification_commands": [],
        "environment": _plan_environment(),
        "evidence": [],
        "human_approvals": {"required": [], "accepted": [], "missing": [], "ok": False},
        "issues": [],
        "verification_date": started_at.isoformat().replace("+00:00", "Z"),
        "output": str(result_path),
    }

    if load_error:
        result["issues"].append(_plan_error("manifest_load_failed", f"could not load plan manifest: {load_error}"))
    else:
        validation = validate_treatcode_plan_manifest(
            manifest,
            repo_root=repo_root,
            manifest_path=manifest_path,
            index_path=index_path,
        )
        result["validation"] = validation
        if not validation["ok"]:
            result["issues"].extend(validation["errors"])

        entries = {str(item.get("id", "")).upper(): item for item in manifest.get("plans", []) if isinstance(item, dict)}
        entry = entries.get(normalized_id)
        if entry is None:
            result["issues"].append(_plan_error("unknown_plan_id", f"unknown plan ID {normalized_id}", plan_id=normalized_id))
        else:
            result["plan"] = {
                "id": normalized_id,
                "version": entry.get("version"),
                "status": _normalise_plan_status(entry.get("status")),
            }
            for dependency in entry.get("depends_on", []):
                dependency_entry = entries.get(str(dependency).upper())
                result["dependencies"][str(dependency).upper()] = {
                    "status": _normalise_plan_status(dependency_entry.get("status")) if dependency_entry else None,
                    "verified_commit": dependency_entry.get("completion_record", {}).get("verified_commit") if dependency_entry and isinstance(dependency_entry.get("completion_record"), dict) else None,
                    "complete": bool(dependency_entry and _normalise_plan_status(dependency_entry.get("status")) == "complete"),
                }
                if not dependency_entry or _normalise_plan_status(dependency_entry.get("status")) != "complete":
                    result["issues"].append(_plan_error("dependency_not_complete", f"dependency {dependency} is not complete", dependency=dependency))

            command_results: list[dict[str, Any]] = []
            commands_passed = True
            for position, command in enumerate(entry.get("verification_commands", [])):
                command_text = _plan_command_text(command)
                command_id = command.get("id", f"command_{position + 1}") if isinstance(command, dict) else f"command_{position + 1}"
                command_record: dict[str, Any] = {
                    "id": str(command_id),
                    "command": command_text,
                    "required": command.get("required", True) if isinstance(command, dict) else True,
                }
                if _plan_is_self_verification(command, normalized_id):
                    command_record.update({"status": "self_reference_skipped", "returncode": 0, "duration_seconds": 0.0, "stdout_sha256": "", "stderr_sha256": ""})
                elif not run_commands:
                    command_record.update({"status": "not_run", "returncode": 125, "duration_seconds": 0.0, "stdout_sha256": "", "stderr_sha256": ""})
                    if command_record["required"]:
                        commands_passed = False
                else:
                    argv = _plan_command_argv(command)
                    execution = run_command(argv, cwd=repo_root, capture=True, timeout=timeout)
                    stdout = execution.get("stdout", "") or ""
                    stderr = execution.get("stderr", "") or ""
                    command_record.update(
                        {
                            "status": "passed" if execution["returncode"] == 0 else "failed",
                            "returncode": execution["returncode"],
                            "duration_seconds": execution["duration_seconds"],
                            "stdout_sha256": hashlib.sha256(stdout.encode("utf-8", errors="replace")).hexdigest(),
                            "stderr_sha256": hashlib.sha256(stderr.encode("utf-8", errors="replace")).hexdigest(),
                            "stdout_bytes": len(stdout.encode("utf-8", errors="replace")),
                            "stderr_bytes": len(stderr.encode("utf-8", errors="replace")),
                            "argv": argv,
                        }
                    )
                    log_path = output_root / f"command-{position + 1:02d}-{re.sub(r'[^A-Za-z0-9_.-]+', '_', str(command_id))}.log"
                    log_path.write_text(
                        f"$ {command_text}\n\n[stdout]\n{stdout}\n[stderr]\n{stderr}",
                        encoding="utf-8",
                    )
                    command_record["log"] = str(log_path)
                    if command_record["required"] and execution["returncode"] != 0:
                        commands_passed = False
                command_results.append(command_record)
            result["verification_commands"] = command_results

            # P00 names its passing unit-test log explicitly. Keep this stable
            # and human-readable while also retaining per-command logs above.
            log_evidence_path = output_root / "unit-test.log"
            if any("unittest" in item["command"] for item in command_results):
                log_lines = []
                for item in command_results:
                    if "unittest" not in item["command"]:
                        continue
                    log_path = Path(item.get("log", ""))
                    if log_path.exists():
                        log_lines.append(log_path.read_text(encoding="utf-8", errors="replace"))
                    else:
                        log_lines.append(
                            f"{item['id']}: returncode={item.get('returncode')} status={item.get('status')}"
                        )
                log_evidence_path.write_text("\n".join(log_lines) + "\n", encoding="utf-8")

            ci_payload = {
                "artifact_id": os.environ.get("GITHUB_RUN_ID") or f"local:{result.get('verified_commit', '')}",
                "artifact_name": "treatcode-plan-verification",
                "plan_id": normalized_id,
                "command_exit_codes": [
                    {"id": item.get("id"), "returncode": item.get("returncode")}
                    for item in command_results
                ],
            }
            ci_payload["content_hash"] = hashlib.sha256(
                canonical_json(ci_payload).encode("utf-8")
            ).hexdigest()
            (output_root / "ci-artifact.json").write_text(
                canonical_json(ci_payload), encoding="utf-8"
            )

            result["evidence"] = _plan_evidence_status(entry, output_root, result_path)
            result["human_approvals"] = _plan_approval_status(entry)
            status = _normalise_plan_status(entry.get("status"))
            if status != "complete":
                result["issues"].append(_plan_error("plan_not_complete", f"{normalized_id} status is {status}, not complete"))
            if not commands_passed:
                result["issues"].append(_plan_error("verification_command_failed", f"one or more required commands failed for {normalized_id}"))
            if not result["human_approvals"]["ok"]:
                result["issues"].append(_plan_error("human_approval_missing", f"required human approval is missing for {normalized_id}", roles=result["human_approvals"]["missing"]))
            evidence_missing = [item for item in result["evidence"] if item.get("path") and not item.get("exists") and not item.get("self_reference")]
            if evidence_missing:
                result["issues"].append(
                    _plan_error(
                        "evidence_missing",
                        f"required evidence is missing for {normalized_id}",
                        references=[item.get("reference") for item in evidence_missing],
                    )
                )
            record = entry.get("completion_record") if isinstance(entry.get("completion_record"), dict) else {}
            required_completion_fields = ("verified_commit", "evidence_artifact", "date")
            missing_completion_fields = [field for field in required_completion_fields if not str(record.get(field, "")).strip()]
            if missing_completion_fields and status == "complete":
                result["issues"].append(_plan_error("completion_record_incomplete", "completion record is missing required fields", fields=missing_completion_fields))
            result["verification_ok"] = bool(validation["ok"] and commands_passed)
            dependencies_ok = all(item.get("complete") for item in result["dependencies"].values())
            result["complete"] = bool(
                result["verification_ok"]
                and status == "complete"
                and result["human_approvals"]["ok"]
                and not evidence_missing
                and not missing_completion_fields
                and dependencies_ok
            )

    result["command_exit_codes"] = [
        {"id": item.get("id"), "returncode": item.get("returncode")}
        for item in result.get("verification_commands", [])
    ]
    result["issues"] = list(result.get("issues", []))
    result["content_hash"] = hashlib.sha256(
        canonical_json(_plan_stable_result_view(result)).encode("utf-8")
    ).hexdigest()
    result_path.write_text(canonical_json(result), encoding="utf-8")
    result["result_sha256"] = hashlib.sha256(result_path.read_bytes()).hexdigest()
    return result


def cmd_website_plans_validate(args: argparse.Namespace) -> int:
    manifest_path = Path(args.manifest).resolve() if args.manifest else PLAN_MANIFEST_PATH
    manifest, load_error = load_treatcode_plan_manifest(manifest_path)
    if load_error:
        report = {
            "ok": False,
            "errors": [_plan_error("manifest_load_failed", f"{manifest_path}: {load_error}")],
            "manifest": str(manifest_path),
        }
    else:
        index_path = Path(args.index).resolve() if args.index else None
        report = validate_treatcode_plan_manifest(
            manifest,
            repo_root=REPO_ROOT,
            manifest_path=manifest_path,
            index_path=index_path,
        )
    if args.json:
        print_json(report)
    else:
        print("TreatCode plans validate")
        text_status("plan manifest", bool(report.get("ok")), f"{report.get('plan_count', 0)} plans")
        for error in report.get("errors", []):
            print(f"- {error.get('code')}: {error.get('message')}")
    return 0 if report.get("ok") else 1


def cmd_website_plan_verify(args: argparse.Namespace) -> int:
    manifest_path = Path(args.manifest).resolve() if args.manifest else PLAN_MANIFEST_PATH
    output_dir = Path(args.output).resolve() if args.output else None
    index_path = Path(args.index).resolve() if args.index else None
    result = verify_treatcode_plan(
        args.plan_id,
        manifest_path=manifest_path,
        output_dir=output_dir,
        repo_root=REPO_ROOT,
        index_path=index_path,
        run_commands=not args.no_run,
        timeout=args.timeout,
    )
    if args.json:
        print_json(result)
    else:
        state = "complete" if result.get("complete") else "incomplete"
        print(f"TreatCode plan {result['plan']['id']}: {state}")
        print(f"evidence: {result.get('output')}")
        for issue in result.get("issues", []):
            print(f"- {issue.get('code')}: {issue.get('message')}")
    if result.get("complete"):
        return 0
    if args.allow_incomplete and result.get("verification_ok"):
        return 0
    return 1


def cmd_website_operations(args: argparse.Namespace) -> int:
    """Run operations control-plane exercises and persist P12 evidence."""

    if args.operations_action != "disaster-recovery-test":
        raise RuntimeError(f"unknown website operations action {args.operations_action!r}")
    npm = "npm.cmd" if platform.system().lower().startswith("win") else "npm"
    execution = run_command(
        [npm, "--prefix", "treatcode", "run", "test:disaster-recovery"],
        cwd=REPO_ROOT,
        capture=True,
        timeout=args.timeout,
    )
    evidence_path = PLAN_EVIDENCE_DIR / "P12" / "operations-command.json"
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    report = {
        "schema": "treatcode.operations_command.v1",
        "command": "npm --prefix treatcode run test:disaster-recovery",
        "returncode": execution["returncode"],
        "ok": execution["returncode"] == 0,
        "duration_seconds": execution["duration_seconds"],
        "stdout": execution.get("stdout", ""),
        "stderr": execution.get("stderr", ""),
        "evidence": "build/treatcode-plan-evidence/P12/disaster-recovery.json",
    }
    evidence_path.write_text(canonical_json(report), encoding="utf-8")
    if args.json:
        print_json(report)
    else:
        print("TreatCode operations disaster-recovery-test")
        print(f"[{'ok' if report['ok'] else 'fail'}] backup, restore, and recovery exercise")
        if report["stdout"].strip():
            print(report["stdout"].rstrip())
        if report["stderr"].strip():
            print(report["stderr"].rstrip(), file=sys.stderr)
    return int(execution["returncode"])


def cmd_website_benchmarks_verify_reference(args: argparse.Namespace) -> int:
    manifest_path = Path(args.manifest).resolve() if args.manifest else P10_MANIFEST_PATH
    reference_path = Path(args.reference).resolve() if args.reference else P10_REFERENCE_PATH
    output_path = Path(args.output).resolve() if args.output else DEFAULT_EVIDENCE_PATH
    report = verify_p10_reference(manifest_path, reference_path, output_path)
    if args.json:
        print_json(report)
    else:
        print("TreatCode P10 benchmark reference verification")
        print(f"[{'ok' if report.get('ok') else 'fail'}] reference protocol")
        print(f"evidence: {report.get('output', output_path)}")
        for check in report.get("checks", []):
            print(f"[ok] {check}")
        for error in report.get("errors", []):
            print(f"[fail] {error.get('code')}: {error.get('message')}")
    return 0 if report.get("ok") else 1


def cmd_website_challenges_validate(args: argparse.Namespace) -> int:
    manifest_path = Path(args.manifest).resolve() if getattr(args, "manifest", None) else None
    report = validate_challenge_manifest(manifest_path) if manifest_path else validate_challenge_manifest()
    evidence_path = REPO_ROOT / "build" / "treatcode-plan-evidence" / "P06" / "challenge-validation.json"
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    evidence_path.write_text(canonical_json(report), encoding="utf-8")
    if args.json:
        print_json(report)
    else:
        print("TreatCode challenge manifest validation")
        text_status("challenge manifest", bool(report.get("ok")), f"{report.get('challenges', 0)} entries")
        lifecycle = report.get("lifecycle_counts", {})
        print(
            f"[{'ok' if report.get('ok') else 'fail'}] lifecycle "
            f"published={lifecycle.get('published', 0)} "
            f"draft={lifecycle.get('draft', 0)} "
            f"retired={lifecycle.get('retired', 0)}"
        )
        for error in report.get("errors", []):
            print(f"[fail] {error.get('code')}: {error.get('message')}")
        print(f"evidence: {evidence_path}")
    return 0 if report.get("ok") else 1


def _repository_index_repo_root(args: argparse.Namespace) -> Path:
    value = getattr(args, "repo_root", None)
    return Path(value).resolve() if value else REPO_ROOT


def cmd_website_index_build(args: argparse.Namespace) -> int:
    repo_root = _repository_index_repo_root(args)
    report = build_index(
        repo_root=repo_root,
        output_dir=Path(args.output).resolve() if getattr(args, "output", None) else None,
        clean=bool(getattr(args, "clean", False)),
    )
    return print_repository_index_report(report, "TreatCode repository index build", json_output=bool(args.json))


def cmd_website_index_verify(args: argparse.Namespace) -> int:
    repo_root = _repository_index_repo_root(args)
    report = verify_index(
        repo_root=repo_root,
        index_path=Path(args.index).resolve() if getattr(args, "index", None) else None,
        evidence_dir=Path(args.evidence).resolve() if getattr(args, "evidence", None) else None,
    )
    return print_repository_index_report(report, "TreatCode repository index verify", json_output=bool(args.json))


def cmd_website_index_compare(args: argparse.Namespace) -> int:
    repo_root = _repository_index_repo_root(args)
    report = compare_clean_incremental(
        repo_root=repo_root,
        evidence_dir=Path(args.evidence).resolve() if getattr(args, "evidence", None) else None,
    )
    return print_repository_index_report(report, "TreatCode repository index compare-clean-incremental", json_output=bool(args.json))


def cmd_website_index_context(args: argparse.Namespace) -> int:
    repo_root = _repository_index_repo_root(args)
    index, load_error, index_file = load_index(
        Path(args.index).resolve() if getattr(args, "index", None) else None,
        repo_root=repo_root,
    )
    if load_error or index is None:
        report = {
            "schema": "treatcode.context-package.v1",
            "ok": False,
            "errors": [{"code": "index_load_failed", "message": load_error or "index is missing"}],
            "index": str(index_file),
        }
        return print_repository_index_report(report, "TreatCode context package", json_output=bool(args.json))
    output = Path(args.output).resolve() if getattr(args, "output", None) else (
        repo_root / "build" / "treatcode-plan-evidence" / "P03" / "context-package.v1.json"
    )
    package = generate_context_package(
        index,
        args.scope,
        output_path=output,
        repo_root=repo_root,
    )
    package["ok"] = not package.get("errors")
    return print_repository_index_report(package, "TreatCode context package", json_output=bool(args.json))


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


def cmd_compact_disk(args: argparse.Namespace) -> int:
    source = Path(args.disk_image).resolve()
    output = Path(args.output).resolve() if args.output else None
    result = compact_sparse_disk(source, output)
    if args.json:
        print_json(result)
    elif result["ok"]:
        before = result["before"]
        after = result["after"]
        print(f"disk: {result['source']}")
        print(f"output: {result['output']}")
        print(
            "records: "
            f"{before.get('readable_records', 0)} -> {after.get('readable_records', 0)} "
            f"live_blocks={after.get('live_blocks', 0)}"
        )
        print(
            "bytes: "
            f"{before.get('file_size', 0)} -> {after.get('file_size', 0)}"
        )
        if before.get("recoverable_tail"):
            print("recovered: ignored a truncated append tail")
    else:
        print(f"disk: {result['source']}", file=sys.stderr)
        for issue in result["issues"]:
            print(f"[warn] {issue}", file=sys.stderr)
    return 0 if result["ok"] else 1


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
    if args.warmups < 0 or args.iterations < 1 or args.max_cv <= 0:
        print(
            "bench requires warmups >= 0, iterations >= 1, and max-cv > 0",
            file=sys.stderr)
        return 2

    warmup_returncodes: list[int] = []
    measured_returncodes: list[int] = []
    samples: list[float] = []
    original_no_build = args.no_build
    total_runs = args.warmups + args.iterations
    for run_index in range(total_runs):
        started = time.perf_counter()
        returncode = cmd_test(args)
        elapsed = time.perf_counter() - started
        args.no_build = True
        if run_index < args.warmups:
            warmup_returncodes.append(returncode)
        else:
            measured_returncodes.append(returncode)
            samples.append(elapsed)
    args.no_build = original_no_build

    mean = statistics.fmean(samples)
    deviation = statistics.pstdev(samples)
    coefficient = deviation / mean if mean > 0 else 0.0
    git_commit = run_command(
        ["git", "rev-parse", "HEAD"], capture=True, timeout=10)
    git_dirty = run_command(
        ["git", "status", "--porcelain"], capture=True, timeout=10)
    cache = parse_cmake_cache(default_build_dir(args.build_dir))
    report = {
        "schema": "trit.benchmark_result.v1",
        "captured_at_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(),
        "source": {
            "repository": str(REPO_ROOT),
            "commit": git_commit["stdout"].strip()
                if git_commit["returncode"] == 0 else "unknown",
            "dirty": bool(git_dirty["stdout"].strip()),
        },
        "host": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "python": platform.python_version(),
        },
        "build": {
            "directory": str(default_build_dir(args.build_dir)),
            "profile": cache.get("CMAKE_BUILD_TYPE", "multi-config-or-default"),
            "compiler": cache.get("CMAKE_CXX_COMPILER", "unknown"),
        },
        "workload": {
            "name": "benchmark-suite",
            "suite": "benchmark",
            "targets": ["test_benchmark", "test_execution_backends_benchmark"],
        },
        "correctness": {
            "passed": all(code == 0 for code in
                          warmup_returncodes + measured_returncodes),
            "warmup_returncodes": warmup_returncodes,
            "measured_returncodes": measured_returncodes,
        },
        "timing": {
            "unit": "seconds",
            "warmups": args.warmups,
            "iterations": args.iterations,
            "samples": samples,
            "median": statistics.median(samples),
            "mean": mean,
            "standard_deviation": deviation,
            "coefficient_of_variation": coefficient,
            "maximum_accepted_cv": args.max_cv,
            "stable": coefficient < args.max_cv,
        },
        "instruction_mix": {},
        "memory": {},
        "tlb": {},
        "scheduler": {},
        "wal": {},
        "disk": {},
    }
    output = Path(args.output) if args.output else (
        default_build_dir(args.build_dir) / "benchmarks" /
        ("benchmark-" +
         _dt.datetime.now(_dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") +
         ".json"))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print(f"wrote {output}")
    if not report["correctness"]["passed"]:
        return 1
    if coefficient >= args.max_cv:
        print(
            f"benchmark timing rejected: CV {coefficient:.3%} is not "
            f"below {args.max_cv:.3%}",
            file=sys.stderr)
        return 1
    return 0


def _load_syscall_trace(path: Path) -> tuple[list[dict[str, Any]], list[str]]:
    """Load and validate the host runtime's deterministic syscall trace.

    Malformed or disabled traces cannot be mistaken for replay evidence, and
    two runs can be compared byte-for-byte at the event level.  A diagnostics
    directory may be passed instead of a JSONL file; in that form the paired
    VM checkpoint and guest-input journal are validated as well.
    """
    issues: list[str] = []
    events: list[dict[str, Any]] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        return events, [f"{path}: cannot read trace: {exc}"]
    previous_cycle_after: int | None = None
    for line_number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError as exc:
            issues.append(f"{path}:{line_number}: invalid JSON: {exc.msg}")
            continue
        if not isinstance(event, dict):
            issues.append(f"{path}:{line_number}: event must be a JSON object")
            continue
        if event.get("schema") != "trit.syscall_trace.v1":
            issues.append(f"{path}:{line_number}: unsupported or missing schema")
            continue
        marker = event.get("event")
        if marker == "trace_disabled":
            issues.append(f"{path}:{line_number}: trace was disabled during capture")
            continue
        if marker == "trace_empty":
            # An enabled VM may legitimately execute no syscalls.  Keep the
            # marker in the event stream so comparison distinguishes it from
            # an accidentally disabled capture.
            events.append(event)
            continue
        required = {
            "sequence", "pc", "physical_pc", "syscall_id", "process_id",
            "before_privilege", "after_privilege", "args", "results",
            "before_status", "after_status", "trap", "trap_code",
            "trap_cause", "cycle_before", "cycle_after",
        }
        missing = sorted(required - event.keys())
        if missing:
            issues.append(
                f"{path}:{line_number}: missing fields: {', '.join(missing)}")
            continue

        def integer(name: str) -> int | None:
            value = event.get(name)
            if isinstance(value, bool) or not isinstance(value, int):
                issues.append(f"{path}:{line_number}: {name} must be an integer")
                return None
            return value

        sequence = integer("sequence")
        cycle_before = integer("cycle_before")
        cycle_after = integer("cycle_after")
        for name in (
            "pc", "physical_pc", "syscall_id", "process_id",
            "before_privilege", "after_privilege", "before_status",
            "after_status", "trap", "trap_code", "trap_cause",
        ):
            integer(name)
        args = event.get("args")
        results = event.get("results")
        if (not isinstance(args, list) or len(args) != 4 or
                any(isinstance(value, bool) or not isinstance(value, int)
                    for value in args)):
            issues.append(f"{path}:{line_number}: args must contain four integers")
        if (not isinstance(results, list) or len(results) != 3 or
                any(isinstance(value, bool) or not isinstance(value, int)
                    for value in results)):
            issues.append(f"{path}:{line_number}: results must contain three integers")
        if sequence is not None and sequence != len(
                [item for item in events if "sequence" in item]):
            issues.append(
                f"{path}:{line_number}: sequence must be contiguous from zero")
        if cycle_before is not None and cycle_after is not None:
            if cycle_after < cycle_before:
                issues.append(f"{path}:{line_number}: cycle_after precedes cycle_before")
            if (previous_cycle_after is not None and
                    cycle_before < previous_cycle_after):
                issues.append(f"{path}:{line_number}: cycles move backwards")
            previous_cycle_after = cycle_after
        events.append(event)
    if not lines:
        issues.append(f"{path}: trace is empty; expected a JSONL marker or event")
    return events, issues


def _load_input_journal(path: Path) -> tuple[list[dict[str, Any]], list[str]]:
    issues: list[str] = []
    events: list[dict[str, Any]] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        return events, [f"{path}: cannot read input journal: {exc}"]
    for line_number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError as exc:
            issues.append(f"{path}:{line_number}: invalid JSON: {exc.msg}")
            continue
        if not isinstance(event, dict) or event.get("schema") != "trit.input_journal.v1":
            issues.append(f"{path}:{line_number}: unsupported or missing schema")
            continue
        sequence = event.get("sequence")
        cycle = event.get("cycle")
        kind = event.get("kind")
        if (isinstance(sequence, bool) or not isinstance(sequence, int) or
                sequence != len(events)):
            issues.append(f"{path}:{line_number}: sequence must be contiguous from zero")
        # A restore/replay boundary legitimately moves the VM cycle backwards;
        # only the event type and non-negative integer shape are invariant in
        # the journal itself.
        if (isinstance(cycle, bool) or not isinstance(cycle, int) or
                cycle < 0):
            issues.append(f"{path}:{line_number}: cycle must be a non-negative integer")
        if kind not in (1, 2, 3):
            issues.append(f"{path}:{line_number}: kind must be keyboard, text, or mouse")
        for name in ("value0", "value1", "value2"):
            value = event.get(name)
            if isinstance(value, bool) or not isinstance(value, int):
                issues.append(f"{path}:{line_number}: {name} must be an integer")
        if not isinstance(event.get("text", ""), str):
            issues.append(f"{path}:{line_number}: text must be a string")
        events.append(event)
    return events, issues


def _load_checkpoint_metadata(path: Path) -> list[str]:
    try:
        metadata = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"{path}: invalid checkpoint metadata: {exc}"]
    if not isinstance(metadata, dict) or metadata.get("schema") not in (
            "trit.runtime_checkpoint.v1", "trit.runtime_checkpoint.v2"):
        return [f"{path}: unsupported or missing checkpoint schema"]
    if not isinstance(metadata.get("available"), bool):
        return [f"{path}: available must be boolean"]
    if not metadata["available"]:
        return []
    issues: list[str] = []
    for name in ("sequence", "input_event_count", "cycle", "pc"):
        value = metadata.get(name)
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            issues.append(f"{path}: {name} must be a non-negative integer")
    if metadata.get("schema") == "trit.runtime_checkpoint.v2":
        for field in ("state_file", "disk_file", "journal_file", "boot_image"):
            value = metadata.get(field)
            if not isinstance(value, str) or not value:
                issues.append(f"{path}: {field} must be a non-empty relative path")
                continue
            target = (path.parent / value).resolve()
            try:
                target.relative_to(path.parent.resolve())
            except ValueError:
                issues.append(f"{path}: {field} escapes the checkpoint bundle")
                continue
            if not target.is_file():
                issues.append(f"{target}: checkpoint bundle file is missing")
    return issues


_REPLAY_RESULT_FIELDS = (
    "ok",
    "status",
    "steps",
    "pc",
    "cycles",
    "require_halt",
)


def _compare_replay_results(
    left: dict[str, Any], right: dict[str, Any]
) -> dict[str, Any]:
    """Compare the deterministic architectural result of two replays.

    The helper's description and error text are intentionally excluded: they
    are diagnostic prose and may include bundle-specific paths.  The fields
    below are the result contract that describes the replay outcome and must
    agree when two independently restored bundles are equivalent.
    """
    mismatches: list[dict[str, Any]] = []
    for field in _REPLAY_RESULT_FIELDS:
        left_value = left.get(field)
        right_value = right.get(field)
        if left_value != right_value:
            mismatches.append(
                {"field": field, "left": left_value, "right": right_value}
            )
    return {
        "match": not mismatches,
        "fields": list(_REPLAY_RESULT_FIELDS),
        "mismatches": mismatches,
    }


def _run_checkpoint_replay(
    executable: Path,
    bundle: Path,
    steps: int,
    require_halt: bool,
) -> tuple[dict[str, Any], bool, str]:
    """Run the standalone replay helper and preserve a stable result shape.

    ``safe`` only means that the process returned one of the documented
    restore/replay statuses and emitted a JSON object.  The caller still
    checks the helper's ``ok`` field so a trapped replay remains a failure.
    """
    command = [str(executable), str(bundle), "--steps", str(steps), "--json"]
    if require_halt:
        command.append("--require-halt")
    completed = subprocess.run(
        command,
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    try:
        candidate = json.loads(completed.stdout)
    except json.JSONDecodeError:
        candidate = None
    if not isinstance(candidate, dict):
        candidate = {
            "ok": False,
            "returncode": completed.returncode,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
        }
    safe_return = completed.returncode in (0, 3, 4)
    return candidate, safe_return, completed.stderr


def cmd_replay(args: argparse.Namespace) -> int:
    requested = Path(args.trace)
    trace = requested / "syscall_trace.jsonl" if requested.is_dir() else requested
    events, issues = _load_syscall_trace(trace)
    artifact_summary: dict[str, Any] = {}
    if requested.is_dir():
        journal = requested / "input_journal.jsonl"
        checkpoint = requested / "checkpoint.json"
        if not journal.is_file():
            issues.append(f"{journal}: missing input journal")
        else:
            input_events, input_issues = _load_input_journal(journal)
            issues.extend(input_issues)
            artifact_summary["input_event_count"] = len(input_events)
        if not checkpoint.is_file():
            issues.append(f"{checkpoint}: missing checkpoint metadata")
        else:
            checkpoint_issues = _load_checkpoint_metadata(checkpoint)
            issues.extend(checkpoint_issues)
            artifact_summary["checkpoint_metadata"] = str(checkpoint)
    comparison: dict[str, Any] | None = None
    if args.against:
        requested_other = Path(args.against)
        other = (requested_other / "syscall_trace.jsonl"
                 if requested_other.is_dir() else requested_other)
        other_events, other_issues = _load_syscall_trace(other)
        issues.extend(other_issues)
        left = [json.dumps(item, sort_keys=True, separators=(",", ":"))
                for item in events]
        right = [json.dumps(item, sort_keys=True, separators=(",", ":"))
                 for item in other_events]
        mismatch = None
        for index, (lhs, rhs) in enumerate(zip(left, right)):
            if lhs != rhs:
                mismatch = index
                break
        if mismatch is None and len(left) != len(right):
            mismatch = min(len(left), len(right))
        comparison = {
            "against": str(other),
            "match": mismatch is None and not other_issues,
            "mismatch_index": mismatch,
            "event_count": len(other_events),
        }
        if mismatch is not None:
            issues.append(f"trace differs from {other} at event {mismatch}")

    summary: dict[str, Any] = {
        "schema": "trit.syscall_trace.v1",
        "trace": str(trace),
        "valid": not issues,
        "event_count": len(events),
        "issues": issues,
    }
    if artifact_summary:
        summary["artifacts"] = artifact_summary
    if comparison is not None:
        summary["comparison"] = comparison
    if args.execute:
        # Keep structural validation separate from replay execution.  A trace
        # mismatch (or malformed bundle) must never be hidden by a helper run.
        structural_issue_count = len(issues)
        executable: Path | None = None
        if not requested.is_dir():
            issues.append("--execute requires a checkpoint bundle directory")
        elif issues:
            # Do not execute a bundle that already failed structural validation.
            pass
        else:
            executable = find_executable(
                default_build_dir(args.build_dir), "trit_checkpoint_replay"
            )
            if executable is None:
                issues.append("trit_checkpoint_replay is not built; build the helper first")
            else:
                execution, safe_return, execution_stderr = _run_checkpoint_replay(
                    executable, requested, args.steps, args.require_halt
                )
                summary["execution"] = execution
                if not safe_return or not execution.get("ok", False):
                    issues.append("checkpoint replay execution failed")
                elif execution_stderr:
                    summary["execution_stderr"] = execution_stderr

        # ``--against`` historically compared only JSONL traces.  When both
        # operands are complete bundles, --execute now performs the same
        # replay in a second process and compares deterministic result fields.
        # This catches a replay that emits an identical trace but diverges in
        # final VM state (or in its halt/trap outcome).
        if (
            requested.is_dir()
            and args.against
            and Path(args.against).is_dir()
            and structural_issue_count == 0
            and executable is not None
        ):
            other_bundle = Path(args.against)
            against_execution, against_safe, against_stderr = _run_checkpoint_replay(
                executable, other_bundle, args.steps, args.require_halt
            )
            summary["execution_against"] = against_execution
            if not against_safe or not against_execution.get("ok", False):
                issues.append("checkpoint replay against-bundle execution failed")
            elif against_stderr:
                summary["execution_against_stderr"] = against_stderr
            if "execution" in summary:
                execution_comparison = _compare_replay_results(
                    summary["execution"], against_execution
                )
                summary["execution_comparison"] = execution_comparison
                if not execution_comparison["match"]:
                    fields = ", ".join(
                        mismatch["field"]
                        for mismatch in execution_comparison["mismatches"]
                    )
                    issues.append(
                        "checkpoint replay result differs from "
                        f"{other_bundle} ({fields})"
                    )
    summary["valid"] = not issues
    if args.json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        state = "valid" if not issues else "invalid"
        print(f"{trace}: {state}; {len(events)} event(s)")
        for issue in issues:
            print(f"error: {issue}", file=sys.stderr)
        if comparison is not None:
            print("comparison: " + ("match" if comparison["match"] else "mismatch"))
    return 0 if not issues else 1


def cmd_fuzz(args: argparse.Namespace) -> int:
    """Run deterministic, bounded malformed-input checks before optional smoke.

    This is intentionally a structural harness rather than a statistical fuzzer:
    it makes the image validators consume reproducible truncations, header
    mutations, record mutations, and tails, and treats an uncaught exception as
    a failure.  The parser is allowed to accept recoverable tails, so the gate
    focuses on safety and stable result shape rather than rejecting every byte
    mutation.
    """

    iterations = max(1, min(int(args.iterations), 4096))
    rng = random.Random(int(args.seed))
    build_dir = default_build_dir(args.build_dir)

    def choose_seed(explicit: str | None, names: list[Path]) -> Path | None:
        if explicit:
            candidate = Path(explicit).resolve()
            return candidate if candidate.is_file() else None
        for candidate in names:
            if candidate.is_file():
                return candidate.resolve()
        return None

    boot_seed = choose_seed(
        args.boot_image,
        [
            REPO_ROOT / "build" / "release" / "TernaryOS" / "ternary-os.tboot",
            build_dir / "ternary-os.tboot",
        ],
    )
    disk_seed = choose_seed(
        args.disk_image,
        [
            REPO_ROOT / "build" / "release" / "TernaryOS" / "ternary-os.tdisk",
            build_dir / "ternary-os.tdisk",
        ],
    )

    report: dict[str, Any] = {
        "schema": "trit.structural_fuzz.v1",
        "ok": True,
        "seed": int(args.seed),
        "iterations": iterations,
        "build_dir": str(build_dir),
        "inputs": {},
        "failures": [],
    }

    def mutate(raw: bytes, kind: str, iteration: int) -> tuple[bytes, str]:
        data = bytearray(raw)
        operators = (
            "truncate",
            "flip",
            "zero_range",
            "append",
            "header_field",
            "record_field",
        )
        operator = operators[iteration % len(operators)]
        if not data:
            return b"\x00", "empty_seed_replacement"
        if operator == "truncate":
            # Include a zero-length case and a partial-header case early.
            end = 0 if iteration == 0 else rng.randrange(1, len(data))
            return bytes(data[:end]), f"truncate:{end}"
        if operator == "flip":
            index = rng.randrange(len(data))
            data[index] ^= 1 << rng.randrange(8)
            return bytes(data), f"flip:{index}"
        if operator == "zero_range":
            start = rng.randrange(len(data))
            width = min(rng.randrange(1, 33), len(data) - start)
            data[start : start + width] = b"\x00" * width
            return bytes(data), f"zero:{start}+{width}"
        if operator == "append":
            width = 1 + rng.randrange(65)
            return bytes(data) + rng.randbytes(width), f"append:{width}"
        if operator == "header_field":
            if kind == "boot" and len(data) >= 24:
                field = (iteration // len(operators)) % 3
                offset = (0, 8, 16)[field]
                current = struct.unpack_from("<Q", data, offset)[0]
                struct.pack_into("<Q", data, offset, current ^ (1 << (iteration % 61)))
                return bytes(data), f"boot_header:{offset}"
            if kind == "disk" and len(data) >= SPARSE_DISK_HEADER.size:
                field = (iteration // len(operators)) % 4
                offset, fmt = (
                    (0, "<Q"),
                    (8, "<I"),
                    (12, "<I"),
                    (32, "<i"),
                )[field]
                current = struct.unpack_from(fmt, data, offset)[0]
                bits = struct.calcsize(fmt) * 8
                struct.pack_into(fmt, data, offset, current ^ (1 << (iteration % (bits - 1))))
                return bytes(data), f"disk_header:{offset}"
            if kind == "checkpoint" and len(data) >= 20:
                # magic + version precede the logical IMEM/DMEM dimensions.
                offset = 12 if iteration % 2 == 0 else 16
                current = struct.unpack_from("<i", data, offset)[0]
                struct.pack_into("<i", data, offset, current ^ (1 << (iteration % 30)))
                return bytes(data), f"checkpoint_header:{offset}"
            return bytes(data[: max(0, len(data) - 1)]), "short_header"
        # Corrupt a record index or a word while preserving the surrounding file.
        if kind == "disk" and len(data) >= SPARSE_DISK_LEGACY_HEADER.size + SPARSE_DISK_RECORD_SIZE:
            header_size = (
                SPARSE_DISK_HEADER.size
                if len(data) >= SPARSE_DISK_HEADER.size
                and struct.unpack_from("<Q", data, 0)[0] == SPARSE_DISK_MAGIC
                else SPARSE_DISK_LEGACY_HEADER.size
            )
            record_count = max(1, (len(data) - header_size) // SPARSE_DISK_RECORD_SIZE)
            record = rng.randrange(record_count)
            base = header_size + record * SPARSE_DISK_RECORD_SIZE
            if iteration % 2:
                struct.pack_into("<i", data, base, -1 if iteration % 4 else 0x7FFFFFFF)
                return bytes(data), f"disk_record_index:{record}"
            word_offset = base + SPARSE_DISK_RECORD_HEADER.size + rng.randrange(STORAGE_BLOCK_WORDS) * SPARSE_DISK_WORD.size
            current = struct.unpack_from("<Q", data, word_offset)[0]
            struct.pack_into("<Q", data, word_offset, current ^ (1 << (iteration % 63)))
            return bytes(data), f"disk_record_word:{record}"
        index = rng.randrange(len(data))
        data[index] ^= 0xFF
        return bytes(data), f"fallback_flip:{index}"

    def exercise(seed_path: Path | None, kind: str) -> None:
        if seed_path is None:
            report["inputs"][kind] = {"available": False}
            report["failures"].append(f"no {kind} seed image was found")
            return
        try:
            raw = seed_path.read_bytes()
        except OSError as exc:
            report["inputs"][kind] = {"available": False, "path": str(seed_path)}
            report["failures"].append(f"{kind} seed could not be read: {exc}")
            return
        if not raw:
            report["inputs"][kind] = {"available": False, "path": str(seed_path)}
            report["failures"].append(f"{kind} seed is empty")
            return
        cases: list[dict[str, Any]] = []
        with tempfile.TemporaryDirectory(prefix=f"trit-{kind}-fuzz-") as temp_dir:
            target = Path(temp_dir) / seed_path.name
            for iteration in range(iterations):
                mutated, mutation = mutate(raw, kind, iteration)
                target.write_bytes(mutated)
                try:
                    inspected = (
                        inspect_boot_image(target)
                        if kind == "boot"
                        else inspect_sparse_disk(target)
                    )
                    if not isinstance(inspected, dict) or not isinstance(inspected.get("issues"), list):
                        raise ValueError("validator returned an invalid result shape")
                    cases.append(
                        {
                            "iteration": iteration,
                            "mutation": mutation,
                            "ok": bool(inspected.get("ok")),
                            "issue_count": len(inspected.get("issues", [])),
                        }
                    )
                except Exception as exc:  # noqa: BLE001 - this is the crash gate
                    report["failures"].append(
                        f"{kind} iteration {iteration} ({mutation}) raised {type(exc).__name__}: {exc}"
                    )
        report["inputs"][kind] = {
            "available": True,
            "path": str(seed_path),
            "bytes": len(raw),
            "cases": cases,
            "accepted_cases": sum(1 for case in cases if case["ok"]),
            "rejected_cases": sum(1 for case in cases if not case["ok"]),
        }

    exercise(boot_seed, "boot")
    exercise(disk_seed, "disk")

    checkpoint_explicit = bool(args.checkpoint_bundle)
    checkpoint_seed = (
        Path(args.checkpoint_bundle).resolve()
        if checkpoint_explicit
        else (build_dir / "host_runtime_checkpoint_bundle").resolve()
    )
    helper = find_executable(build_dir, "trit_checkpoint_replay")
    checkpoint_report: dict[str, Any] = {
        "available": False,
        "path": str(checkpoint_seed),
        "cases": [],
    }
    if checkpoint_seed.is_dir() and (checkpoint_seed / "vm_state.bin").is_file() and helper:
        try:
            state_bytes = (checkpoint_seed / "vm_state.bin").read_bytes()
            with tempfile.TemporaryDirectory(prefix="trit-checkpoint-fuzz-") as temp_dir:
                bundle = Path(temp_dir) / "bundle"
                shutil.copytree(checkpoint_seed, bundle)
                state_path = bundle / "vm_state.bin"
                for iteration in range(iterations):
                    mutated, mutation = mutate(state_bytes, "checkpoint", iteration)
                    state_path.write_bytes(mutated)
                    command_result = run_command(
                        [str(helper), "--bundle", str(bundle), "--steps", "32", "--json"],
                        cwd=REPO_ROOT,
                        capture=True,
                        timeout=min(max(5, int(args.timeout)), 60),
                    )
                    parsed: dict[str, Any] | None = None
                    try:
                        candidate = json.loads(command_result["stdout"])
                        if isinstance(candidate, dict):
                            parsed = candidate
                    except json.JSONDecodeError:
                        parsed = None
                    safe_return = command_result["returncode"] in (0, 3, 4)
                    if not safe_return or parsed is None:
                        report["failures"].append(
                            "checkpoint iteration "
                            f"{iteration} ({mutation}) exited unsafely: "
                            f"rc={command_result['returncode']} "
                            f"stderr={command_result['stderr'][:240]}"
                        )
                    checkpoint_report["cases"].append(
                        {
                            "iteration": iteration,
                            "mutation": mutation,
                            "returncode": command_result["returncode"],
                            "ok": bool(parsed.get("ok")) if parsed else False,
                        }
                    )
            checkpoint_report.update(
                {
                    "available": True,
                    "bytes": len(state_bytes),
                    "accepted_cases": sum(
                        1 for case in checkpoint_report["cases"] if case["ok"]
                    ),
                    "rejected_cases": sum(
                        1 for case in checkpoint_report["cases"] if not case["ok"]
                    ),
                }
            )
        except OSError as exc:
            report["failures"].append(f"checkpoint fuzz setup failed: {exc}")
    elif checkpoint_explicit:
        report["failures"].append(
            "explicit checkpoint bundle is missing or trit_checkpoint_replay is unavailable"
        )
    else:
        checkpoint_report["skipped"] = True
    report["inputs"]["checkpoint_replay"] = checkpoint_report
    report["ok"] = not report["failures"]

    if not args.skip_tests:
        args.suites = ["smoke"]
        args.all = False
        args.list = False
        if args.json:
            captured = io.StringIO()
            with contextlib.redirect_stdout(captured):
                test_rc = cmd_test(args)
            try:
                report["smoke"] = json.loads(captured.getvalue())
            except json.JSONDecodeError:
                report["smoke"] = {"ok": test_rc == 0, "output": captured.getvalue()}
        else:
            test_rc = cmd_test(args)
            report["smoke"] = {"ok": test_rc == 0}
        if test_rc != 0:
            report["ok"] = False

    if args.json:
        print_json(report)
    else:
        print(
            f"structural fuzz: {'ok' if report['ok'] else 'fail'} "
            f"seed={report['seed']} iterations={iterations}"
        )
        for kind, details in report["inputs"].items():
            if details.get("available"):
                print(
                    f"- {kind}: rejected={details['rejected_cases']} "
                    f"accepted={details['accepted_cases']}"
                )
            else:
                print(f"- {kind}: unavailable")
        for failure in report["failures"]:
            print(f"[error] {failure}", file=sys.stderr)
    return 0 if report["ok"] else 1


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
    bench.add_argument(
        "--warmups", type=int, default=2,
        help="unmeasured warmup iterations (default: 2)")
    bench.add_argument(
        "--iterations", type=int, default=7,
        help="measured iterations (default: 7)")
    bench.add_argument(
        "--max-cv", type=float, default=0.03,
        help="maximum accepted coefficient of variation (default: 0.03)")
    bench.add_argument(
        "--output", default=None,
        help="benchmark JSON destination")
    bench.set_defaults(func=cmd_bench)

    inspect = sub.add_parser("inspect-image", help="validate and summarize a .tboot image")
    inspect.add_argument("image")
    inspect.add_argument("--json", action="store_true")
    inspect.set_defaults(func=cmd_inspect_image)

    compact_disk = sub.add_parser("compact-disk", help="compact a sparse .tdisk append-record image")
    compact_disk.add_argument("disk_image")
    compact_disk.add_argument("--output", default=None, help="write compacted image to a new path")
    compact_disk.add_argument("--json", action="store_true")
    compact_disk.set_defaults(func=cmd_compact_disk)

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

    replay = sub.add_parser(
        "replay",
        help="validate and compare deterministic syscall traces",
    )
    replay.add_argument("trace", help="captured syscall_trace.jsonl")
    replay.add_argument(
        "--against",
        default=None,
        help=(
            "compare this trace with a second capture event-by-event; with "
            "--execute and two bundle directories, also compare replay results"
        ),
    )
    replay.add_argument(
        "--execute",
        action="store_true",
        help="restore and replay a checkpoint bundle in the native helper process",
    )
    replay.add_argument("--build-dir", default=None,
                        help="CMake build directory containing trit_checkpoint_replay")
    replay.add_argument("--steps", type=int, default=100000,
                        help="maximum replay steps when --execute is used")
    replay.add_argument("--require-halt", action="store_true",
                        help="fail --execute unless replay reaches HALTED")
    replay.add_argument("--json", action="store_true")
    replay.set_defaults(func=cmd_replay)

    fuzz = sub.add_parser("fuzz", help="run deterministic malformed-image checks")
    add_test_options(fuzz)
    fuzz.add_argument("--iterations", type=int, default=64,
                      help="mutations per image kind (default: 64)")
    fuzz.add_argument("--seed", type=int, default=0x54524954,
                      help="deterministic mutation seed")
    fuzz.add_argument("--boot-image", default=None,
                      help="explicit .tboot seed; otherwise use the release/build image")
    fuzz.add_argument("--disk-image", default=None,
                      help="explicit .tdisk seed; otherwise use the release/build image")
    fuzz.add_argument("--checkpoint-bundle", default=None,
                      help="explicit checkpoint bundle; otherwise fuzz the standard build bundle when present")
    fuzz.add_argument("--skip-tests", action="store_true",
                      help="skip the smoke suite after structural fuzzing")
    fuzz.set_defaults(func=cmd_fuzz)

    knowledge = sub.add_parser("knowledge", help="manage Obsidian and Graphify knowledge artifacts")
    knowledge_sub = knowledge.add_subparsers(dest="knowledge_command", required=True)

    knowledge_setup = knowledge_sub.add_parser("setup", help="write Obsidian/Graphify integration files")
    knowledge_setup.add_argument("--check", action="store_true", help="verify integration files without writing")
    knowledge_setup.add_argument("--json", action="store_true", help="emit JSON report")
    knowledge_setup.set_defaults(func=cmd_knowledge_setup)

    knowledge_status = knowledge_sub.add_parser("status", help="validate the docs vault and optional graph integration")
    knowledge_status.add_argument("--json", action="store_true", help="emit JSON report")
    knowledge_status.set_defaults(func=cmd_knowledge_status)

    knowledge_canvas = knowledge_sub.add_parser("canvas", help="generate the Obsidian JSON Canvas map")
    knowledge_canvas.add_argument("--check", action="store_true", help="verify canvas without writing")
    knowledge_canvas.add_argument("--json", action="store_true", help="emit JSON report")
    knowledge_canvas.set_defaults(func=cmd_knowledge_canvas)

    knowledge_graph = knowledge_sub.add_parser("graph", help="run Graphify and optionally archive its reports")
    knowledge_graph.add_argument("--dry-run", action="store_true", help="print the Graphify command without running it")
    knowledge_graph.add_argument("--no-archive", action="store_true", help="do not archive Graphify output under docs/_graphify/runs")
    knowledge_graph.add_argument("--no-trit", action="store_true", help="skip the Trit .trit graph augmentation pass")
    knowledge_graph.add_argument("--timeout", type=int, default=600, help="Graphify timeout in seconds")
    knowledge_graph.add_argument("--json", action="store_true", help="emit JSON report")
    knowledge_graph.set_defaults(func=cmd_knowledge_graph)

    website = sub.add_parser("website", help="validate TreatCode platform plans and domain contracts")
    website_sub = website.add_subparsers(dest="website_command", required=True)

    website_plans = website_sub.add_parser("plans", help="validate the complete TreatCode plan manifest")
    website_plans_sub = website_plans.add_subparsers(dest="plans_command", required=True)
    website_plans_validate = website_plans_sub.add_parser("validate", help="validate plan coverage, dependencies, and commands")
    website_plans_validate.add_argument("--manifest", default=None, help="plan manifest path")
    website_plans_validate.add_argument("--index", default=None, help="plan index path")
    website_plans_validate.add_argument("--json", action="store_true", help="emit JSON report")
    website_plans_validate.set_defaults(func=cmd_website_plans_validate)

    website_plan = website_sub.add_parser("plan", help="verify one TreatCode plan")
    website_plan_sub = website_plan.add_subparsers(dest="plan_command", required=True)
    website_plan_verify = website_plan_sub.add_parser("verify", help="run the plan's machine verification gates")
    website_plan_verify.add_argument("plan_id", help="plan id such as P01")
    website_plan_verify.add_argument("--manifest", default=None, help="plan manifest path")
    website_plan_verify.add_argument("--index", default=None, help="plan index path")
    website_plan_verify.add_argument("--output", default=None, help="evidence output directory")
    website_plan_verify.add_argument("--no-run", action="store_true", help="record commands without executing them")
    website_plan_verify.add_argument("--allow-incomplete", action="store_true", help="return success for machine-passing pre-completion verification")
    website_plan_verify.add_argument("--timeout", type=int, default=300, help="per-command timeout in seconds")
    website_plan_verify.add_argument("--json", action="store_true", help="emit JSON report")
    website_plan_verify.set_defaults(func=cmd_website_plan_verify)

    website_operations = website_sub.add_parser("operations", help="run TreatCode operations and recovery exercises")
    website_operations_sub = website_operations.add_subparsers(dest="operations_action", required=True)
    website_operations_recovery = website_operations_sub.add_parser("disaster-recovery-test", help="exercise backup, clean restore, and recovery objectives")
    website_operations_recovery.add_argument("--timeout", type=int, default=300, help="exercise timeout in seconds")
    website_operations_recovery.add_argument("--json", action="store_true", help="emit JSON report")
    website_operations_recovery.set_defaults(func=cmd_website_operations)

    website_benchmarks = website_sub.add_parser("benchmarks", help="validate P10 benchmark protocols and reference results")
    website_benchmarks_sub = website_benchmarks.add_subparsers(dest="benchmarks_command", required=True)
    website_benchmarks_verify = website_benchmarks_sub.add_parser("verify-reference", help="verify the checked-in P10 reference result")
    website_benchmarks_verify.add_argument("--manifest", default=None, help="P10 benchmark manifest path")
    website_benchmarks_verify.add_argument("--reference", default=None, help="P10 reference result path")
    website_benchmarks_verify.add_argument("--output", default=None, help="verification evidence output path")
    website_benchmarks_verify.add_argument("--json", action="store_true", help="emit JSON report")
    website_benchmarks_verify.set_defaults(func=cmd_website_benchmarks_verify_reference)

    website_registry = website_sub.add_parser("registry", help="validate stack, capability, contract, and decision registries")
    website_registry_sub = website_registry.add_subparsers(dest="registry_command", required=True)
    website_registry_validate = website_registry_sub.add_parser("validate", help="validate all TreatCode registry manifests")
    website_registry_validate.add_argument("--json", action="store_true", help="emit JSON report")
    website_registry_validate.set_defaults(func=cmd_website_registry_validate)
    website_registry_coverage = website_registry_sub.add_parser("coverage", help="validate stack coverage dimensions")
    website_registry_coverage.add_argument("--strict", action="store_true", help="reject planned implementation dimensions")
    website_registry_coverage.add_argument("--json", action="store_true", help="emit JSON report")
    website_registry_coverage.set_defaults(func=cmd_website_registry_coverage)

    website_index = website_sub.add_parser("index", help="build and inspect the commit-addressed repository intelligence index")
    website_index_sub = website_index.add_subparsers(dest="index_command", required=True)

    website_index_build = website_index_sub.add_parser("build", help="build a clean or incremental repository index")
    website_index_build.add_argument("--clean", action="store_true", help="ignore any existing index and rebuild all records")
    website_index_build.add_argument("--output", default=None, help="index directory or JSON path")
    website_index_build.add_argument("--repo-root", default=None, help="repository root (defaults to the Trit root)")
    website_index_build.add_argument("--json", action="store_true", help="emit JSON report")
    website_index_build.set_defaults(func=cmd_website_index_build)

    website_index_verify = website_index_sub.add_parser("verify", help="verify coverage, source spans, hashes, authority, and freshness")
    website_index_verify.add_argument("--index", default=None, help="index directory or JSON path")
    website_index_verify.add_argument("--evidence", default=None, help="evidence output directory")
    website_index_verify.add_argument("--repo-root", default=None, help="repository root (defaults to the Trit root)")
    website_index_verify.add_argument("--json", action="store_true", help="emit JSON report")
    website_index_verify.set_defaults(func=cmd_website_index_verify)

    website_index_compare = website_index_sub.add_parser("compare-clean-incremental", help="prove clean and incremental indexes are equivalent")
    website_index_compare.add_argument("--evidence", default=None, help="evidence output directory")
    website_index_compare.add_argument("--repo-root", default=None, help="repository root (defaults to the Trit root)")
    website_index_compare.add_argument("--json", action="store_true", help="emit JSON report")
    website_index_compare.set_defaults(func=cmd_website_index_compare)

    for context_name in ("context", "context-package"):
        website_index_context = website_index_sub.add_parser(context_name, help="generate a bounded context package for files, symbols, or records")
        website_index_context.add_argument("--scope", action="append", required=True, help="file path, file id, symbol id/name, or manifest record id; repeat for multiple scopes")
        website_index_context.add_argument("--index", default=None, help="index directory or JSON path")
        website_index_context.add_argument("--output", default=None, help="context package JSON path")
        website_index_context.add_argument("--repo-root", default=None, help="repository root (defaults to the Trit root)")
        website_index_context.add_argument("--json", action="store_true", help="emit JSON report")
        website_index_context.set_defaults(func=cmd_website_index_context)

    website_schemas = website_sub.add_parser("schemas", help="validate versioned platform schemas and fixtures")
    website_schemas_sub = website_schemas.add_subparsers(dest="schemas_command", required=True)
    website_schemas_validate = website_schemas_sub.add_parser("validate", help="validate the schema catalog")
    website_schemas_validate.add_argument("--json", action="store_true", help="emit JSON report")
    website_schemas_validate.set_defaults(func=cmd_website, website_action="schemas_validate")
    website_schemas_fixtures = website_schemas_sub.add_parser("test-fixtures", help="run valid and negative schema fixtures")
    website_schemas_fixtures.add_argument("--json", action="store_true", help="emit JSON report")
    website_schemas_fixtures.set_defaults(func=cmd_website, website_action="schemas_fixtures")

    website_challenges = website_sub.add_parser("challenges", help="validate the versioned challenge manifest")
    website_challenges_sub = website_challenges.add_subparsers(dest="challenges_command", required=True)
    website_challenges_validate = website_challenges_sub.add_parser("validate", help="validate challenge lifecycle, facets, contracts, and pilots")
    website_challenges_validate.add_argument("--manifest", default=None, help="challenge manifest path")
    website_challenges_validate.add_argument("--json", action="store_true", help="emit JSON report")
    website_challenges_validate.set_defaults(func=cmd_website_challenges_validate)

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
