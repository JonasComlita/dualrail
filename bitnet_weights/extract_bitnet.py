#!/usr/bin/env python3
"""Stream and resume BitNet safetensors conversion into Trit T40 files.

The converter is intentionally independent of PyTorch and NumPy.  It reads a
validated safetensors source tensor-by-tensor, writes each output atomically,
and records completion after every tensor in ``conversion_metadata.v1.json``.
An interrupted conversion therefore resumes from the last verified tensor;
source size and SHA-256 are part of the resume lock.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import struct
import sys
from array import array
from pathlib import Path
from typing import Any, Iterable, Iterator, Mapping

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from safetensors_stream import (  # type: ignore[import-not-found]
        DEFAULT_CHUNK_BYTES,
        SafeTensorError,
        SafeTensorFile,
        TensorInfo,
        sha256_file,
    )
else:
    from .safetensors_stream import (
        DEFAULT_CHUNK_BYTES,
        SafeTensorError,
        SafeTensorFile,
        TensorInfo,
        sha256_file,
    )


CONVERSION_SCHEMA = "trit.bitnet_conversion.v1"
CONVERTER_VERSION = "streaming-t40-v5-packed-bitnet-plane-low-pair-bf16-source"
METADATA_NAME = "conversion_metadata.v1.json"
MANIFEST_NAME = "manifest.csv"
GUEST_PACKAGE_NAME = "guest_package.v1.json"
DEFAULT_SKIP_ELEMENTS = 1_000_000
T40_TRITS = 40
T40_POWERS = tuple(3**index for index in range(T40_TRITS))
OFFICIAL_PROFILES = {"official-slice", "official-full"}
OFFICIAL_TOKEN_VOCAB = 128256
OFFICIAL_HIDDEN_SIZE = 2560
OFFICIAL_LAYER_COUNT = 30


def _official_model_identity(source: SafeTensorFile) -> dict[str, Any]:
    """Validate the pinned Microsoft safetensors geometry without loading data."""

    issues: list[str] = []
    names = set(source.tensors)
    embedding = source.tensors.get("model.embed_tokens.weight")
    norm = source.tensors.get("model.norm.weight")
    if not embedding or embedding.dtype != "BF16" or embedding.shape != (128256, 2560):
        issues.append("model.embed_tokens.weight must be BF16 [128256, 2560]")
    if not norm or norm.dtype != "BF16" or norm.shape != (2560,):
        issues.append("model.norm.weight must be BF16 [2560]")

    layer_pattern = re.compile(r"^model\.layers\.(\d+)\.")
    layers = {int(match.group(1)) for name in names if (match := layer_pattern.match(name))}
    if layers != set(range(OFFICIAL_LAYER_COUNT)):
        issues.append("model must contain exactly model.layers.0 through model.layers.29")

    projections = {
        "self_attn.q_proj.weight": (640, 2560),
        "self_attn.k_proj.weight": (160, 2560),
        "self_attn.v_proj.weight": (160, 2560),
        "self_attn.o_proj.weight": (640, 2560),
        "mlp.gate_proj.weight": (1728, 2560),
        "mlp.up_proj.weight": (1728, 2560),
        "mlp.down_proj.weight": (640, 6912),
    }
    for layer in range(OFFICIAL_LAYER_COUNT):
        for suffix, shape in projections.items():
            name = f"model.layers.{layer}.{suffix}"
            info = source.tensors.get(name)
            if not info or info.dtype != "U8" or info.shape != shape:
                issues.append(f"{name} must be packed U8 {list(shape)}")
            scale = source.tensors.get(name + "_scale")
            if not scale or scale.dtype != "BF16" or scale.shape != (1,):
                issues.append(f"{name}_scale must be BF16 [1]")
    if issues:
        raise ConversionError("official BitNet safetensors identity failed: " + "; ".join(issues[:8]))
    return {
        "architecture": "bitnet-b1.58",
        "name": "bitnet2b",
        "vocab_size": OFFICIAL_TOKEN_VOCAB,
        "hidden_size": OFFICIAL_HIDDEN_SIZE,
        "block_count": OFFICIAL_LAYER_COUNT,
        "intermediate_size": 6912,
        "head_count": 20,
        "head_count_kv": 5,
        "head_dim": 128,
        "tensor_count": len(source.tensors),
    }


class ConversionError(ValueError):
    """A provenance, metadata, or conversion failure."""


def _clamp_ternary(value: float) -> int:
    if not math.isfinite(value):
        raise ConversionError("tensor contains a non-finite value")
    return -1 if value < -0.5 else (1 if value > 0.5 else 0)


def quantize_to_ternary(values: Iterable[float]) -> tuple[list[int], float]:
    """Compatibility helper for small callers; conversion uses streaming passes."""

    materialized = [float(value) for value in values]
    if not materialized:
        return [], 0.0
    gamma = sum(abs(value) for value in materialized) / len(materialized)
    if gamma < 1e-9:
        return [0] * len(materialized), 0.0
    return [_clamp_ternary(value / gamma) for value in materialized], gamma


def _encode_t40_value(value: float) -> int:
    if not math.isfinite(value):
        raise ConversionError("cannot encode a non-finite value as T40")
    if value == 0.0:
        return sum(power for power in T40_POWERS)
    mantissa = float(value)
    exponent = 0
    for _ in range(100):
        if abs(mantissa) <= 1.5:
            break
        mantissa /= 3.0
        exponent += 1
    for _ in range(100):
        if abs(mantissa) >= 0.5 or mantissa == 0.0:
            break
        mantissa *= 3.0
        exponent -= 1

    trits = [0] * T40_TRITS
    temporary_exponent = exponent
    for index in range(7):
        remainder = (temporary_exponent + 1) % 3
        trit = remainder - 1
        trits[33 + index] = trit
        temporary_exponent = (temporary_exponent - trit) // 3
    temporary_mantissa = mantissa
    for index in range(32, -1, -1):
        trit = 1 if temporary_mantissa >= 0.5 else (-1 if temporary_mantissa <= -0.5 else 0)
        trits[index] = trit
        temporary_mantissa = (temporary_mantissa - trit) * 3.0
    return _pack_t40_trits(trits)


def _pack_t40_trits(trits: Iterable[int]) -> int:
    value = 0
    for index, trit in enumerate(trits):
        if index >= T40_TRITS:
            raise ConversionError("too many trits for a T40 word")
        if trit not in (-1, 0, 1):
            raise ConversionError(f"invalid ternary value: {trit}")
        value += (int(trit) + 1) * T40_POWERS[index]
    return value


def encode_t40_batch(values: Iterable[float]) -> array:
    """Encode a small batch and retain the old ``array.tobytes`` API."""

    encoded = array("Q")
    for value in values:
        encoded.append(_encode_t40_value(float(value)))
    return encoded


def _iter_t40_words(values: Iterable[float | int | bool], *, ternary: bool) -> Iterator[int]:
    trits: list[int] = []
    for raw in values:
        if ternary:
            trit = int(raw)
            if trit not in (-1, 0, 1):
                raise ConversionError(f"quantized value is not ternary: {trit}")
        else:
            trit = None
        if ternary:
            trits.append(trit)
            if len(trits) == T40_TRITS:
                yield _pack_t40_trits(trits)
                trits.clear()
        else:
            yield _encode_t40_value(float(raw))
    if ternary and trits:
        trits.extend([0] * (T40_TRITS - len(trits)))
        yield _pack_t40_trits(trits)


def _atomic_write_bytes(path: Path, chunks: Iterable[bytes]) -> tuple[int, str]:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    size = 0
    import hashlib

    digest = hashlib.sha256()
    try:
        with temporary.open("wb") as stream:
            for chunk in chunks:
                stream.write(chunk)
                digest.update(chunk)
                size += len(chunk)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise
    return size, digest.hexdigest()


def _atomic_json(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def _write_manifest(path: Path, records: Mapping[str, Mapping[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    with temporary.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(("parameter_name", "filename", "scale", "mode"))
        for name in sorted(records):
            record = records[name]
            writer.writerow((name, record["filename"], record["scale"], record["mode"]))
    temporary.replace(path)


def _write_guest_package(
    target_dir: Path,
    records: Mapping[str, Mapping[str, Any]],
    metadata: Mapping[str, Any],
) -> None:
    """Publish the completed conversion as a read-only guest package.

    The conversion directory is deliberately a directory-backed guest VFS:
    every promoted tensor remains a separate file and the package lock binds
    the directory contents to the completed conversion metadata.  The package
    itself is written last, so an interrupted conversion can never be mistaken
    for an executable package.
    """

    entries: list[dict[str, Any]] = []

    def add_entry(name: str, kind: str, **extra: Any) -> None:
        path = target_dir / name
        if not path.is_file():
            raise ConversionError(f"guest package entry is missing: {name}")
        size = path.stat().st_size
        digest = sha256_file(path)
        entry: dict[str, Any] = {
            "name": name,
            "kind": kind,
            "size": size,
            "sha256": digest,
        }
        entry.update(extra)
        entries.append(entry)

    add_entry(MANIFEST_NAME, "manifest")
    add_entry(METADATA_NAME, "conversion-metadata")
    for tensor_name in sorted(records):
        record = records[tensor_name]
        filename = record.get("filename")
        if filename in (None, "SKIP"):
            continue
        if not isinstance(filename, str) or not filename:
            raise ConversionError(f"guest package tensor has an invalid filename: {tensor_name}")
        add_entry(
            filename,
            "tensor",
            tensor=tensor_name,
            mode=record.get("mode", ""),
            count=record.get("count", 0),
        )

    package = {
        "schema": "trit.bitnet_guest_package.v1",
        "version": 1,
        "status": "complete",
        "profile": metadata.get("profile", "generic"),
        "converter": metadata.get("converter", CONVERTER_VERSION),
        "source": metadata.get("source", {}),
        "model_identity": metadata.get("model_identity"),
        "tensor_count": metadata.get("tensor_count", len(records)),
        "converted_count": metadata.get("converted_count", 0),
        "skipped_count": metadata.get("skipped_count", 0),
        "entries": entries,
    }
    _atomic_json(target_dir / GUEST_PACKAGE_NAME, package)


def _load_existing_metadata(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ConversionError(f"cannot read conversion metadata {path}: {exc}") from exc
    if not isinstance(value, dict) or value.get("schema") != CONVERSION_SCHEMA:
        raise ConversionError("existing conversion metadata has an unsupported schema")
    return value


def _source_lock(
    path: Path,
    *,
    expected_sha256: str | None,
    expected_size: int | None,
    source_revision: str,
    model_id: str,
) -> dict[str, Any]:
    size = path.stat().st_size
    if expected_size is not None and size != expected_size:
        raise ConversionError(f"source size mismatch: expected {expected_size}, observed {size}")
    digest = sha256_file(path)
    if expected_sha256 and digest != expected_sha256:
        raise ConversionError("source SHA-256 does not match the provenance lock")
    return {
        "path": str(path.resolve()),
        "size": size,
        "sha256": digest,
        "source_revision": source_revision,
        "model_id": model_id,
    }


def _check_resume_lock(
    existing: Mapping[str, Any],
    source: Mapping[str, Any],
    *,
    source_revision: str,
    model_id: str,
    profile: str,
    token_ids: tuple[int, ...],
) -> None:
    if existing.get("converter") != CONVERTER_VERSION:
        raise ConversionError("existing conversion was produced by an incompatible converter")
    old_source = existing.get("source")
    if not isinstance(old_source, dict):
        raise ConversionError("existing conversion metadata has no source lock")
    for key in ("size", "sha256"):
        if old_source.get(key) != source.get(key):
            raise ConversionError(f"source {key} changed; refusing to resume")
    for key, value in (("source_revision", source_revision), ("model_id", model_id)):
        old_value = old_source.get(key, "")
        if value and old_value != value:
            raise ConversionError(f"source {key} changed; refusing to resume")
    if existing.get("profile", "generic") != profile:
        raise ConversionError("conversion profile changed; refusing to resume")
    old_token_ids = existing.get("token_ids", [])
    if list(token_ids) != old_token_ids:
        raise ConversionError("conversion token IDs changed; refusing to resume")


def _is_projection(name: str) -> bool:
    lowered = name.lower()
    return lowered.endswith("_proj.weight")


def _is_embedding(name: str) -> bool:
    return name == "model.embed_tokens.weight"


def _selected_for_profile(name: str, profile: str) -> bool:
    if profile == "generic":
        return True
    if _is_embedding(name) or name == "model.norm.weight":
        return True
    match = re.match(r"^model\.layers\.(\d+)\.", name)
    if not match:
        return False
    return profile == "official-full" or int(match.group(1)) == 0


def _parse_token_ids(value: str | Iterable[int] | None) -> tuple[int, ...]:
    if value is None:
        return ()
    if isinstance(value, str):
        pieces = [piece.strip() for piece in value.split(",") if piece.strip()]
        try:
            result = tuple(int(piece) for piece in pieces)
        except ValueError as exc:
            raise ConversionError("token IDs must be comma-separated integers") from exc
    else:
        result = tuple(int(item) for item in value)
    if len(result) != len(set(result)):
        raise ConversionError("token IDs must be unique")
    if any(item < 0 or item >= OFFICIAL_TOKEN_VOCAB for item in result):
        raise ConversionError("token IDs must be in the official vocabulary range")
    return result


def _read_scalar(source: SafeTensorFile, info: TensorInfo, *, chunk_bytes: int) -> float:
    values = list(source.iter_values(info, chunk_bytes=chunk_bytes))
    if len(values) != 1:
        raise ConversionError(f"scale tensor is not scalar: {info.name}")
    value = float(values[0])
    if not math.isfinite(value) or value == 0.0:
        raise ConversionError(f"scale tensor is invalid: {info.name}")
    return value


def _iter_packed_ternary(
    source: SafeTensorFile,
    info: TensorInfo,
    *,
    chunk_bytes: int,
) -> Iterator[int]:
    """Decode the official flattened I2 payload without materializing it.

    The Microsoft converter packs four ternary row planes into one byte,
    least-significant pair first (bits 1..0, 3..2, 5..4, 7..6).  It expands
    the packed ``[rows / 4, columns]`` tensor into four complete row planes
    before reshaping to ``[rows, columns]``.  Codes ``0``, ``1``, and ``2``
    represent ``-1``, ``0``, and ``+1``; code ``3`` is reserved and is
    rejected.  Re-reading the source for each plane keeps the decoder
    streaming and bounded-memory without materializing the expanded tensor.
    """

    if len(info.shape) != 2 or info.shape[0] <= 0 or info.shape[1] <= 0:
        raise ConversionError(f"packed tensor must be a non-empty rank-2 tensor: {info.name}")

    emitted = 0
    for shift in (0, 2, 4, 6):
        for raw in source.iter_values(info, chunk_bytes=chunk_bytes):
            byte = int(raw)
            code = (byte >> shift) & 0x03
            if code == 3:
                raise ConversionError(
                    f"official packed tensor contains reserved 2-bit code 3: {info.name}"
                )
            emitted += 1
            yield code - 1
    if emitted != info.count * 4:
        raise ConversionError(
            f"packed tensor count mismatch for {info.name}: expected {info.count * 4}, observed {emitted}"
        )


def _iter_bf16_span(
    source: SafeTensorFile,
    info: TensorInfo,
    begin: int,
    end: int,
    *,
    chunk_bytes: int,
) -> Iterator[float]:
    carry = b""
    emitted = 0
    for chunk in source.iter_raw_span(info, begin, end, chunk_bytes=chunk_bytes):
        carry += chunk
        complete = len(carry) - (len(carry) % 2)
        for (value,) in struct.iter_unpack("<H", carry[:complete]):
            numeric = struct.unpack("<f", struct.pack("<I", int(value) << 16))[0]
            if not math.isfinite(numeric):
                raise ConversionError(f"tensor contains a non-finite value: {info.name}")
            emitted += 1
            yield numeric
        carry = carry[complete:]
    if carry:
        raise ConversionError(f"BF16 span is not aligned: {info.name}")


def _record_matches(path: Path, record: Mapping[str, Any]) -> bool:
    if record.get("status") != "complete":
        return False
    if record.get("filename") == "SKIP":
        return True
    if not path.is_file():
        return False
    expected_size = record.get("output_size")
    expected_hash = record.get("output_sha256")
    if not isinstance(expected_size, int) or path.stat().st_size != expected_size:
        return False
    return isinstance(expected_hash, str) and sha256_file(path) == expected_hash


def convert_bitnet_to_trit(
    model_path: str | os.PathLike[str],
    output_dir: str | os.PathLike[str],
    *,
    expected_sha256: str | None = None,
    expected_size: int | None = None,
    source_revision: str = "",
    model_id: str = "",
    chunk_bytes: int = DEFAULT_CHUNK_BYTES,
    skip_elements: int = DEFAULT_SKIP_ELEMENTS,
    resume: bool = True,
    profile: str = "generic",
    token_ids: str | Iterable[int] | None = None,
) -> dict[str, Any]:
    """Convert a safetensors source one tensor at a time.

    ``expected_sha256`` and ``expected_size`` should come from the external
    provenance manifest for an official conversion.  Omitting them is allowed
    for local diagnostics but marks the generated metadata as unlocked.
    """

    source_path = Path(model_path).resolve()
    target_dir = Path(output_dir).resolve()
    if profile not in {"generic", *OFFICIAL_PROFILES}:
        raise ConversionError(f"unsupported conversion profile: {profile}")
    selected_token_ids = _parse_token_ids(token_ids)
    if profile in OFFICIAL_PROFILES and not selected_token_ids:
        raise ConversionError("official profiles require explicit fixed token IDs")
    if chunk_bytes <= 0 or skip_elements < 0:
        raise ConversionError("chunk_bytes must be positive and skip-elements non-negative")
    try:
        source = SafeTensorFile(source_path)
        model_identity = (
            _official_model_identity(source) if profile in OFFICIAL_PROFILES else None
        )
        source_lock = _source_lock(
            source_path,
            expected_sha256=expected_sha256,
            expected_size=expected_size,
            source_revision=source_revision,
            model_id=model_id,
        )
    except (OSError, SafeTensorError) as exc:
        raise ConversionError(str(exc)) from exc

    target_dir.mkdir(parents=True, exist_ok=True)
    metadata_path = target_dir / METADATA_NAME
    existing = _load_existing_metadata(metadata_path) if resume else None
    if existing is not None:
        _check_resume_lock(
            existing,
            source_lock,
            source_revision=source_revision,
            model_id=model_id,
            profile=profile,
            token_ids=selected_token_ids,
        )
        records: dict[str, dict[str, Any]] = {
            str(key): dict(value)
            for key, value in (existing.get("tensors") or {}).items()
            if isinstance(value, dict)
        }
        resumed_count = 0
    else:
        records = {}
        resumed_count = 0
        existing = {
            "schema": CONVERSION_SCHEMA,
            "version": 1,
            "conversion_version": 2,
            "status": "in_progress",
            "converter": CONVERTER_VERSION,
            "profile": profile,
            "token_ids": list(selected_token_ids),
            "model_identity": model_identity,
            "source": source_lock,
            "provenance_locked": bool(
                expected_sha256 and expected_size is not None and source_revision and model_id
            ),
            "tensors": records,
        }
        _atomic_json(metadata_path, existing)

    for name in source.tensor_names():
        info = source.tensor(name)
        output_name = (
            "model_embed_tokens_weight.bf16"
            if _is_embedding(name) and profile in OFFICIAL_PROFILES
            else f"{name.replace('.', '_')}.bf16"
            if profile in OFFICIAL_PROFILES and info.dtype == "BF16" and
            not _is_projection(name)
            else f"{name.replace('.', '_')}.t40"
        )
        output_path = target_dir / output_name
        old_record = records.get(name)
        if old_record and _record_matches(output_path, old_record):
            resumed_count += 1
            continue
        if old_record and old_record.get("filename") not in (None, "SKIP", output_name):
            raise ConversionError(f"tensor output name changed for {name}")

        if not _selected_for_profile(name, profile):
            record = {
                "status": "complete",
                "dtype": info.dtype,
                "shape": list(info.shape),
                "source_count": info.count,
                "count": 0,
                "filename": "SKIP",
                "scale": 1.0,
                "mode": "T40",
                "reason": f"excluded by {profile} profile",
            }
        elif profile == "generic" and not _is_projection(name) and info.count > skip_elements:
            record = {
                "status": "complete",
                "dtype": info.dtype,
                "shape": list(info.shape),
                "source_count": info.count,
                "count": info.count,
                "filename": "SKIP",
                "scale": 1.0,
                "mode": "T40",
                "reason": "high-precision tensor is read from the source model at inference time",
            }
        else:
            if not name or not output_name:
                raise ConversionError("tensor name cannot produce an output path")
            logical_shape = list(info.shape)
            logical_count = info.count
            quantization = "source_values"
            if (profile in OFFICIAL_PROFILES and info.dtype == "BF16" and
                    not _is_projection(name)):
                if info.dtype != "BF16":
                    raise ConversionError("official high-precision tensor must be BF16")
                if _is_embedding(name):
                    if len(info.shape) != 2:
                        raise ConversionError("official embedding must be rank-2 BF16")
                    hidden_size = info.shape[1]
                    row_bytes = hidden_size * 2
                    if profile == "official-full":
                        raw_chunks = source.iter_raw(info, chunk_bytes=chunk_bytes)
                        logical_shape = list(info.shape)
                        logical_count = info.count
                        quantization = "source_bf16"
                    else:
                        for token_id in selected_token_ids:
                            if token_id >= info.shape[0]:
                                raise ConversionError(
                                    f"token ID {token_id} is outside the embedding")
                        raw_chunks = (
                            chunk
                            for token_id in selected_token_ids
                            for chunk in source.iter_raw_span(
                                info,
                                token_id * row_bytes,
                                (token_id + 1) * row_bytes,
                                chunk_bytes=chunk_bytes,
                            )
                        )
                        logical_shape = [len(selected_token_ids), hidden_size]
                        logical_count = len(selected_token_ids) * hidden_size
                        quantization = "source_bf16_rows"
                else:
                    raw_chunks = source.iter_raw(info, chunk_bytes=chunk_bytes)
                    logical_shape = list(info.shape)
                    logical_count = info.count
                    quantization = "source_bf16"
                output_size, output_sha256 = _atomic_write_bytes(output_path, raw_chunks)
                record = {
                    "status": "complete",
                    "dtype": info.dtype,
                    "shape": logical_shape,
                    "source_shape": list(info.shape),
                    "source_count": info.count,
                    "count": logical_count,
                    "filename": output_name,
                    "scale": 1.0,
                    "mode": "BF16",
                    "quantization": quantization,
                    "output_size": output_size,
                    "output_sha256": output_sha256,
                }
                records[name] = record
                existing["tensors"] = records
                _atomic_json(metadata_path, existing)
                _write_manifest(target_dir / MANIFEST_NAME, records)
                continue
            elif _is_projection(name) and info.dtype == "U8":
                scale_info = source.tensors.get(name + "_scale")
                if not scale_info or scale_info.dtype != "BF16" or scale_info.shape != (1,):
                    raise ConversionError(f"packed official tensor has no BF16 scale: {name}")
                source_scale = _read_scalar(source, scale_info, chunk_bytes=chunk_bytes)
                values = _iter_packed_ternary(source, info, chunk_bytes=chunk_bytes)
                logical_shape = [info.shape[0] * 4, *info.shape[1:]]
                logical_count = info.count * 4
                # The companion scale is the real-valued magnitude applied to
                # the decoded ternary weights.  BitNet's reference kernels
                # multiply the integer dot product by this scale; storing its
                # reciprocal silently inverts every projection.
                record_scale = source_scale
                ternary = True
                quantization = "bitnet_u8_2bit"
            elif _is_projection(name):
                total = 0.0
                count = 0
                for value in source.iter_values(info, chunk_bytes=chunk_bytes):
                    numeric = float(value)
                    if not math.isfinite(numeric):
                        raise ConversionError(f"tensor contains a non-finite value: {name}")
                    total += abs(numeric)
                    count += 1
                scale = total / count if count else 0.0
                if scale < 1e-9:
                    values = (0 for _ in range(info.count))
                else:
                    def quantized() -> Iterator[int]:
                        for value in source.iter_values(info, chunk_bytes=chunk_bytes):
                            scaled = max(-1.0, min(1.0, float(value) / scale))
                            rounded = int(
                                math.floor(scaled + 0.5)
                                if scaled >= 0
                                else math.ceil(scaled - 0.5)
                            )
                            yield max(-1, min(1, rounded))
                    values = quantized()
                record_scale = scale
                ternary = True
                quantization = "generic_absmean_ternary"
            else:
                values = source.iter_values(info, chunk_bytes=chunk_bytes)
                record_scale = 1.0
                ternary = False

            def chunks() -> Iterator[bytes]:
                pending: list[int] = []
                for word in _iter_t40_words(values, ternary=ternary):
                    pending.append(word)
                    if len(pending) >= 4096:
                        yield struct.pack(f"<{len(pending)}Q", *pending)
                        pending.clear()
                if pending:
                    yield struct.pack(f"<{len(pending)}Q", *pending)

            output_size, output_sha256 = _atomic_write_bytes(output_path, chunks())
            record = {
                "status": "complete",
                "dtype": info.dtype,
                "shape": logical_shape,
                "source_shape": list(info.shape),
                "source_count": info.count,
                "count": logical_count,
                "filename": output_name,
                "scale": record_scale,
                "mode": "T40",
                "quantization": quantization,
                "output_size": output_size,
                "output_sha256": output_sha256,
            }
        records[name] = record
        existing["tensors"] = records
        _atomic_json(metadata_path, existing)
        _write_manifest(target_dir / MANIFEST_NAME, records)

    existing["status"] = "complete"
    existing["tensor_count"] = len(source.tensors)
    existing["converted_count"] = sum(
        1 for record in records.values() if record.get("filename") != "SKIP"
    )
    existing["skipped_count"] = sum(
        1 for record in records.values() if record.get("filename") == "SKIP"
    )
    _atomic_json(metadata_path, existing)
    _write_manifest(target_dir / MANIFEST_NAME, records)
    _write_guest_package(target_dir, records, existing)
    return {
        "schema": CONVERSION_SCHEMA,
        "status": existing["status"],
        "source": source_lock,
        "output_dir": str(target_dir),
        "metadata": str(metadata_path),
        "manifest": str(target_dir / MANIFEST_NAME),
        "guest_package": str(target_dir / GUEST_PACKAGE_NAME),
        "tensor_count": existing["tensor_count"],
        "converted_count": existing["converted_count"],
        "skipped_count": existing["skipped_count"],
        "resumed_count": resumed_count,
        "provenance_locked": existing["provenance_locked"],
        "profile": existing.get("profile", profile),
        "token_ids": existing.get("token_ids", list(selected_token_ids)),
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model_path")
    parser.add_argument("output_dir")
    parser.add_argument("--expected-sha256")
    parser.add_argument("--expected-size", type=int)
    parser.add_argument("--source-revision", default="")
    parser.add_argument("--model-id", default="")
    parser.add_argument("--chunk-mib", type=int, default=1)
    parser.add_argument("--skip-elements", type=int, default=DEFAULT_SKIP_ELEMENTS)
    parser.add_argument(
        "--profile",
        choices=("generic", "official-slice", "official-full"),
        default="generic",
    )
    parser.add_argument(
        "--token-ids",
        help="comma-separated fixed embedding row IDs (required for official profiles)",
    )
    parser.add_argument("--no-resume", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        result = convert_bitnet_to_trit(
            args.model_path,
            args.output_dir,
            expected_sha256=args.expected_sha256,
            expected_size=args.expected_size,
            source_revision=args.source_revision,
            model_id=args.model_id,
            chunk_bytes=args.chunk_mib * 1024 * 1024,
            skip_elements=args.skip_elements,
            resume=not args.no_resume,
            profile=args.profile,
            token_ids=args.token_ids,
        )
    except (ConversionError, OSError) as exc:
        print(f"BitNet conversion failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
