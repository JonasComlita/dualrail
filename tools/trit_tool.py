#!/usr/bin/env python3
"""Agent-operable host tooling for Trit/Ternary OS.

This script intentionally avoids third-party packages so it can run before the
project itself is fully built. The PowerShell wrappers beside it expose the
stable command names documented in AGENTS.md.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import os
import platform
import re
import shutil
import statistics
import struct
import subprocess
import sys
import time
from urllib.parse import unquote
from pathlib import Path
from typing import Any


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
    "BENCHMARK_SCHEMA.json",
    "ROADMAP_STATUS.json",
    "TEST_MANIFEST.json",
    "SYSCALL_MANIFEST.json",
    "IMAGE_FORMAT_MANIFEST.json",
    "APP_MANIFEST.json",
]
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

    replay = sub.add_parser("replay", help="placeholder for future deterministic replay")
    replay.add_argument("trace", nargs="?")
    replay.set_defaults(func=cmd_replay)

    fuzz = sub.add_parser("fuzz", help="run the current fuzz stand-in")
    add_test_options(fuzz)
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
