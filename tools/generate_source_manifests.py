#!/usr/bin/env python3
"""Derive stable JSON contract slices from source constants.

The root JSON files remain the authoritative contracts.  This tool only
derives values that are mechanically present in source and writes a checked-in
sidecar under ``generated/``.  Policy prose (service groups/status/notes,
filesystem layout, payload field descriptions, and validation commands) stays
hand-authored in the root manifests.

``--check`` is the safe/default operation.  It verifies source invariants,
checks the generated sidecar, and compares mechanically-derived fields with
the authoritative root manifests.  ``--write`` is explicit and only refreshes
the sidecar; it never rewrites a root manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable


REPO = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = REPO / "generated" / "source_contract_manifest.json"
GENERATOR_VERSION = 1


class ContractError(ValueError):
    """A source or manifest contract cannot be derived safely."""


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ContractError(f"unable to read {path}: {exc}") from exc


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(_read(path))
    except json.JSONDecodeError as exc:
        raise ContractError(f"{path}: invalid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ContractError(f"{path}: expected a JSON object")
    return value


def _relative(root: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def _source_hash(path: Path) -> str:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ContractError(f"unable to hash {path}: {exc}") from exc
    return hashlib.sha256(data).hexdigest()


def _canonical_json(value: dict[str, Any]) -> str:
    # Keep insertion order (the schema is intentionally human-readable) while
    # using one stable indentation/newline policy on every host.
    return json.dumps(value, ensure_ascii=False, indent=2, separators=(",", ": ")) + "\n"


def _parse_cpp_constants(source: str, *, names: Iterable[str] | None = None) -> dict[str, str]:
    """Parse simple ``constexpr NAME = expression;`` declarations.

    This deliberately handles declarations only, not arbitrary C++
    expressions.  A duplicate declaration is an error rather than silently
    choosing the first/last value.
    """

    wanted = set(names) if names is not None else None
    pattern = re.compile(
        r"(?:inline\s+)?(?:static\s+)?constexpr\s+"
        r"[A-Za-z_][\w:<>]*\s+(?P<name>[A-Za-z_]\w*)\s*=\s*"
        r"(?P<expr>[^;]+);",
        re.MULTILINE,
    )
    values: dict[str, str] = {}
    for match in pattern.finditer(source):
        name = match.group("name")
        if wanted is not None and name not in wanted:
            continue
        expression = re.sub(r"//.*", "", match.group("expr")).strip()
        if name in values:
            raise ContractError(f"duplicate C++ constant {name}")
        values[name] = expression
    if wanted:
        missing = sorted(wanted - values.keys())
        if missing:
            raise ContractError("missing C++ constants: " + ", ".join(missing))
    return values


def _parse_int(text: str) -> int | None:
    token = text.strip()
    token = re.sub(r"(?:ULL|LLU|UL|LU|LL|L|U)\b", "", token, flags=re.IGNORECASE)
    if re.fullmatch(r"[-+]?0[xX][0-9a-fA-F]+", token):
        return int(token, 16)
    if re.fullmatch(r"[-+]?\d+", token):
        return int(token, 10)
    return None


def _resolve_value(
    expression: str,
    constants: dict[str, str],
    aliases: dict[str, int],
    *,
    stack: tuple[str, ...] = (),
) -> int:
    expression = expression.strip()
    direct = _parse_int(expression)
    if direct is not None:
        return direct

    shift = re.fullmatch(r"(.+?)\s*<<\s*(\d+)", expression)
    if shift:
        return _resolve_value(shift.group(1), constants, aliases, stack=stack) << int(shift.group(2))

    # C++ qualified names are reduced to their leaf because the source slices
    # intentionally contain no overloaded names.
    key = expression.replace(" ", "")
    key = key.removeprefix("sandbox::").removeprefix("architecture::v2::")
    key = key.removeprefix("vm::")
    if key in aliases:
        return aliases[key]
    if key not in constants:
        raise ContractError(f"unsupported constant expression: {expression!r}")
    if key in stack:
        raise ContractError("cyclic constant aliases: " + " -> ".join((*stack, key)))
    return _resolve_value(constants[key], constants, aliases, stack=(*stack, key))


def _parse_runtime_constants(root: Path) -> tuple[list[dict[str, Any]], dict[str, int], list[str]]:
    path = root / "ternary_compiler_ir.h"
    source = _read(path)
    match = re.search(r"namespace runtime\s*\{(?P<body>.*?)\}\s*// namespace runtime", source, re.S)
    if not match:
        raise ContractError("ternary_compiler_ir.h: runtime namespace not found")
    body = match.group("body")
    raw = _parse_cpp_constants(body)
    values: dict[str, int] = {}
    for name in raw:
        values[name] = _resolve_value(raw[name], raw, {})
    aliases: list[str] = []
    entries: list[dict[str, Any]] = []
    for name, expression in raw.items():
        target = expression.strip()
        target_key = target.removeprefix("runtime::")
        alias_of = target_key if target_key in raw and target_key != name else None
        if alias_of:
            aliases.append(name)
        entries.append(
            {
                "name": name,
                "id": values[name],
                "expression": expression,
                **({"alias_of": alias_of} if alias_of else {}),
            }
        )

    # Numeric IDs may repeat only for an explicit source alias.  This catches
    # accidental duplicate service IDs while preserving sys_yield aliases.
    by_id: dict[int, list[dict[str, Any]]] = {}
    for entry in entries:
        by_id.setdefault(entry["id"], []).append(entry)
    duplicate_errors: list[str] = []
    for service_id, same_id in sorted(by_id.items()):
        non_alias = [entry for entry in same_id if "alias_of" not in entry]
        if len(non_alias) > 1:
            duplicate_errors.append(
                f"runtime syscall id {service_id} has multiple canonical constants: "
                + ", ".join(entry["name"] for entry in non_alias)
            )
    if duplicate_errors:
        raise ContractError("; ".join(duplicate_errors))
    return entries, values, sorted(aliases)


def _parse_compiler_wrappers(root: Path, runtime_values: dict[str, int]) -> list[dict[str, Any]]:
    path = root / "ternary_compiler_codegen.h"
    source = _read(path)
    body_match = re.search(
        r"static\s+bool\s+isRuntimeName\s*\([^)]*\)\s*\{(?P<body>.*?)\n\s*\}",
        source,
        re.S,
    )
    if not body_match:
        raise ContractError("ternary_compiler_codegen.h: isRuntimeName body not found")
    wrapper_names = set(re.findall(r'name\s*==\s*"([A-Za-z_]\w*)"', body_match.group("body")))

    mapping_match = re.search(
        r"static\s+int\s+runtimeService\s*\([^)]*\)\s*\{(?P<body>.*?)\n\s*\}",
        source,
        re.S,
    )
    if not mapping_match:
        raise ContractError("ternary_compiler_codegen.h: runtimeService body not found")
    mapping_body = mapping_match.group("body")
    mapping: dict[str, str] = {}
    mapping_pattern = re.compile(
        r'name\s*==\s*"(?P<name>[A-Za-z_]\w*)"\s*\)\s*'
        r'(?:\{\s*)?return\s+runtime::(?P<target>[A-Za-z_]\w*)\s*;',
        re.S,
    )
    for match in mapping_pattern.finditer(mapping_body):
        name = match.group("name")
        target = match.group("target")
        if name in mapping:
            raise ContractError(f"duplicate compiler runtime wrapper {name}")
        if target not in runtime_values:
            raise ContractError(f"compiler wrapper {name} targets missing runtime constant {target}")
        mapping[name] = target

    missing_mapping = sorted(wrapper_names - mapping.keys())
    extra_mapping = sorted(mapping.keys() - wrapper_names)
    if missing_mapping or extra_mapping:
        detail = []
        if missing_mapping:
            detail.append("allowlist entries without runtimeService mapping: " + ", ".join(missing_mapping))
        if extra_mapping:
            detail.append("runtimeService entries missing from allowlist: " + ", ".join(extra_mapping))
        raise ContractError("; ".join(detail))

    return [
        {"name": name, "target": mapping[name], "id": runtime_values[mapping[name]]}
        for name in sorted(mapping)
    ]


def _parse_legacy_vm_constants(root: Path, runtime_values: dict[str, int]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    path = root / "ternary_vm_state.h"
    source = _read(path)
    block_match = re.search(
        r"static\s+constexpr\s+int\s+SYSCALL_WRITE_INT\b.*?\n\s*static\s+constexpr\s+int\s+EXEC_MAGIC",
        source,
        re.S,
    )
    if not block_match:
        raise ContractError("ternary_vm_state.h: legacy syscall constants not found")
    raw = _parse_cpp_constants(block_match.group(0))
    values = {name: _resolve_value(expr, raw, {}) for name, expr in raw.items()}
    entries = [{"name": name, "id": values[name], "expression": raw[name]} for name in raw]
    by_id: dict[int, list[str]] = {}
    for name, value in values.items():
        by_id.setdefault(value, []).append(name)
    duplicates = [
        f"legacy VM syscall id {value} has duplicate constants: {', '.join(names)}"
        for value, names in sorted(by_id.items())
        if len(names) > 1
    ]
    if duplicates:
        raise ContractError("; ".join(duplicates))

    # The VM header's early SYSCALL_* values are retained for an older host
    # surface.  Report conflicts explicitly, but do not fail generation: the
    # v2 compiler runtime namespace and root SYSCALL_MANIFEST are authoritative.
    legacy_to_runtime = {
        "SYSCALL_WRITE_INT": "sys_write_int",
        "SYSCALL_NEWLINE": "sys_newline",
        "SYSCALL_CLEAR_CONSOLE": "sys_clear",
        "SYSCALL_YIELD": "sys_yield",
        "SYSCALL_SLEEP_UNTIL_TICK": "sys_sleep_until_tick",
        "SYSCALL_EXIT": "sys_exit",
        "SYSCALL_GETPID": "sys_getpid",
        "SYSCALL_UPTIME": "sys_uptime",
        "SYSCALL_READ_INPUT": "sys_read_console_word",
        "SYSCALL_SPAWN": "sys_spawn_static",
        "SYSCALL_WAITPID": "sys_waitpid",
        "SYSCALL_OPEN": "sys_open",
        "SYSCALL_CLOSE": "sys_close",
        "SYSCALL_READ": "sys_read",
        "SYSCALL_WRITE": "sys_write",
        "SYSCALL_STAT": "sys_stat",
        "SYSCALL_READDIR": "sys_readdir",
        "SYSCALL_BRK": "sys_brk",
        "SYSCALL_SBRK": "sys_sbrk",
        "SYSCALL_FORK": "sys_fork",
        "SYSCALL_EXEC": "sys_exec",
        "SYSCALL_WRITE_CHAR": "sys_write_char",
        "SYSCALL_FSYNC": "sys_fsync",
        "SYSCALL_KILL": "sys_kill",
        "SYSCALL_SUSPEND": "sys_suspend",
        "SYSCALL_RESUME": "sys_resume",
        "SYSCALL_GETPROC": "sys_getproc",
        "SYSCALL_FUTEX_WAIT": "sys_futex_wait",
        "SYSCALL_FUTEX_WAKE": "sys_futex_wake",
        "SYSCALL_IPC_RECV_BLOCKING": "sys_ipc_recv_blocking",
        "SYSCALL_WAIT_EVENT": "sys_wait_event",
        "SYSCALL_SLEEP_MS": "sys_sleep_ms",
        "SYSCALL_APP_SPAWN": "sys_app_spawn",
        "SYSCALL_REBOOT": "sys_reboot",
        "SYSCALL_RENAME": "sys_rename",
    }
    conflicts = []
    for legacy_name, runtime_name in sorted(legacy_to_runtime.items()):
        if legacy_name not in values or runtime_name not in runtime_values:
            continue
        if values[legacy_name] != runtime_values[runtime_name]:
            conflicts.append(
                {
                    "constant": legacy_name,
                    "id": values[legacy_name],
                    "v2_name": runtime_name,
                    "v2_id": runtime_values[runtime_name],
                }
            )
    return entries, conflicts


def _parse_apps(root: Path) -> list[dict[str, Any]]:
    path = root / "build_tos_image.cpp"
    source = _read(path)
    stack_values: dict[str, int] = {}
    for match in re.finditer(r"constexpr\s+int\s+(k[A-Za-z_]\w*)\s*=\s*(\d+)\s*;", source):
        stack_values[match.group(1)] = int(match.group(2))
    vector_match = re.search(
        r"std::vector<BundledApp>\s+apps\s*=\s*\{(?P<body>.*?)\n\s*\};",
        source,
        re.S,
    )
    if not vector_match:
        raise ContractError("build_tos_image.cpp: BundledApp vector not found")
    row_pattern = re.compile(
        r"\{\s*\"(?P<source>[^\"]+)\"\s*,\s*\"(?P<id>[^\"]+)\"\s*,\s*"
        r"\"(?P<title>[^\"]*)\"\s*,\s*\"(?P<path>[^\"]+)\"\s*,\s*"
        r"(?P<ppn>[^,]+)\s*,\s*(?P<stack>[^,]+)\s*,\s*(?P<gui>true|false)\s*\}",
        re.S,
    )
    rows = []
    for match in row_pattern.finditer(vector_match.group("body")):
        stack_expr = match.group("stack").strip()
        stack_value = _parse_int(stack_expr)
        if stack_value is None:
            stack_value = stack_values.get(stack_expr)
        if stack_value is None:
            raise ContractError(f"build_tos_image.cpp: unknown app stack constant {stack_expr}")
        ppn = _parse_int(match.group("ppn"))
        if ppn is None:
            raise ContractError(f"build_tos_image.cpp: unsupported app text PPN {match.group('ppn').strip()}")
        source_name = match.group("source")
        rows.append(
            {
                "source": f"apps/{source_name}.trit",
                "id": match.group("id"),
                "title": match.group("title"),
                "guest_path": match.group("path"),
                "text_ppn": ppn,
                "stack_words": stack_value,
                "gui_registry": match.group("gui") == "true",
            }
        )
    if not rows:
        raise ContractError("build_tos_image.cpp: BundledApp vector has no parseable entries")
    initializer_count = vector_match.group("body").count("{")
    if initializer_count != len(rows):
        raise ContractError(
            "build_tos_image.cpp: BundledApp vector contains an unparseable initializer "
            f"({initializer_count} rows found, {len(rows)} parsed)"
        )
    by_id: dict[str, int] = {}
    by_path: dict[str, int] = {}
    for index, row in enumerate(rows):
        if row["id"] in by_id:
            raise ContractError(f"duplicate BundledApp id {row['id']} (rows {by_id[row['id']]} and {index})")
        if row["guest_path"] in by_path:
            raise ContractError(
                f"duplicate BundledApp guest path {row['guest_path']} "
                f"(rows {by_path[row['guest_path']]} and {index})"
            )
        by_id[row["id"]] = index
        by_path[row["guest_path"]] = index
        if not (root / row["source"]).is_file():
            raise ContractError(f"BundledApp {row['id']} references missing source {row['source']}")
    return rows


def _parse_image_constants(root: Path) -> dict[str, Any]:
    arch = _load_json(root / "ARCHITECTURE_MANIFEST.json")
    try:
        architecture = arch["architecture"]
        formats = arch["formats"]
    except KeyError as exc:
        raise ContractError(f"ARCHITECTURE_MANIFEST.json missing {exc}") from exc
    aliases = {
        "TBOOT_WRITE_VERSION": int(formats["tboot_write_version"]),
        "TDISK_WRITE_VERSION": int(formats["tdisk_write_version"]),
        "STORAGE_BLOCK_WORDS": int(architecture["storage_block_words"]),
        "BASE_PAGE_WORDS": int(architecture["base_page_words"]),
        "SUPERPAGE_WORDS": int(architecture["superpage_words"]),
    }
    host_path = root / "ternary_host_runtime.h"
    vm_path = root / "ternary_vm_state.h"
    host_names = {
        "TOS_BOOT_MAGIC",
        "TOS_BOOT_FORMAT_VERSION",
        "TOS_SPARSE_DISK_MAGIC",
        "TOS_LEGACY_SPARSE_DISK_MAGIC",
        "TOS_SPARSE_DISK_VERSION",
        "TOS_IMAGE_SECTION_EXECUTABLE",
        "TOS_IMAGE_SECTION_KERNEL",
        "TOS_IMAGE_SECTION_APP",
    }
    host_raw = _parse_cpp_constants(_read(host_path), names=host_names)
    vm_names = {"STORAGE_BLOCK_WORDS", "MMU_PAGE_WORDS", "MMU_SUPERPAGE_WORDS", "kSparseDiskV2Magic", "kSparseDiskVersion"}
    vm_raw = _parse_cpp_constants(_read(vm_path), names=vm_names)
    overlap = sorted(set(host_raw) & set(vm_raw))
    if overlap:
        raise ContractError("duplicate image constants across host/vm sources: " + ", ".join(overlap))
    all_raw = {**host_raw, **vm_raw}
    values = {name: _resolve_value(expr, all_raw, aliases) for name, expr in all_raw.items()}
    return {
        "tboot": {
            "magic_hex": f"0x{values['TOS_BOOT_MAGIC']:x}",
            "write_version": values["TOS_BOOT_FORMAT_VERSION"],
        },
        "tdisk": {
            "magic_hex": f"0x{values['TOS_SPARSE_DISK_MAGIC']:x}",
            "write_version": values["TOS_SPARSE_DISK_VERSION"],
            "legacy_magic_hex": f"0x{values['TOS_LEGACY_SPARSE_DISK_MAGIC']:x}",
            "block_words": values["STORAGE_BLOCK_WORDS"],
        },
        "section_flags": {
            "executable": values["TOS_IMAGE_SECTION_EXECUTABLE"],
            "kernel": values["TOS_IMAGE_SECTION_KERNEL"],
            "app": values["TOS_IMAGE_SECTION_APP"],
        },
        "vm_duplicates": {
            "sparse_disk_magic": f"0x{values['kSparseDiskV2Magic']:x}",
            "sparse_disk_version": values["kSparseDiskVersion"],
            "page_words": values["MMU_PAGE_WORDS"],
            "superpage_words": values["MMU_SUPERPAGE_WORDS"],
        },
    }


def derive(root: Path) -> dict[str, Any]:
    """Derive the stable source-contract sidecar for ``root``."""

    root = root.resolve()
    runtime_entries, runtime_values, aliases = _parse_runtime_constants(root)
    wrappers = _parse_compiler_wrappers(root, runtime_values)
    legacy_entries, legacy_conflicts = _parse_legacy_vm_constants(root, runtime_values)
    apps = _parse_apps(root)
    image = _parse_image_constants(root)
    source_paths = [
        "ternary_compiler_ir.h",
        "ternary_compiler_codegen.h",
        "ternary_vm_state.h",
        "build_tos_image.cpp",
        "ternary_host_runtime.h",
        "ARCHITECTURE_MANIFEST.json",
    ]
    return {
        "version": 1,
        "schema": "trit.source_contract_manifest.v1",
        "generated_by": "tools/generate_source_manifests.py",
        "generator_version": GENERATOR_VERSION,
        "authoritative_manifests": [
            "SYSCALL_MANIFEST.json",
            "APP_MANIFEST.json",
            "IMAGE_FORMAT_MANIFEST.json",
        ],
        "sources": [
            {"path": path, "sha256": _source_hash(root / path)} for path in source_paths
        ],
        "syscalls": {
            "runtime_constants": sorted(runtime_entries, key=lambda item: item["name"]),
            "compiler_wrappers": wrappers,
            "compiler_aliases": aliases,
            "legacy_vm_constants": sorted(legacy_entries, key=lambda item: item["name"]),
            "legacy_conflicts": legacy_conflicts,
        },
        "apps": {
            "bundled_apps": apps,
            "count": len(apps),
            "gui_count": sum(1 for app in apps if app["gui_registry"]),
        },
        "image_formats": image,
        "notes": {
            "authority": "Root manifests, source files, and tests remain authoritative.",
            "hand_authored": [
                "SYSCALL_MANIFEST service groups, statuses, notes, and calling-convention prose",
                "APP_MANIFEST root layout, default user, registry format, supporting sources, and validation commands",
                "IMAGE_FORMAT_MANIFEST payload fields, runtime policy, migration policy, and tool descriptions",
                "ternary_vm_state.h legacy SYSCALL_* names (reported as conflicts, not rewritten)",
            ],
        },
    }


def _service_errors(root: Path, derived: dict[str, Any]) -> list[str]:
    data = _load_json(root / "SYSCALL_MANIFEST.json")
    services = data.get("services")
    if not isinstance(services, list):
        return ["SYSCALL_MANIFEST.json: services must be an array"]
    errors: list[str] = []
    by_name: dict[str, dict[str, Any]] = {}
    by_id: dict[int, str] = {}
    for index, service in enumerate(services):
        if not isinstance(service, dict) or not isinstance(service.get("name"), str) or not isinstance(service.get("id"), int):
            errors.append(f"SYSCALL_MANIFEST.json: invalid service entry at index {index}")
            continue
        name = service["name"]
        service_id = service["id"]
        if name in by_name:
            errors.append(f"SYSCALL_MANIFEST.json: duplicate service name {name}")
        if service_id in by_id:
            errors.append(f"SYSCALL_MANIFEST.json: duplicate service id {service_id}")
        by_name[name] = service
        by_id[service_id] = name

    source_entries = derived["syscalls"]["runtime_constants"]
    source_map = {entry["name"]: entry for entry in source_entries}
    source_aliases = {entry["name"] for entry in source_entries if "alias_of" in entry}
    root_aliases = data.get("compiler_aliases", {})
    if not isinstance(root_aliases, dict):
        errors.append("SYSCALL_MANIFEST.json: compiler_aliases must be an object")
        root_aliases = {}
    for entry in source_entries:
        name = entry["name"]
        if name in source_aliases:
            alias_target = entry["alias_of"]
            if root_aliases.get(name) != alias_target:
                errors.append(f"SYSCALL_MANIFEST.json: compiler alias {name} must target {alias_target}")
            continue
        service = by_name.get(name)
        if service is None:
            errors.append(f"SYSCALL_MANIFEST.json: missing source service {name}")
        elif service.get("id") != entry["id"]:
            errors.append(
                f"SYSCALL_MANIFEST.json: service {name} id {service.get('id')} differs from source {entry['id']}"
            )
    for name, target in root_aliases.items():
        entry = source_map.get(name)
        if entry is None or entry.get("alias_of") != target:
            errors.append(f"SYSCALL_MANIFEST.json: alias {name}={target!r} is not a source alias")

    allowed_legacy = {"reserved_or_legacy", "permanently_reserved"}
    for service in services:
        if not isinstance(service, dict):
            continue
        if service.get("name") not in source_map and service.get("status") not in allowed_legacy:
            errors.append(
                f"SYSCALL_MANIFEST.json: service {service.get('name')} has no source constant "
                "and is not marked reserved/legacy"
            )
    return errors


def _app_errors(root: Path, derived: dict[str, Any]) -> list[str]:
    data = _load_json(root / "APP_MANIFEST.json")
    manifest_apps = data.get("bundled_apps")
    if not isinstance(manifest_apps, list):
        return ["APP_MANIFEST.json: bundled_apps must be an array"]
    errors: list[str] = []
    root_by_id: dict[str, dict[str, Any]] = {}
    root_by_path: dict[str, str] = {}
    for index, app in enumerate(manifest_apps):
        if not isinstance(app, dict) or not isinstance(app.get("id"), str):
            errors.append(f"APP_MANIFEST.json: invalid bundled app at index {index}")
            continue
        app_id = app["id"]
        guest_path = app.get("guest_path")
        if app_id in root_by_id:
            errors.append(f"APP_MANIFEST.json: duplicate app id {app_id}")
        if isinstance(guest_path, str) and guest_path in root_by_path:
            errors.append(f"APP_MANIFEST.json: duplicate guest path {guest_path}")
        root_by_id[app_id] = app
        if isinstance(guest_path, str):
            root_by_path[guest_path] = app_id

    source_apps = derived["apps"]["bundled_apps"]
    for source_app in source_apps:
        app_id = source_app["id"]
        app = root_by_id.get(app_id)
        if app is None:
            errors.append(f"APP_MANIFEST.json: missing builder app {app_id}")
            continue
        for key in ("source", "title", "guest_path", "stack_words", "gui_registry"):
            if app.get(key) != source_app[key]:
                errors.append(
                    f"APP_MANIFEST.json: app {app_id} {key}={app.get(key)!r} differs from builder {source_app[key]!r}"
                )
    source_ids = {app["id"] for app in source_apps}
    for app_id in sorted(set(root_by_id) - source_ids):
        errors.append(f"APP_MANIFEST.json: app {app_id} has no builder entry")
    return errors


def _image_errors(root: Path, derived: dict[str, Any]) -> list[str]:
    data = _load_json(root / "IMAGE_FORMAT_MANIFEST.json")
    errors: list[str] = []
    formats = data.get("formats")
    if not isinstance(formats, dict):
        return ["IMAGE_FORMAT_MANIFEST.json: formats must be an object"]
    source = derived["image_formats"]
    tboot = formats.get("tboot", {})
    tdisk = formats.get("tdisk", {})
    checks = [
        ("formats.tboot.magic_hex", tboot.get("magic_hex"), source["tboot"]["magic_hex"]),
        ("formats.tboot.write_version", tboot.get("write_version"), source["tboot"]["write_version"]),
        ("formats.tdisk.magic_hex", tdisk.get("magic_hex"), source["tdisk"]["magic_hex"]),
        ("formats.tdisk.write_version", tdisk.get("write_version"), source["tdisk"]["write_version"]),
        ("formats.tdisk.legacy_magic_hex", tdisk.get("legacy_magic_hex"), source["tdisk"]["legacy_magic_hex"]),
        ("formats.tdisk.block_words", tdisk.get("block_words"), source["tdisk"]["block_words"]),
    ]
    for path, actual, expected in checks:
        if actual != expected:
            errors.append(f"IMAGE_FORMAT_MANIFEST.json: {path}={actual!r} differs from source {expected!r}")
    return errors


def compare_roots(root: Path, derived: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    for checker in (_service_errors, _app_errors, _image_errors):
        try:
            errors.extend(checker(root, derived))
        except ContractError as exc:
            errors.append(str(exc))
    return errors


def check(root: Path, output: Path) -> tuple[list[str], list[str]]:
    """Return (errors, non-fatal warnings) for the current tree."""

    errors: list[str] = []
    warnings: list[str] = []
    try:
        derived = derive(root)
    except ContractError as exc:
        return [str(exc)], []
    warnings.extend(
        f"legacy VM constant conflict: {item['constant']}={item['id']} vs "
        f"v2 {item['v2_name']}={item['v2_id']}"
        for item in derived["syscalls"]["legacy_conflicts"]
    )
    expected = _canonical_json(derived)
    if not output.is_file():
        errors.append(f"generated sidecar is missing: {_relative(root, output)}")
    else:
        try:
            actual = output.read_text(encoding="utf-8")
        except OSError as exc:
            errors.append(f"unable to read generated sidecar {output}: {exc}")
        else:
            if actual != expected:
                errors.append(f"generated sidecar drift: {_relative(root, output)}")
    errors.extend(compare_roots(root, derived))
    return errors, warnings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="verify source, sidecar, and root manifest drift (default)")
    mode.add_argument("--write", action="store_true", help="write the generated sidecar; never rewrites root manifests")
    parser.add_argument("--repo", type=Path, default=REPO, help="repository root (for isolated fixtures)")
    parser.add_argument("--output", type=Path, default=None, help="sidecar path (defaults to generated/source_contract_manifest.json)")
    args = parser.parse_args(argv)
    root = args.repo.resolve()
    output = (args.output if args.output is not None else root / "generated" / "source_contract_manifest.json").resolve()
    if not args.write:
        errors, warnings = check(root, output)
        for warning in warnings:
            print("warning: " + warning, file=sys.stderr)
        if errors:
            for error in errors:
                print("source manifest drift: " + error, file=sys.stderr)
            return 1
        print(f"source manifest sidecar is current: {_relative(root, output)}")
        return 0

    try:
        derived = derive(root)
    except ContractError as exc:
        print(f"source manifest generation failed: {exc}", file=sys.stderr)
        return 1
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(_canonical_json(derived), encoding="utf-8", newline="\n")
    print(f"wrote {_relative(root, output)}")
    for item in derived["syscalls"]["legacy_conflicts"]:
        print(
            f"warning: legacy VM constant conflict: {item['constant']}={item['id']} vs "
            f"v2 {item['v2_name']}={item['v2_id']}",
            file=sys.stderr,
        )
    root_errors = compare_roots(root, derived)
    if root_errors:
        for error in root_errors:
            print("source manifest drift: " + error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
