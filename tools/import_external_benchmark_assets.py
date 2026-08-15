#!/usr/bin/env python3
"""Inventory and validate optional external Doom/BitNet benchmark assets.

This tool deliberately never downloads files.  A caller supplies an already
downloaded file or directory, and the tool records a deterministic inventory
(names, sizes, hashes and format metadata).  The checked-in provenance manifest
therefore remains useful on machines that do not have multi-gigabyte model
weights or a complete Doom WAD installed.

The importer understands three stages:

``metadata``
    A model-card/configuration slice with no weights.  This is useful for
    checking provenance without making a model-availability claim.
``sliced``
    One or more model shards.  Shard names such as
    ``model-00001-of-00003.safetensors`` are parsed and checked for duplicate
    indices and a consistent declared count.
``full``
    A complete WAD/model artifact.  Full means complete only when every file
    in the generated inventory is present and validates locally; the static
    provenance manifest intentionally marks its external entries as absent.

Validation is dependency-free and streams file hashes.  It does not parse
tensor payloads, execute model code, or invoke a renderer.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import struct
import sys
from pathlib import Path
from typing import Any, Iterable, Mapping


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "benchmarks" / "assets" / "external_assets.v1.json"
SCHEMA = "trit.external_benchmark_assets.v1"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SHARD_RE = re.compile(r"(?:^|[-_.])(?P<index>\d{1,6})[-_]of[-_](?P<count>\d{1,6})(?:[-_.]|$)", re.IGNORECASE)


class AssetError(ValueError):
    """A deterministic validation or import error."""


def sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(chunk_size)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _json_load(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AssetError(f"cannot read JSON {path}: {exc}") from exc


def _is_safe_relative(path: str) -> bool:
    candidate = Path(path)
    return not candidate.is_absolute() and ".." not in candidate.parts


def validate_wad_bytes(data: bytes) -> dict[str, Any]:
    """Validate the container/header/directory of a Doom WAD.

    This intentionally does not claim that a WAD is playable by a particular
    engine.  It only proves that the on-disk container is structurally safe to
    hand to a renderer.
    """

    issues: list[str] = []
    if len(data) < 12:
        return {"ok": False, "issues": ["WAD is shorter than its 12-byte header"]}
    magic = data[:4]
    if magic not in (b"IWAD", b"PWAD"):
        issues.append("WAD magic must be IWAD or PWAD")
    lump_count, directory_offset = struct.unpack_from("<II", data, 4)
    directory_bytes = lump_count * 16
    if directory_offset > len(data) or directory_bytes > len(data) - directory_offset:
        issues.append("WAD directory extends beyond file")
    lumps: list[dict[str, Any]] = []
    if not issues:
        for index in range(lump_count):
            offset = directory_offset + index * 16
            file_offset, size = struct.unpack_from("<II", data, offset)
            raw_name = data[offset + 8:offset + 16]
            try:
                name = raw_name.rstrip(b"\0").decode("ascii")
            except UnicodeDecodeError:
                issues.append(f"WAD lump {index} has a non-ASCII name")
                name = ""
            if file_offset > len(data) or size > len(data) - file_offset:
                issues.append(f"WAD lump {index} data extends beyond file")
            if not name or any(byte < 32 or byte > 126 for byte in raw_name.rstrip(b"\0")):
                issues.append(f"WAD lump {index} has an invalid name")
            lumps.append({"index": index, "name": name, "offset": file_offset, "size": size})
    return {
        "ok": not issues,
        "issues": issues,
        "format": magic.decode("ascii", errors="replace"),
        "lump_count": lump_count,
        "directory_offset": directory_offset,
        "lumps": lumps,
    }


def validate_wad(path: Path) -> dict[str, Any]:
    try:
        return validate_wad_bytes(path.read_bytes())
    except OSError as exc:
        return {"ok": False, "issues": [f"cannot read WAD {path}: {exc}"]}


def _read_safetensors_header(path: Path) -> dict[str, Any]:
    try:
        with path.open("rb") as stream:
            prefix = stream.read(8)
            if len(prefix) != 8:
                return {"ok": False, "issues": ["safetensors file is shorter than 8-byte header"]}
            (header_length,) = struct.unpack("<Q", prefix)
            file_size = path.stat().st_size
            if header_length > file_size - 8:
                return {"ok": False, "issues": ["safetensors header extends beyond file"]}
            if header_length > 128 * 1024 * 1024:
                return {"ok": False, "issues": ["safetensors header exceeds 128 MiB safety limit"]}
            raw_header = stream.read(header_length)
        header = json.loads(raw_header.decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError, struct.error) as exc:
        return {"ok": False, "issues": [f"invalid safetensors header: {exc}"]}
    if not isinstance(header, dict):
        return {"ok": False, "issues": ["safetensors header must be a JSON object"]}
    issues: list[str] = []
    payload_start = 8 + header_length
    tensors = []
    for name, metadata in header.items():
        if name == "__metadata__":
            continue
        if not isinstance(name, str) or not isinstance(metadata, dict):
            issues.append(f"tensor entry {name!r} is not an object")
            continue
        offsets = metadata.get("data_offsets")
        if not isinstance(offsets, list) or len(offsets) != 2 or not all(isinstance(item, int) for item in offsets):
            issues.append(f"tensor {name!r} has invalid data_offsets")
            continue
        start, end = offsets
        if start < 0 or end < start:
            issues.append(f"tensor {name!r} has descending data_offsets")
        if end > path.stat().st_size - payload_start:
            issues.append(f"tensor {name!r} payload extends beyond file")
        tensors.append({"name": name, "dtype": metadata.get("dtype"), "shape": metadata.get("shape"), "data_offsets": offsets})
    return {"ok": not issues, "format": "safetensors", "header_bytes": header_length, "tensor_count": len(tensors), "tensors": tensors, "issues": issues}


def _read_gguf_header(path: Path) -> dict[str, Any]:
    try:
        with path.open("rb") as stream:
            prefix = stream.read(24)
        if len(prefix) < 16:
            return {"ok": False, "issues": ["GGUF file is shorter than its fixed header"]}
        magic, version, tensor_count, metadata_count = struct.unpack_from("<4sIQQ", prefix)
    except (OSError, struct.error) as exc:
        return {"ok": False, "issues": [f"invalid GGUF header: {exc}"]}
    issues: list[str] = []
    if magic != b"GGUF":
        issues.append("GGUF magic must be GGUF")
    if version not in (1, 2, 3):
        issues.append(f"unsupported GGUF version {version}")
    return {"ok": not issues, "format": "gguf", "version": version, "tensor_count": tensor_count, "metadata_count": metadata_count, "issues": issues}


def validate_model_file(path: Path) -> dict[str, Any]:
    suffix = path.suffix.lower()
    if suffix == ".safetensors":
        result = _read_safetensors_header(path)
    elif suffix == ".gguf":
        result = _read_gguf_header(path)
    elif suffix in (".json", ".yaml", ".yml"):
        try:
            value = _json_load(path)
            result = {"ok": isinstance(value, dict), "format": "metadata-json", "issues": [] if isinstance(value, dict) else ["metadata JSON must be an object"]}
        except AssetError as exc:
            result = {"ok": False, "format": "metadata-json", "issues": [str(exc)]}
    else:
        result = {"ok": path.is_file() and path.stat().st_size > 0, "format": suffix.lstrip(".") or "binary", "issues": [] if path.is_file() and path.stat().st_size > 0 else ["model file is empty or missing"]}
    result["size"] = path.stat().st_size if path.exists() else 0
    result["sha256"] = sha256_file(path) if path.is_file() else None
    return result


def parse_shard_name(name: str) -> tuple[int, int] | None:
    match = SHARD_RE.search(name)
    if not match:
        return None
    return int(match.group("index")), int(match.group("count"))


def validate_shard_set(files: Iterable[Mapping[str, Any]], stage: str) -> list[str]:
    issues: list[str] = []
    files = list(files)
    parsed: list[tuple[int, int, str]] = []
    for entry in files:
        name = str(entry.get("name", entry.get("path", "")))
        parsed_name = parse_shard_name(name)
        if parsed_name:
            parsed.append((*parsed_name, name))
    if stage == "sliced" and not parsed:
        issues.append("sliced BitNet inventory must contain at least one numbered shard")
    if parsed:
        declared_counts = {count for _, count, _ in parsed}
        if len(declared_counts) != 1:
            issues.append("BitNet shard names declare inconsistent total counts")
        indices = [index for index, _, _ in parsed]
        if len(indices) != len(set(indices)):
            issues.append("BitNet shard inventory contains duplicate indices")
        if any(index < 1 or index > count for index, count, _ in parsed):
            issues.append("BitNet shard index is outside its declared range")
        if stage == "full":
            count = next(iter(declared_counts))
            if sorted(indices) != list(range(1, count + 1)):
                issues.append("full BitNet shard inventory is missing one or more numbered shards")
    elif stage == "full" and not files:
        issues.append("full BitNet inventory must contain at least one model file")
    return issues


def _file_entry(path: Path, display_name: str | None = None) -> dict[str, Any]:
    name = display_name or path.name
    entry: dict[str, Any] = {"name": name, "size": path.stat().st_size, "sha256": sha256_file(path), "present": True}
    if path.suffix.lower() == ".wad":
        entry["validation"] = validate_wad(path)
    elif path.suffix.lower() in (".safetensors", ".gguf", ".json", ".yaml", ".yml"):
        entry["validation"] = validate_model_file(path)
    else:
        entry["validation"] = {"ok": path.stat().st_size > 0, "format": path.suffix.lstrip(".") or "binary", "issues": [] if path.stat().st_size > 0 else ["file is empty"]}
    return entry


def _collect_source_files(source: Path, kind: str) -> list[Path]:
    if not source.exists():
        raise AssetError(f"source does not exist: {source}")
    if source.is_file():
        return [source]
    suffixes = {"doom": {".wad"}, "bitnet": {".safetensors", ".gguf", ".json", ".yaml", ".yml", ".bin", ".t40"}}[kind]
    return sorted((path for path in source.rglob("*") if path.is_file() and path.suffix.lower() in suffixes), key=lambda item: item.as_posix().lower())


def import_source(source: Path, *, kind: str, stage: str, asset_id: str, output_dir: Path | None = None, copy: bool = False, source_url: str = "", license_name: str = "") -> dict[str, Any]:
    paths = _collect_source_files(source, kind)
    if not paths:
        raise AssetError(f"no {kind} files found under {source}")
    if stage == "metadata" and kind != "bitnet":
        raise AssetError("metadata stage is only valid for BitNet metadata/configuration")
    if stage == "metadata" and any(path.suffix.lower() not in {".json", ".yaml", ".yml"} for path in paths):
        raise AssetError("metadata stage may only contain JSON/YAML files")
    target_root = (output_dir / asset_id) if output_dir else None
    entries: list[dict[str, Any]] = []
    for path in paths:
        relative_name = path.name if source.is_file() else path.relative_to(source).as_posix()
        target_name = relative_name
        if copy:
            if target_root is None:
                raise AssetError("--copy requires --output-dir")
            target = target_root / relative_name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            path_for_inventory = target
        else:
            path_for_inventory = path
        entry = _file_entry(path_for_inventory, relative_name)
        if not copy:
            entry["storage"] = "external"
        entries.append(entry)
    issues: list[str] = []
    for entry in entries:
        validation = entry.get("validation", {})
        if validation.get("ok") is not True:
            issues.extend(f"{entry['name']}: {item}" for item in validation.get("issues", ["validation failed"]))
    if kind == "bitnet":
        issues.extend(validate_shard_set(entries, stage))
    else:
        if stage == "full" and not any(path.suffix.lower() == ".wad" for path in paths):
            issues.append("full Doom inventory must contain a WAD")
    status = "present" if not issues else "invalid"
    return {"id": asset_id, "kind": kind, "stage": stage, "status": status, "source_url": source_url, "license": license_name, "files": entries, "issues": issues}


def validate_manifest(manifest: Mapping[str, Any], *, root: Path = ROOT, require_present: bool = False) -> list[str]:
    issues: list[str] = []
    if manifest.get("schema") != SCHEMA:
        issues.append(f"schema must be {SCHEMA}")
    if manifest.get("version") != 1:
        issues.append("version must be 1")
    assets = manifest.get("assets")
    if not isinstance(assets, list) or not assets:
        issues.append("assets must be a non-empty array")
        return issues
    seen: set[str] = set()
    for index, asset in enumerate(assets):
        prefix = f"assets[{index}]"
        if not isinstance(asset, dict):
            issues.append(f"{prefix} must be an object")
            continue
        asset_id = asset.get("id")
        if not isinstance(asset_id, str) or not asset_id:
            issues.append(f"{prefix}.id must be a non-empty string")
        elif asset_id in seen:
            issues.append(f"{prefix}.id is duplicated: {asset_id}")
        else:
            seen.add(asset_id)
        kind = asset.get("kind")
        if kind not in ("doom", "bitnet"):
            issues.append(f"{prefix}.kind must be doom or bitnet")
        stage = asset.get("stage")
        if stage not in ("metadata", "sliced", "full"):
            issues.append(f"{prefix}.stage must be metadata, sliced or full")
        status = asset.get("status")
        if status not in ("not_present", "metadata_only", "present", "invalid"):
            issues.append(f"{prefix}.status is not recognized")
        if not isinstance(asset.get("source_url"), str) or not asset.get("source_url"):
            issues.append(f"{prefix}.source_url must be recorded")
        if not isinstance(asset.get("license"), str) or not asset.get("license"):
            issues.append(f"{prefix}.license must be recorded")
        if stage == "metadata" and status == "present":
            issues.append(f"{prefix}.metadata assets must use status=metadata_only")
        if stage == "metadata" and status not in ("metadata_only", "not_present", "invalid"):
            issues.append(f"{prefix}.metadata status must be metadata_only, not_present or invalid")
        if status == "metadata_only" and stage != "metadata":
            issues.append(f"{prefix}.metadata_only status requires stage=metadata")
        files = asset.get("files")
        if not isinstance(files, list) or not files:
            issues.append(f"{prefix}.files must be a non-empty array")
            continue
        for file_index, item in enumerate(files):
            file_prefix = f"{prefix}.files[{file_index}]"
            if not isinstance(item, dict):
                issues.append(f"{file_prefix} must be an object")
                continue
            name = item.get("name")
            if not isinstance(name, str) or not name or not _is_safe_relative(name):
                issues.append(f"{file_prefix}.name must be a safe relative path")
            expected_size = item.get("size")
            if not isinstance(expected_size, int) or expected_size < 0:
                issues.append(f"{file_prefix}.size must be a non-negative integer")
            expected_hash = item.get("sha256")
            if expected_hash is not None and (not isinstance(expected_hash, str) or not SHA256_RE.fullmatch(expected_hash)):
                issues.append(f"{file_prefix}.sha256 must be null or a lowercase SHA-256 digest")
            present = item.get("present")
            if not isinstance(present, bool):
                issues.append(f"{file_prefix}.present must be boolean")
            if present:
                path = root / name
                if not path.is_file():
                    issues.append(f"{file_prefix} is marked present but is missing: {path}")
                else:
                    observed_size = path.stat().st_size
                    observed_hash = sha256_file(path)
                    if isinstance(expected_size, int) and observed_size != expected_size:
                        issues.append(f"{file_prefix} size mismatch: expected {expected_size}, observed {observed_size}")
                    if expected_hash and observed_hash != expected_hash:
                        issues.append(f"{file_prefix} hash mismatch")
                    if kind == "doom" and path.suffix.lower() == ".wad":
                        wad_result = validate_wad(path)
                        if wad_result.get("ok") is not True:
                            issues.extend(
                                f"{file_prefix}: {item}"
                                for item in wad_result.get("issues", ["invalid WAD"])
                            )
                    if kind == "bitnet":
                        model_result = validate_model_file(path)
                        if model_result.get("ok") is not True:
                            issues.extend(
                                f"{file_prefix}: {item}"
                                for item in model_result.get("issues", ["invalid model file"])
                            )
            elif require_present:
                issues.append(f"{file_prefix} is not present")
        if status == "present" and any(not item.get("present", False) for item in files if isinstance(item, dict)):
            issues.append(f"{prefix}.status=present requires every file to be present")
        if status == "not_present" and all(item.get("present", False) for item in files if isinstance(item, dict)):
            issues.append(f"{prefix}.status=not_present conflicts with present files")
        if kind == "bitnet":
            issues.extend(f"{prefix}: {item}" for item in validate_shard_set(files, str(stage)))
    return issues


def _write_json(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _command_validate(args: argparse.Namespace) -> int:
    path = Path(args.manifest)
    try:
        manifest = _json_load(path)
    except AssetError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    if not isinstance(manifest, dict):
        print("manifest must be a JSON object", file=sys.stderr)
        return 2
    root = Path(args.root).resolve() if args.root else path.parent.parent.parent.resolve()
    issues = validate_manifest(manifest, root=root, require_present=args.require_present)
    result = {"schema": "trit.external_benchmark_assets.validation.v1", "manifest": str(path), "ok": not issues, "issues": issues}
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    elif issues:
        print("external benchmark assets: FAIL")
        for issue in issues:
            print(f"- {issue}")
    else:
        print("external benchmark assets: PASS")
    return 0 if not issues else 1


def _command_import(args: argparse.Namespace) -> int:
    try:
        result = import_source(Path(args.source).resolve(), kind=args.kind, stage=args.stage, asset_id=args.id, output_dir=Path(args.output_dir).resolve() if args.output_dir else None, copy=args.copy, source_url=args.source_url, license_name=args.license)
    except AssetError as exc:
        print(f"import failed: {exc}", file=sys.stderr)
        return 2
    if args.output_manifest:
        _write_json(Path(args.output_manifest), {"schema": SCHEMA, "version": 1, "assets": [result]})
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["status"] == "present" else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate = subparsers.add_parser("validate", help="validate a checked-in or generated inventory")
    validate.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    validate.add_argument("--root", help="root used to resolve present file names")
    validate.add_argument("--require-present", action="store_true", help="fail when any inventory file is absent")
    validate.add_argument("--json", action="store_true")
    validate.set_defaults(func=_command_validate)
    importer = subparsers.add_parser("import", help="inventory a local source without downloading it")
    importer.add_argument("--kind", choices=("doom", "bitnet"), required=True)
    importer.add_argument("--stage", choices=("metadata", "sliced", "full"), required=True)
    importer.add_argument("--source", required=True, help="local file or directory")
    importer.add_argument("--id", required=True, help="stable inventory id")
    importer.add_argument("--source-url", default="")
    importer.add_argument("--license", default="")
    importer.add_argument("--output-dir", help="destination root when --copy is used")
    importer.add_argument("--copy", action="store_true", help="copy validated source files into --output-dir")
    importer.add_argument("--output-manifest", help="write a generated one-asset inventory")
    importer.set_defaults(func=_command_import)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
