#!/usr/bin/env python3
"""Acquire and validate provenance-locked external Doom and BitNet assets.

Validation is deliberately offline: it never follows a URL or silently
substitutes a generated payload.  The explicit ``acquire`` command is the only
network path.  It derives URLs from the checked-in provenance lock, streams
downloads into the ignored ``build/external-assets`` cache, verifies the pinned
size and SHA-256, and promotes only validated payloads.  The cache contains
only inventories and operator-supplied payloads; it is never a source file for
Git.

The format checks are intentionally streaming and bounded:

* WAD headers and directories are read from a stream without loading the WAD.
* Safetensors headers are parsed without loading tensor payloads.
* SHA-256 is computed in fixed-size chunks.
* BitNet shard numbering and conversion metadata are checked deterministically.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path
from typing import Any, BinaryIO, Iterable, Mapping


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "benchmarks" / "assets" / "external_assets.v1.json"
DEFAULT_CACHE_DIR = ROOT / "build" / "external-assets"
SCHEMA = "trit.external_benchmark_assets.v1"
CACHE_INVENTORY = "inventory.v1.json"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SHARD_RE = re.compile(
    r"(?:^|[-_.])(?P<index>\d{1,6})[-_]of[-_](?P<count>\d{1,6})(?:[-_.]|$)",
    re.IGNORECASE,
)
MAX_WAD_LUMPS = 1_000_000
MAX_SAFETENSORS_HEADER = 128 * 1024 * 1024
MAX_GGUF_HEADER = 128 * 1024 * 1024
DOWNLOAD_CHUNK_BYTES = 1024 * 1024
DEFAULT_DOWNLOAD_TIMEOUT = 120

OFFICIAL_BITNET_IDENTITY: dict[str, Any] = {
    "architecture": "bitnet-b1.58",
    "name": "bitnet2b",
    "vocab_size": 128256,
    "hidden_size": 2560,
    "block_count": 30,
    "intermediate_size": 6912,
    "head_count": 20,
    "head_count_kv": 5,
    "head_dim": 128,
}


class AssetError(ValueError):
    """A deterministic validation or import error."""


def sha256_stream(stream: BinaryIO, chunk_size: int = 1024 * 1024) -> str:
    """Hash a binary stream from its current position in bounded chunks."""

    if chunk_size <= 0:
        raise ValueError("chunk_size must be positive")
    digest = hashlib.sha256()
    while True:
        chunk = stream.read(chunk_size)
        if not chunk:
            break
        digest.update(chunk)
    return digest.hexdigest()


def sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    with path.open("rb") as stream:
        return sha256_stream(stream, chunk_size)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _json_load(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AssetError(f"cannot read JSON {path}: {exc}") from exc


def _read_exact(stream: BinaryIO, size: int) -> bytes:
    if size < 0:
        raise ValueError("size must be non-negative")
    chunks: list[bytes] = []
    remaining = size
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def _stream_size(stream: BinaryIO) -> int | None:
    try:
        current = stream.tell()
        stream.seek(0, io.SEEK_END)
        size = stream.tell()
        stream.seek(current, io.SEEK_SET)
        return int(size)
    except (AttributeError, OSError, ValueError):
        return None


def _seek_or_discard(stream: BinaryIO, offset: int) -> bool:
    try:
        stream.seek(offset, io.SEEK_SET)
        return stream.tell() == offset
    except (AttributeError, OSError, ValueError):
        return False


def validate_wad_stream(
    stream: BinaryIO,
    *,
    file_size: int | None = None,
    chunk_size: int = 1024 * 1024,
) -> dict[str, Any]:
    """Validate a WAD header and directory without loading the payload.

    ``stream`` may be a normal file or a seekable archive member.  The
    directory is read entry-by-entry, which keeps memory proportional to the
    directory metadata rather than the WAD's tens of megabytes of data.
    """

    del chunk_size  # reserved for callers that share a streaming policy
    if file_size is None:
        file_size = _stream_size(stream)
    if file_size is None:
        return {"ok": False, "issues": ["WAD stream size is unavailable"]}
    try:
        if not _seek_or_discard(stream, 0):
            return {"ok": False, "issues": ["WAD stream is not seekable"]}
        header = _read_exact(stream, 12)
    except (OSError, ValueError) as exc:
        return {"ok": False, "issues": [f"cannot read WAD header: {exc}"]}
    if len(header) < 12:
        return {"ok": False, "issues": ["WAD is shorter than its 12-byte header"]}

    issues: list[str] = []
    magic = header[:4]
    if magic not in (b"IWAD", b"PWAD"):
        issues.append("WAD magic must be IWAD or PWAD")
    lump_count, directory_offset = struct.unpack_from("<II", header, 4)
    if lump_count > MAX_WAD_LUMPS:
        issues.append(f"WAD lump count exceeds safety limit ({MAX_WAD_LUMPS})")
    directory_bytes = lump_count * 16
    if directory_offset > file_size or directory_bytes > file_size - directory_offset:
        issues.append("WAD directory extends beyond file")

    lumps: list[dict[str, Any]] = []
    if not issues:
        if not _seek_or_discard(stream, directory_offset):
            return {"ok": False, "issues": ["cannot seek to WAD directory"]}
        for index in range(lump_count):
            entry = _read_exact(stream, 16)
            if len(entry) != 16:
                issues.append(f"WAD directory entry {index} is truncated")
                break
            file_offset, size = struct.unpack_from("<II", entry, 0)
            raw_name = entry[8:16]
            name_bytes = raw_name.rstrip(b"\0")
            try:
                name = name_bytes.decode("ascii")
            except UnicodeDecodeError:
                name = ""
                issues.append(f"WAD lump {index} has a non-ASCII name")
            if not name or any(byte < 32 or byte > 126 for byte in name_bytes):
                issues.append(f"WAD lump {index} has an invalid name")
            if file_offset > file_size or size > file_size - file_offset:
                issues.append(f"WAD lump {index} data extends beyond file")
            lumps.append({"index": index, "name": name, "offset": file_offset, "size": size})
    return {
        "ok": not issues,
        "issues": issues,
        "format": magic.decode("ascii", errors="replace"),
        "lump_count": lump_count,
        "directory_offset": directory_offset,
        "lumps": lumps,
    }


def validate_wad_bytes(data: bytes) -> dict[str, Any]:
    return validate_wad_stream(io.BytesIO(data), file_size=len(data))


def validate_wad(path: Path) -> dict[str, Any]:
    try:
        size = path.stat().st_size
        with path.open("rb") as stream:
            return validate_wad_stream(stream, file_size=size)
    except OSError as exc:
        return {"ok": False, "issues": [f"cannot read WAD {path}: {exc}"]}


def _dtype_size(dtype: str) -> int | None:
    return {
        "BOOL": 1,
        "U8": 1,
        "I8": 1,
        "U16": 2,
        "I16": 2,
        "F16": 2,
        "BF16": 2,
        "U32": 4,
        "I32": 4,
        "F32": 4,
        "U64": 8,
        "I64": 8,
        "F64": 8,
    }.get(dtype)


def _read_safetensors_header(path: Path) -> dict[str, Any]:
    try:
        file_size = path.stat().st_size
        with path.open("rb") as stream:
            prefix = _read_exact(stream, 8)
            if len(prefix) != 8:
                return {"ok": False, "issues": ["safetensors file is shorter than 8-byte header"]}
            (header_length,) = struct.unpack("<Q", prefix)
            if header_length > file_size - 8:
                return {"ok": False, "issues": ["safetensors header extends beyond file"]}
            if header_length > MAX_SAFETENSORS_HEADER:
                return {"ok": False, "issues": ["safetensors header exceeds 128 MiB safety limit"]}
            raw_header = _read_exact(stream, header_length)
        header = json.loads(raw_header.decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError, struct.error, ValueError) as exc:
        return {"ok": False, "issues": [f"invalid safetensors header: {exc}"]}
    if not isinstance(header, dict):
        return {"ok": False, "issues": ["safetensors header must be a JSON object"]}

    issues: list[str] = []
    payload_start = 8 + header_length
    tensors: list[dict[str, Any]] = []
    ranges: list[tuple[int, int, str]] = []
    for name, metadata in header.items():
        if name == "__metadata__":
            if not isinstance(metadata, dict):
                issues.append("__metadata__ must be an object")
            continue
        if not isinstance(name, str) or not isinstance(metadata, dict):
            issues.append(f"tensor entry {name!r} is not an object")
            continue
        dtype = metadata.get("dtype")
        shape = metadata.get("shape")
        offsets = metadata.get("data_offsets")
        if not isinstance(dtype, str) or not dtype:
            issues.append(f"tensor {name!r} has no dtype")
        if not isinstance(shape, list) or not all(isinstance(item, int) and item >= 0 for item in shape):
            issues.append(f"tensor {name!r} has invalid shape")
        if not isinstance(offsets, list) or len(offsets) != 2 or not all(isinstance(item, int) for item in offsets):
            issues.append(f"tensor {name!r} has invalid data_offsets")
            continue
        start, end = offsets
        if start < 0 or end < start:
            issues.append(f"tensor {name!r} has descending data_offsets")
        if end > file_size - payload_start:
            issues.append(f"tensor {name!r} payload extends beyond file")
        if end >= start:
            ranges.append((start, end, name))
        item_size = _dtype_size(dtype) if isinstance(dtype, str) else None
        if item_size is not None and isinstance(shape, list):
            count = 1
            for dimension in shape:
                count *= dimension
            if end - start != count * item_size:
                issues.append(f"tensor {name!r} byte length does not match dtype and shape")
        tensors.append({"name": name, "dtype": dtype, "shape": shape, "data_offsets": offsets})
    ordered_ranges = sorted(ranges)
    for previous, current in zip(ordered_ranges, ordered_ranges[1:]):
        if current[0] < previous[1]:
            issues.append(f"safetensors tensor ranges overlap: {previous[2]} and {current[2]}")
    identity, identity_issues = _infer_safetensors_identity(tensors)
    issues.extend(identity_issues)
    return {
        "ok": not issues,
        "format": "safetensors",
        "header_bytes": header_length,
        "tensor_count": len(tensors),
        "tensors": tensors,
        "metadata": header.get("__metadata__", {}),
        "identity": identity,
        "issues": issues,
    }


def _infer_safetensors_identity(
    tensors: list[Mapping[str, Any]],
) -> tuple[dict[str, Any], list[str]]:
    """Infer the official BitNet model identity from tensor names/shapes.

    Safetensors files do not carry the complete model-card configuration in
    their small metadata object.  The geometry below is therefore checked
    directly from the tensor header.  This remains bounded: tensor payloads
    are never opened here.
    """

    by_name = {str(item.get("name")): item for item in tensors}
    layer_numbers: set[int] = set()
    layer_pattern = re.compile(r"^model\.layers\.(\d+)\.")
    for name in by_name:
        match = layer_pattern.match(name)
        if match:
            layer_numbers.add(int(match.group(1)))
    recognized = bool(layer_numbers) or "model.embed_tokens.weight" in by_name
    if not recognized:
        return {"recognized": False}, []

    issues: list[str] = []
    identity: dict[str, Any] = {
        "recognized": True,
        "architecture": "bitnet-b1.58",
        "name": "bitnet2b",
        "block_count": len(layer_numbers),
    }

    def shape(name: str) -> list[int] | None:
        value = by_name.get(name)
        candidate = value.get("shape") if value else None
        return list(candidate) if isinstance(candidate, list) else None

    embedding_shape = shape("model.embed_tokens.weight")
    if embedding_shape == [128256, 2560]:
        identity.update(vocab_size=128256, hidden_size=2560)
    else:
        issues.append(
            "official BitNet safetensors embedding must have shape [128256, 2560]"
        )

    norm_shape = shape("model.norm.weight")
    if norm_shape != [2560]:
        issues.append("official BitNet safetensors final norm must have shape [2560]")

    if layer_numbers != set(range(30)):
        issues.append("official BitNet safetensors must contain exactly layers 0..29")

    expected_projection_shapes = {
        "q_proj": [640, 2560],
        "k_proj": [160, 2560],
        "v_proj": [160, 2560],
        "o_proj": [640, 2560],
        "gate_proj": [1728, 2560],
        "up_proj": [1728, 2560],
        "down_proj": [640, 6912],
    }
    for layer in range(30):
        for projection, expected_shape in expected_projection_shapes.items():
            name = (
                f"model.layers.{layer}.self_attn.{projection}.weight"
                if projection in {"q_proj", "k_proj", "v_proj", "o_proj"}
                else f"model.layers.{layer}.mlp.{projection}.weight"
            )
            if shape(name) != expected_shape:
                issues.append(f"{name} has unexpected packed shape")
            scale_name = name + "_scale"
            scale_entry = by_name.get(scale_name)
            if not scale_entry or scale_entry.get("dtype") != "BF16" or shape(scale_name) != [1]:
                issues.append(f"{scale_name} must be a BF16 scalar")
    identity.update(
        intermediate_size=6912,
        head_count=20,
        head_count_kv=5,
        head_dim=128,
    )
    return identity, issues


class _GgufReader:
    """Bounded little-endian reader for GGUF metadata and tensor infos."""

    def __init__(self, stream: BinaryIO, file_size: int):
        self.stream = stream
        self.file_size = file_size
        self.bytes_read = 0

    def read(self, size: int) -> bytes:
        if size < 0 or self.bytes_read + size > MAX_GGUF_HEADER:
            raise AssetError("GGUF header exceeds the bounded parser limit")
        data = _read_exact(self.stream, size)
        if len(data) != size:
            raise AssetError("GGUF header is truncated")
        self.bytes_read += size
        return data

    def u32(self) -> int:
        return struct.unpack("<I", self.read(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self.read(8))[0]

    def string(self) -> str:
        length = self.u64()
        if length > MAX_GGUF_HEADER or length > self.file_size:
            raise AssetError("GGUF string is unreasonably large")
        try:
            return self.read(length).decode("utf-8")
        except UnicodeDecodeError as exc:
            raise AssetError(f"GGUF string is not UTF-8: {exc}") from exc

    def value(self, value_type: int) -> Any:
        scalar_formats: dict[int, str] = {
            0: "<B", 1: "<b", 2: "<H", 3: "<h", 4: "<I", 5: "<i",
            6: "<f", 7: "<?", 10: "<Q", 11: "<q", 12: "<d",
        }
        if value_type == 8:
            return self.string()
        if value_type == 9:
            element_type = self.u32()
            count = self.u64()
            if count > 1_000_000:
                raise AssetError("GGUF metadata array is too large")
            return [self.value(element_type) for _ in range(count)]
        fmt = scalar_formats.get(value_type)
        if fmt is None:
            raise AssetError(f"unsupported GGUF metadata type {value_type}")
        return struct.unpack(fmt, self.read(struct.calcsize(fmt)))[0]


def _infer_gguf_identity(
    metadata: Mapping[str, Any], tensors: list[Mapping[str, Any]], version: int,
) -> tuple[dict[str, Any], list[str]]:
    architecture = metadata.get("general.architecture")
    name = metadata.get("general.name")
    recognized = architecture == "bitnet-b1.58" or any(
        str(item.get("name", "")).startswith("blk.") for item in tensors
    )
    if not recognized:
        return {"recognized": False}, []
    identity: dict[str, Any] = {
        "recognized": True,
        "architecture": architecture,
        "name": name,
        "block_count": metadata.get("bitnet-b1.58.block_count"),
        "vocab_size": metadata.get("bitnet-b1.58.vocab_size"),
        "hidden_size": metadata.get("bitnet-b1.58.embedding_length"),
        "intermediate_size": metadata.get("bitnet-b1.58.feed_forward_length"),
        "head_count": metadata.get("bitnet-b1.58.attention.head_count"),
        "head_count_kv": metadata.get("bitnet-b1.58.attention.head_count_kv"),
        "head_dim": metadata.get("bitnet-b1.58.rope.dimension_count"),
        "gguf_version": version,
        "tensor_count": len(tensors),
    }
    issues: list[str] = []
    for key, expected in OFFICIAL_BITNET_IDENTITY.items():
        if key == "architecture":
            actual = architecture
        elif key == "name":
            actual = name
        elif key == "block_count":
            actual = metadata.get("bitnet-b1.58.block_count")
        elif key == "vocab_size":
            actual = metadata.get("bitnet-b1.58.vocab_size")
        elif key == "hidden_size":
            actual = metadata.get("bitnet-b1.58.embedding_length")
        elif key == "intermediate_size":
            actual = metadata.get("bitnet-b1.58.feed_forward_length")
        elif key == "head_count":
            actual = metadata.get("bitnet-b1.58.attention.head_count")
        elif key == "head_count_kv":
            actual = metadata.get("bitnet-b1.58.attention.head_count_kv")
        elif key == "head_dim":
            actual = metadata.get("bitnet-b1.58.rope.dimension_count")
        else:
            continue
        if actual != expected:
            issues.append(f"official BitNet GGUF identity {key} mismatch")
    if version != 3:
        issues.append("official BitNet GGUF must use version 3")
    tensor_names = {str(item.get("name")) for item in tensors}
    for required in ("token_embd.weight", "output_norm.weight", "blk.0.attn_q.weight"):
        if required not in tensor_names:
            issues.append(f"official BitNet GGUF is missing {required}")
    return identity, issues


def _read_gguf_header(path: Path) -> dict[str, Any]:
    try:
        file_size = path.stat().st_size
        with path.open("rb") as stream:
            prefix = _read_exact(stream, 24)
            if len(prefix) != 24:
                return {"ok": False, "issues": ["GGUF file is shorter than its fixed header"]}
            magic, version, tensor_count, metadata_count = struct.unpack("<4sIQQ", prefix)
            reader = _GgufReader(stream, file_size)
            metadata: dict[str, Any] = {}
            for _ in range(metadata_count):
                key = reader.string()
                metadata[key] = reader.value(reader.u32())
            tensors: list[dict[str, Any]] = []
            for _ in range(tensor_count):
                name = reader.string()
                dimensions = reader.u32()
                if dimensions > 32:
                    raise AssetError("GGUF tensor has too many dimensions")
                shape = [reader.u64() for _ in range(dimensions)]
                tensor_type = reader.u32()
                offset = reader.u64()
                tensors.append({
                    "name": name,
                    "shape": shape,
                    "type": tensor_type,
                    "offset": offset,
                })
        if magic != b"GGUF":
            return {"ok": False, "issues": ["GGUF magic must be GGUF"]}
        identity, identity_issues = _infer_gguf_identity(metadata, tensors, version)
    except (OSError, struct.error, AssetError, OverflowError) as exc:
        return {"ok": False, "issues": [f"invalid GGUF header: {exc}"]}
    issues: list[str] = []
    if version not in (1, 2, 3):
        issues.append(f"unsupported GGUF version {version}")
    issues.extend(identity_issues)
    return {
        "ok": not issues,
        "format": "gguf",
        "version": version,
        "tensor_count": tensor_count,
        "metadata_count": metadata_count,
        "metadata": metadata,
        "tensors": tensors,
        "identity": identity,
        "issues": issues,
    }


def validate_model_file(path: Path) -> dict[str, Any]:
    suffix = path.suffix.lower()
    if suffix == ".safetensors":
        result = _read_safetensors_header(path)
    elif suffix == ".gguf":
        result = _read_gguf_header(path)
    elif suffix in (".json", ".yaml", ".yml"):
        try:
            value = _json_load(path)
            result = {
                "ok": isinstance(value, dict),
                "format": "metadata-json",
                "issues": [] if isinstance(value, dict) else ["metadata JSON must be an object"],
            }
        except AssetError as exc:
            result = {"ok": False, "format": "metadata-json", "issues": [str(exc)]}
    else:
        present = path.is_file() and path.stat().st_size > 0
        result = {
            "ok": present,
            "format": suffix.lstrip(".") or "binary",
            "issues": [] if present else ["model file is empty or missing"],
        }
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
    entries = list(files)
    parsed: list[tuple[int, int, str]] = []
    for entry in entries:
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
    elif stage == "full" and not entries:
        issues.append("full BitNet inventory must contain at least one model file")
    return issues


def _file_entry(path: Path, display_name: str | None = None) -> dict[str, Any]:
    name = display_name or path.name
    entry: dict[str, Any] = {
        "name": name,
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
        "present": True,
    }
    if path.suffix.lower() == ".wad":
        entry["validation"] = validate_wad(path)
    elif path.suffix.lower() in (".safetensors", ".gguf", ".json", ".yaml", ".yml"):
        entry["validation"] = validate_model_file(path)
    else:
        entry["validation"] = {
            "ok": path.stat().st_size > 0,
            "format": path.suffix.lstrip(".") or "binary",
            "issues": [] if path.stat().st_size > 0 else ["file is empty"],
        }
    return entry


def _collect_source_files(source: Path, kind: str) -> list[Path]:
    if not source.exists():
        raise AssetError(f"source does not exist: {source}")
    if source.is_file():
        return [source]
    suffixes = {
        "doom": {".wad"},
        "bitnet": {".safetensors", ".gguf", ".json", ".yaml", ".yml", ".bin", ".t40"},
    }[kind]
    return sorted(
        (path for path in source.rglob("*") if path.is_file() and path.suffix.lower() in suffixes),
        key=lambda item: item.as_posix().lower(),
    )


def _is_safe_relative(path: str) -> bool:
    candidate = Path(path)
    return not candidate.is_absolute() and ".." not in candidate.parts


def _safe_join(root: Path, relative: str) -> Path:
    if not _is_safe_relative(relative):
        raise AssetError(f"unsafe relative asset path: {relative}")
    target = (root / relative).resolve()
    root_resolved = root.resolve()
    try:
        target.relative_to(root_resolved)
    except ValueError as exc:
        raise AssetError(f"asset path escapes cache: {relative}") from exc
    return target


def _asset_by_id(manifest: Mapping[str, Any], asset_id: str) -> Mapping[str, Any]:
    assets = manifest.get("assets")
    if not isinstance(assets, list):
        raise AssetError("lock manifest assets must be an array")
    for asset in assets:
        if isinstance(asset, dict) and asset.get("id") == asset_id:
            return asset
    raise AssetError(f"asset id is not present in the provenance manifest: {asset_id}")


def _expected_file_for(entry: Path, expected: Mapping[str, Any]) -> bool:
    expected_name = str(expected.get("name", ""))
    actual = entry.as_posix().replace("\\", "/")
    return actual == expected_name or Path(actual).name == Path(expected_name).name


def _lock_import(
    manifest: Mapping[str, Any] | None,
    *,
    asset_id: str,
    kind: str,
    stage: str,
    source_url: str,
    license_name: str,
    source_revision: str,
) -> tuple[Mapping[str, Any] | None, str, str, str]:
    if manifest is None:
        return None, source_url, license_name, source_revision
    asset = _asset_by_id(manifest, asset_id)
    if asset.get("kind") != kind:
        raise AssetError(f"asset {asset_id} is locked as kind={asset.get('kind')}, not {kind}")
    if asset.get("stage") != stage:
        raise AssetError(f"asset {asset_id} is locked as stage={asset.get('stage')}, not {stage}")
    expected_url = str(asset.get("source_url", ""))
    expected_license = str(asset.get("license", ""))
    expected_revision = str(asset.get("source_revision", ""))
    for label, supplied, expected in (
        ("source URL", source_url, expected_url),
        ("license", license_name, expected_license),
        ("source revision", source_revision, expected_revision),
    ):
        if supplied and supplied != expected:
            raise AssetError(f"{label} does not match the provenance lock for {asset_id}")
    if not expected_url or not expected_license:
        raise AssetError(f"asset {asset_id} has incomplete provenance URL/license")
    return asset, expected_url, expected_license, expected_revision


def _expected_digest(asset: Mapping[str, Any], expected_file: Mapping[str, Any]) -> str | None:
    digest = expected_file.get("sha256")
    if isinstance(digest, str) and digest:
        return digest
    provenance = asset.get("provenance")
    if isinstance(provenance, dict):
        candidate = provenance.get("sha256")
        if isinstance(candidate, str):
            return candidate
        candidate = provenance.get("file_sha256")
        if isinstance(candidate, str):
            return candidate
    return None


def _validate_locked_entries(
    entries: list[dict[str, Any]],
    *,
    asset: Mapping[str, Any],
    stage: str,
) -> list[str]:
    issues: list[str] = []
    expected_files = asset.get("files")
    if not isinstance(expected_files, list) or not expected_files:
        return ["provenance lock has no expected files"]
    by_basename: dict[str, list[dict[str, Any]]] = {}
    for entry in entries:
        by_basename.setdefault(Path(str(entry["name"])).name.lower(), []).append(entry)
    matched: list[dict[str, Any]] = []
    for expected in expected_files:
        if not isinstance(expected, dict):
            issues.append("provenance lock contains a non-object file entry")
            continue
        expected_name = str(expected.get("name", ""))
        candidates = by_basename.get(Path(expected_name).name.lower(), [])
        if len(candidates) != 1:
            issues.append(
                f"locked file {expected_name} has {len(candidates)} matching local files"
            )
            continue
        entry = candidates[0]
        matched.append(entry)
        expected_size = expected.get("size")
        if isinstance(expected_size, int) and entry.get("size") != expected_size:
            issues.append(
                f"{expected_name}: size mismatch: expected {expected_size}, observed {entry.get('size')}"
            )
        expected_hash = _expected_digest(asset, expected)
        if expected_hash and entry.get("sha256") != expected_hash:
            issues.append(f"{expected_name}: hash mismatch")
        expected_format = expected.get("format")
        observed_format = entry.get("validation", {}).get("format")
        if expected_format and observed_format and expected_format != observed_format:
            issues.append(
                f"{expected_name}: format mismatch: expected {expected_format}, observed {observed_format}"
            )
    if len(matched) != len(entries):
        issues.append("local inventory contains files outside the provenance lock")
    issues.extend(validate_shard_set(matched, stage))
    return issues


def import_source(
    source: Path,
    *,
    kind: str,
    stage: str,
    asset_id: str,
    output_dir: Path | None = None,
    copy: bool = False,
    source_url: str = "",
    license_name: str = "",
    source_revision: str = "",
    manifest: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Inventory a local source, optionally copying it into the ignored cache.

    Passing ``manifest`` makes this a provenance-locked import.  The command
    line always passes the checked-in manifest; direct callers may omit it for
    creating a local diagnostic inventory that is not an official claim.
    """

    asset, source_url, license_name, source_revision = _lock_import(
        manifest,
        asset_id=asset_id,
        kind=kind,
        stage=stage,
        source_url=source_url,
        license_name=license_name,
        source_revision=source_revision,
    )
    paths = _collect_source_files(source, kind)
    if not paths:
        raise AssetError(f"no {kind} files found under {source}")
    if stage == "metadata" and kind != "bitnet":
        raise AssetError("metadata stage is only valid for BitNet metadata/configuration")
    if stage == "metadata" and any(path.suffix.lower() not in {".json", ".yaml", ".yml"} for path in paths):
        raise AssetError("metadata stage may only contain JSON/YAML files")

    lock_files = asset.get("files") if asset else None
    if isinstance(lock_files, list) and lock_files:
        local_names = [path.relative_to(source).as_posix() if source.is_dir() else path.name for path in paths]
        for expected in lock_files:
            if not isinstance(expected, dict):
                continue
            if not any(_expected_file_for(Path(name), expected) for name in local_names):
                raise AssetError(f"locked file is missing from local source: {expected.get('name')}")

    target_root = (output_dir or DEFAULT_CACHE_DIR).resolve() if copy else None
    payload_root = (target_root / asset_id / "payload") if target_root else None
    entries: list[dict[str, Any]] = []
    for path in paths:
        relative_name = path.relative_to(source).as_posix() if source.is_dir() else path.name
        if asset:
            expected_matches = [
                item for item in lock_files or []
                if isinstance(item, dict) and _expected_file_for(Path(relative_name), item)
            ]
            display_name = str(expected_matches[0]["name"]) if len(expected_matches) == 1 else relative_name
        else:
            display_name = relative_name
        path_for_inventory = path
        cache_name = display_name
        if copy:
            assert payload_root is not None
            target = _safe_join(payload_root, display_name)
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = target.with_name(target.name + ".part")
            shutil.copyfile(path, temporary)
            temporary.replace(target)
            path_for_inventory = target
            cache_name = (Path(asset_id) / "payload" / display_name).as_posix()
        entry = _file_entry(path_for_inventory, cache_name if copy else display_name)
        entry["storage"] = "cache" if copy else "external"
        entry["source_name"] = relative_name
        entries.append(entry)

    issues: list[str] = []
    for entry in entries:
        validation = entry.get("validation", {})
        if validation.get("ok") is not True:
            issues.extend(f"{entry['name']}: {item}" for item in validation.get("issues", ["validation failed"]))
    if asset:
        lock_entries = [dict(entry, name=entry.get("source_name", entry["name"])) for entry in entries]
        issues.extend(_validate_locked_entries(lock_entries, asset=asset, stage=stage))
    elif kind == "bitnet":
        issues.extend(validate_shard_set(entries, stage))
    elif stage == "full" and not any(path.suffix.lower() == ".wad" for path in paths):
        issues.append("full Doom inventory must contain a WAD")
    status = "metadata_only" if stage == "metadata" and not issues else ("present" if not issues else "invalid")
    result: dict[str, Any] = {
        "id": asset_id,
        "kind": kind,
        "stage": stage,
        "status": status,
        "source_url": source_url,
        "source_revision": source_revision,
        "license": license_name,
        "provenance_locked": asset is not None,
        "files": entries,
        "issues": issues,
    }
    if asset is not None:
        provenance = asset.get("provenance")
        result["provenance"] = dict(provenance) if isinstance(provenance, Mapping) else {}
    if copy:
        result["cache_dir"] = str((target_root / asset_id).resolve())
    return result


def validate_manifest(
    manifest: Mapping[str, Any],
    *,
    root: Path = ROOT,
    require_present: bool = False,
) -> list[str]:
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
        if not isinstance(asset.get("source_revision"), str) or not asset.get("source_revision"):
            issues.append(f"{prefix}.source_revision must be recorded")
        provenance = asset.get("provenance")
        if not isinstance(provenance, dict):
            issues.append(f"{prefix}.provenance must be an object")
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
                            issues.extend(f"{file_prefix}: {item}" for item in wad_result.get("issues", ["invalid WAD"]))
                    if kind == "bitnet":
                        model_result = validate_model_file(path)
                        if model_result.get("ok") is not True:
                            issues.extend(f"{file_prefix}: {item}" for item in model_result.get("issues", ["invalid model file"]))
            elif require_present:
                issues.append(f"{file_prefix} is not present")
        if status == "present" and any(not item.get("present", False) for item in files if isinstance(item, dict)):
            issues.append(f"{prefix}.status=present requires every file to be present")
        if status == "not_present" and all(item.get("present", False) for item in files if isinstance(item, dict)):
            issues.append(f"{prefix}.status=not_present conflicts with present files")
        if kind == "bitnet":
            issues.extend(f"{prefix}: {item}" for item in validate_shard_set(files, str(stage)))
    return issues


def validate_cache(cache_dir: Path, asset_id: str) -> dict[str, Any]:
    """Return ``skip`` when a cache is absent and never turn absence into pass."""

    cache_dir = cache_dir.resolve()
    inventory_path = cache_dir / asset_id / CACHE_INVENTORY
    if not inventory_path.is_file():
        return {
            "status": "skip",
            "ok": False,
            "asset_id": asset_id,
            "reason": f"validated cache inventory is missing: {inventory_path}",
        }
    try:
        manifest = _json_load(inventory_path)
    except AssetError as exc:
        return {"status": "invalid", "ok": False, "asset_id": asset_id, "reason": str(exc)}
    if not isinstance(manifest, dict):
        return {"status": "invalid", "ok": False, "asset_id": asset_id, "reason": "cache inventory must be an object"}
    issues = validate_manifest(manifest, root=cache_dir, require_present=True)
    try:
        asset = _asset_by_id(manifest, asset_id)
    except AssetError:
        asset = None
        issues.append(f"cache inventory does not contain {asset_id}")
    if asset and not asset.get("provenance_locked", False):
        issues.append("cache inventory is not provenance-locked")
    return {
        "status": "present" if not issues else "invalid",
        "ok": not issues,
        "asset_id": asset_id,
        "inventory": str(inventory_path),
        "issues": issues,
    }


def _write_json(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def _expected_archive(asset: Mapping[str, Any]) -> tuple[str, int, str]:
    provenance = asset.get("provenance")
    if not isinstance(provenance, dict):
        raise AssetError("asset provenance is missing")
    url = str(asset.get("source_url", ""))
    size = provenance.get("archive_size")
    digest = provenance.get("archive_sha256")
    if not url or not isinstance(size, int) or size <= 0:
        raise AssetError("archive acquisition requires a locked source URL and archive_size")
    if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
        raise AssetError("archive acquisition requires a locked archive_sha256")
    return url, size, digest


def _download_url(asset: Mapping[str, Any], expected_name: str) -> str:
    """Derive a single-file URL from a locked model-card URL.

    Hugging Face model-card URLs are intentionally kept in the provenance
    manifest.  The revision-pinned resolve URL is derived here so a caller
    cannot redirect acquisition to an arbitrary file without changing the
    lock.  Other direct URLs are accepted only when they already name the
    requested file.
    """

    provenance = asset.get("provenance")
    if isinstance(provenance, dict):
        candidate = provenance.get("download_url")
        if isinstance(candidate, str) and candidate:
            return candidate
    source_url = str(asset.get("source_url", ""))
    revision = str(asset.get("source_revision", ""))
    parsed = urllib.parse.urlparse(source_url)
    source_path = parsed.path.rstrip("/")
    if parsed.netloc.lower().endswith("huggingface.co"):
        if "/resolve/" in source_path and Path(source_path).name == Path(expected_name).name:
            return source_url
        if not revision:
            raise AssetError("Hugging Face acquisition requires a locked source_revision")
        encoded_revision = urllib.parse.quote(revision, safe="")
        encoded_name = urllib.parse.quote(expected_name.replace("\\", "/"), safe="/")
        return source_url.rstrip("/") + f"/resolve/{encoded_revision}/{encoded_name}?download=true"
    if Path(source_path).name == Path(expected_name).name:
        return source_url
    raise AssetError(
        f"cannot derive a locked download URL for {expected_name} from {source_url}"
    )


def _download_locked(
    url: str,
    target: Path,
    *,
    expected_size: int,
    expected_sha256: str,
    timeout: int,
    resume: bool,
    force: bool,
) -> dict[str, Any]:
    """Download one locked file with resumable, atomic promotion."""

    if expected_size < 0 or not SHA256_RE.fullmatch(expected_sha256):
        raise AssetError(f"invalid download lock for {target.name}")
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_file():
        observed_size = target.stat().st_size
        observed_hash = sha256_file(target)
        if observed_size == expected_size and observed_hash == expected_sha256:
            return {
                "url": url,
                "path": str(target),
                "size": observed_size,
                "sha256": observed_hash,
                "reused": True,
                "resumed": False,
            }
        if not force:
            raise AssetError(
                f"existing cache file does not match lock: {target}; use --force to replace it"
            )
        target.unlink()

    partial = target.with_name(target.name + ".part")
    partial_size = partial.stat().st_size if partial.is_file() else 0
    if partial_size > expected_size:
        partial.unlink()
        partial_size = 0
    if not resume and partial.is_file():
        partial.unlink()
        partial_size = 0
    if partial_size == expected_size and partial.is_file():
        partial_hash = sha256_file(partial)
        if partial_hash == expected_sha256:
            partial.replace(target)
            return {
                "url": url,
                "path": str(target),
                "size": expected_size,
                "sha256": partial_hash,
                "reused": False,
                "resumed": True,
            }
        partial.unlink()
        partial_size = 0

    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/octet-stream",
            "User-Agent": "trit-external-asset-acquirer/1",
        },
    )
    # Large Hugging Face/Xet objects can delay an unbounded full response.
    # Always make the byte interval explicit; this also makes retries bounded
    # by the locked size and keeps the same code path for fresh and resumed
    # downloads.
    request.add_header("Range", f"bytes={partial_size}-{expected_size - 1}")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            status = getattr(response, "status", None) or response.getcode()
            append = partial_size > 0 and status == 206
            if partial_size > 0 and not append:
                partial_size = 0
            mode = "ab" if append else "wb"
            with partial.open(mode) as output:
                while True:
                    chunk = response.read(DOWNLOAD_CHUNK_BYTES)
                    if not chunk:
                        break
                    output.write(chunk)
                    if output.tell() > expected_size:
                        raise AssetError(
                            f"download exceeded locked size for {target.name}: "
                            f"expected {expected_size}"
                        )
                output.flush()
                os.fsync(output.fileno())
    except (OSError, urllib.error.URLError, urllib.error.HTTPError) as exc:
        raise AssetError(
            f"download failed for {target.name}; resumable partial left at {partial}: {exc}"
        ) from exc

    observed_size = partial.stat().st_size if partial.is_file() else 0
    observed_hash = sha256_file(partial) if partial.is_file() else ""
    if observed_size != expected_size or observed_hash != expected_sha256:
        raise AssetError(
            f"download lock mismatch for {target.name}: "
            f"size={observed_size} sha256={observed_hash}"
        )
    partial.replace(target)
    return {
        "url": url,
        "path": str(target),
        "size": observed_size,
        "sha256": observed_hash,
        "reused": False,
        "resumed": bool(partial_size),
    }


def _extract_locked_archive(
    archive: Path,
    staging: Path,
    asset: Mapping[str, Any],
) -> list[dict[str, Any]]:
    """Extract only the files listed by the lock, verifying each member."""

    expected_files = asset.get("files")
    if not isinstance(expected_files, list) or not expected_files:
        raise AssetError("archive acquisition has no locked files")
    with zipfile.ZipFile(archive) as source:
        members = {
            info.filename.replace("\\", "/"): info
            for info in source.infolist()
            if not info.is_dir()
        }
        results: list[dict[str, Any]] = []
        for expected in expected_files:
            if not isinstance(expected, dict):
                raise AssetError("archive lock contains a non-object file entry")
            name = str(expected.get("name", ""))
            info = members.get(name)
            if info is None:
                raise AssetError(f"locked archive member is missing: {name}")
            expected_size = expected.get("size")
            expected_hash = _expected_digest(asset, expected)
            if not isinstance(expected_size, int) or expected_size < 0:
                raise AssetError(f"locked archive member has invalid size: {name}")
            if not expected_hash or not SHA256_RE.fullmatch(expected_hash):
                raise AssetError(f"locked archive member has no SHA-256: {name}")
            target = _safe_join(staging, name)
            target.parent.mkdir(parents=True, exist_ok=True)
            partial = target.with_name(target.name + ".part")
            digest = hashlib.sha256()
            size = 0
            try:
                with source.open(info) as input_stream, partial.open("wb") as output:
                    while True:
                        chunk = input_stream.read(DOWNLOAD_CHUNK_BYTES)
                        if not chunk:
                            break
                        output.write(chunk)
                        digest.update(chunk)
                        size += len(chunk)
                    output.flush()
                    os.fsync(output.fileno())
            except (OSError, zipfile.BadZipFile) as exc:
                partial.unlink(missing_ok=True)
                raise AssetError(f"cannot extract locked archive member {name}: {exc}") from exc
            observed_hash = digest.hexdigest()
            if size != expected_size or observed_hash != expected_hash:
                partial.unlink(missing_ok=True)
                raise AssetError(
                    f"archive member lock mismatch for {name}: "
                    f"size={size} sha256={observed_hash}"
                )
            partial.replace(target)
            results.append({"name": name, "size": size, "sha256": observed_hash})
    return results


def _command_acquire(args: argparse.Namespace) -> int:
    try:
        manifest_value = _json_load(Path(args.manifest))
        if not isinstance(manifest_value, dict):
            raise AssetError("lock manifest must be a JSON object")
        manifest_issues = validate_manifest(manifest_value, root=ROOT)
        if manifest_issues:
            raise AssetError("provenance manifest is invalid: " + "; ".join(manifest_issues))
        asset = _asset_by_id(manifest_value, args.id)
        if args.kind and asset.get("kind") != args.kind:
            raise AssetError(
                f"asset {args.id} is locked as kind={asset.get('kind')}, not {args.kind}"
            )
        if asset.get("stage") == "metadata":
            raise AssetError("metadata fixtures are imported locally; acquire is for payloads")
        cache_dir = Path(args.cache_dir).resolve()
        cached = validate_cache(cache_dir, args.id)
        if cached.get("status") == "present" and not args.force:
            print(json.dumps({"status": "present", "reused_cache": True, **cached}, indent=2, sort_keys=True))
            return 0

        asset_root = cache_dir / args.id
        staging = asset_root / ".staging"
        staging.mkdir(parents=True, exist_ok=True)
        downloads: list[dict[str, Any]] = []
        if asset.get("kind") == "doom":
            url, archive_size, archive_hash = _expected_archive(asset)
            archive_name = Path(urllib.parse.urlparse(url).path).name or "source.zip"
            archive_target = asset_root / "acquisition" / archive_name
            downloads.append(_download_locked(
                url,
                archive_target,
                expected_size=archive_size,
                expected_sha256=archive_hash,
                timeout=args.timeout,
                resume=not args.no_resume,
                force=args.force,
            ))
            extracted = _extract_locked_archive(archive_target, staging, asset)
        else:
            extracted = []
            expected_files = asset.get("files")
            if not isinstance(expected_files, list) or not expected_files:
                raise AssetError("asset has no locked files")
            for expected in expected_files:
                if not isinstance(expected, dict):
                    raise AssetError("asset files must be objects")
                name = str(expected.get("name", ""))
                expected_size = expected.get("size")
                expected_hash = _expected_digest(asset, expected)
                if not isinstance(expected_size, int) or expected_size <= 0:
                    raise AssetError(f"locked file has invalid size: {name}")
                if not expected_hash or not SHA256_RE.fullmatch(expected_hash):
                    raise AssetError(f"locked file has no SHA-256: {name}")
                target = _safe_join(staging, Path(name).name)
                downloads.append(_download_locked(
                    _download_url(asset, name),
                    target,
                    expected_size=expected_size,
                    expected_sha256=expected_hash,
                    timeout=args.timeout,
                    resume=not args.no_resume,
                    force=args.force,
                ))
                extracted.append({"name": name, "size": expected_size, "sha256": expected_hash})

        result = import_source(
            staging,
            kind=str(asset["kind"]),
            stage=str(asset["stage"]),
            asset_id=args.id,
            output_dir=cache_dir,
            copy=True,
            manifest=manifest_value,
        )
        generated = {"schema": SCHEMA, "version": 1, "assets": [result]}
        _write_json(asset_root / CACHE_INVENTORY, generated)
        verified = validate_cache(cache_dir, args.id)
        if verified.get("status") != "present":
            raise AssetError(
                "acquired payload did not pass cache validation: "
                + "; ".join(verified.get("issues", []))
            )
        output = {
            "status": "present",
            "asset_id": args.id,
            "cache": verified,
            "downloads": downloads,
            "extracted": extracted,
        }
        print(json.dumps(output, indent=2, sort_keys=True))
        return 0
    except (AssetError, OSError, urllib.error.URLError, zipfile.BadZipFile) as exc:
        print(f"acquire failed: {exc}", file=sys.stderr)
        return 2


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


def _command_cache(args: argparse.Namespace) -> int:
    result = validate_cache(Path(args.cache_dir), args.asset_id)
    print(json.dumps(result, indent=2, sort_keys=True))
    # A missing cache is an explicit skip for orchestration, not a successful
    # validation.  Use a distinct code so shell gates cannot confuse it with a pass.
    return 0 if result["status"] == "present" else (3 if result["status"] == "skip" else 1)


def _command_import(args: argparse.Namespace) -> int:
    try:
        manifest = _json_load(Path(args.manifest)) if args.manifest else None
        if manifest is not None and not isinstance(manifest, dict):
            raise AssetError("lock manifest must be a JSON object")
        output_dir = Path(args.cache_dir or args.output_dir).resolve() if (args.cache_dir or args.output_dir) else DEFAULT_CACHE_DIR
        result = import_source(
            Path(args.source).resolve(),
            kind=args.kind,
            stage=args.stage,
            asset_id=args.id,
            output_dir=output_dir,
            copy=args.copy,
            source_url=args.source_url,
            license_name=args.license,
            source_revision=args.source_revision,
            manifest=manifest,
        )
    except (AssetError, OSError) as exc:
        print(f"import failed: {exc}", file=sys.stderr)
        return 2
    generated = {"schema": SCHEMA, "version": 1, "assets": [result]}
    if args.copy:
        _write_json(output_dir / args.id / CACHE_INVENTORY, generated)
    if args.output_manifest:
        _write_json(Path(args.output_manifest), generated)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["status"] in ("present", "metadata_only") else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate = subparsers.add_parser("validate", help="validate a checked-in or generated inventory")
    validate.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    validate.add_argument("--root", help="root used to resolve present file names")
    validate.add_argument("--require-present", action="store_true", help="fail when any inventory file is absent")
    validate.add_argument("--json", action="store_true")
    validate.set_defaults(func=_command_validate)

    cache = subparsers.add_parser("cache", help="validate one ignored external-assets cache entry")
    cache.add_argument("--cache-dir", default=str(DEFAULT_CACHE_DIR))
    cache.add_argument("--asset-id", required=True)
    cache.set_defaults(func=_command_cache)

    acquire = subparsers.add_parser(
        "acquire",
        help="download and validate one lock-authorized payload into the ignored cache",
    )
    acquire.add_argument("--id", required=True, help="asset id from the provenance lock manifest")
    acquire.add_argument("--kind", choices=("doom", "bitnet"), help="optional kind assertion")
    acquire.add_argument("--manifest", default=str(DEFAULT_MANIFEST), help="provenance lock manifest")
    acquire.add_argument("--cache-dir", default=str(DEFAULT_CACHE_DIR))
    acquire.add_argument("--timeout", type=int, default=DEFAULT_DOWNLOAD_TIMEOUT)
    acquire.add_argument("--no-resume", action="store_true")
    acquire.add_argument("--force", action="store_true", help="replace a mismatched cached file")
    acquire.set_defaults(func=_command_acquire)

    importer = subparsers.add_parser("import", help="inventory a local source without downloading it")
    importer.add_argument("--kind", choices=("doom", "bitnet"), required=True)
    importer.add_argument("--stage", choices=("metadata", "sliced", "full"), required=True)
    importer.add_argument("--source", required=True, help="local file or directory")
    importer.add_argument("--id", required=True, help="asset id from the provenance lock manifest")
    importer.add_argument("--manifest", default=str(DEFAULT_MANIFEST), help="provenance lock manifest")
    importer.add_argument("--source-url", default="")
    importer.add_argument("--source-revision", default="")
    importer.add_argument("--license", default="")
    importer.add_argument("--cache-dir", help=f"ignored cache root (default: {DEFAULT_CACHE_DIR})")
    importer.add_argument("--output-dir", help="compatibility alias for --cache-dir")
    importer.add_argument("--copy", action="store_true", help="copy validated source files into the ignored cache")
    importer.add_argument("--output-manifest", help="also write a generated one-asset inventory")
    importer.set_defaults(func=_command_import)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
