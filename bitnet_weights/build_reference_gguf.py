#!/usr/bin/env python3
"""Build a GGUF reference model from the locked BitNet safetensors payload.

The published safetensors and GGUF assets are independently versioned.  They
therefore cannot be used as a correctness pair when their packed weights do
not match.  This tool keeps the pinned GGUF header, vocabulary, and tensor
geometry, then replaces every model tensor with the equivalent stream-decoded
safetensors value.  The resulting file is local cache output only and is
intended for the pinned bitnet.cpp runner used to establish golden tokens.

Projection tensors use the official BitNet I2_S layout: four logical ternary
values per byte, high pair first, followed by the 32-byte aligned scale tail.
No tensor is materialized in full while it is rewritten.
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
from typing import Any, Iterable, Iterator


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bitnet_weights"))
sys.path.insert(0, str(ROOT / "build" / "external-assets"))

from extract_bitnet import (  # type: ignore[import-not-found]
    DEFAULT_CHUNK_BYTES,
    _iter_packed_ternary,
    _read_scalar,
    sha256_file,
)
from safetensors_stream import SafeTensorFile, TensorInfo  # type: ignore[import-not-found]


GGUF_PY = "gguf-py"
BLOCK_RE = re.compile(r"^blk\.(\d+)\.(.+)$")

BLOCK_SOURCE_SUFFIXES = {
    "attn_norm.weight": "input_layernorm.weight",
    "attn_sub_norm.weight": "self_attn.attn_sub_norm.weight",
    "attn_k.weight": "self_attn.k_proj.weight",
    "attn_output.weight": "self_attn.o_proj.weight",
    "attn_q.weight": "self_attn.q_proj.weight",
    "attn_v.weight": "self_attn.v_proj.weight",
    "ffn_norm.weight": "post_attention_layernorm.weight",
    "ffn_sub_norm.weight": "mlp.ffn_sub_norm.weight",
    "ffn_down.weight": "mlp.down_proj.weight",
    "ffn_gate.weight": "mlp.gate_proj.weight",
    "ffn_up.weight": "mlp.up_proj.weight",
}


class ReferenceModelError(ValueError):
    """Raised when the two model containers cannot be paired safely."""


def _source_name(target_name: str) -> str:
    if target_name == "token_embd.weight":
        return "model.embed_tokens.weight"
    if target_name == "output_norm.weight":
        return "model.norm.weight"
    match = BLOCK_RE.match(target_name)
    if not match:
        raise ReferenceModelError(f"unsupported GGUF tensor: {target_name}")
    layer, suffix = match.groups()
    source_suffix = BLOCK_SOURCE_SUFFIXES.get(suffix)
    if source_suffix is None:
        raise ReferenceModelError(f"unsupported GGUF block tensor: {target_name}")
    return f"model.layers.{layer}.{source_suffix}"


def _iter_bf16_output(
    source: SafeTensorFile,
    info: TensorInfo,
    target_type: str,
    *,
    chunk_bytes: int,
) -> Iterator[bytes]:
    """Convert one BF16 source tensor into the target GGUF float type."""

    if info.dtype != "BF16":
        raise ReferenceModelError(f"{info.name} must be BF16, got {info.dtype}")
    if target_type not in {"F16", "F32"}:
        raise ReferenceModelError(f"unsupported float target type: {target_type}")

    # The official converter performs this cast with NumPy/PyTorch.  Keep the
    # same IEEE conversion boundary, but do it per bounded chunk so the large
    # embedding tensor does not spend minutes in a Python per-element loop.
    try:
        import numpy as np  # type: ignore[import-not-found]
    except ImportError:
        np = None  # type: ignore[assignment]
    if np is not None:
        carry = b""
        emitted = 0
        for chunk in source.iter_raw(info, chunk_bytes=chunk_bytes):
            carry += chunk
            complete = len(carry) - (len(carry) % 2)
            if complete:
                bits = np.frombuffer(carry[:complete], dtype=np.dtype("<u2"))
                values = (bits.astype(np.dtype("<u4")) << np.uint32(16)).view(
                    np.dtype("<f4")
                )
                if target_type == "F16":
                    yield values.astype(np.dtype("<f2")).tobytes()
                else:
                    yield values.astype(np.dtype("<f4"), copy=False).tobytes()
                emitted += int(bits.size)
            carry = carry[complete:]
        if carry:
            raise ReferenceModelError(f"BF16 tensor is not element-aligned: {info.name}")
        if emitted != info.count:
            raise ReferenceModelError(
                f"BF16 count mismatch for {info.name}: expected {info.count}, observed {emitted}"
            )
        return

    output = bytearray()
    carry = b""
    emitted = 0
    item_size = 2
    output_fmt = "<e" if target_type == "F16" else "<f"
    output_size = 2 if target_type == "F16" else 4
    for chunk in source.iter_raw(info, chunk_bytes=chunk_bytes):
        carry += chunk
        complete = len(carry) - (len(carry) % item_size)
        for (bits,) in struct.iter_unpack("<H", carry[:complete]):
            value = struct.unpack("<f", struct.pack("<I", int(bits) << 16))[0]
            output.extend(struct.pack(output_fmt, value))
            emitted += 1
            if len(output) >= chunk_bytes:
                yield bytes(output)
                output.clear()
        carry = carry[complete:]
    if carry:
        raise ReferenceModelError(f"BF16 tensor is not element-aligned: {info.name}")
    if emitted != info.count:
        raise ReferenceModelError(
            f"BF16 count mismatch for {info.name}: expected {info.count}, observed {emitted}"
        )
    if output:
        yield bytes(output)
    if emitted * output_size != info.count * output_size:
        raise ReferenceModelError(f"float output size mismatch for {info.name}")


def _iter_i2_output(
    source: SafeTensorFile,
    info: TensorInfo,
    scale_info: TensorInfo,
    *,
    chunk_bytes: int,
) -> Iterator[bytes]:
    """Pack source U8 planes into the official GGUF I2_S byte stream."""

    if info.dtype != "U8" or len(info.shape) != 2:
        raise ReferenceModelError(f"I2 source must be rank-2 U8: {info.name}")
    if info.shape[0] * 4 <= 0:
        raise ReferenceModelError(f"I2 source has invalid shape: {info.name}")
    scale = _read_scalar(source, scale_info, chunk_bytes=chunk_bytes)

    # The source byte stream is already grouped by source row and column.  A
    # logical plane is one selected pair from each source byte.  The official
    # converter expands four complete planes, then reshapes the flattened
    # logical tensor into 128-value blocks.  In each block, the values at
    # offsets 0, 32, 64, and 96 form one packed output byte.
    try:
        import numpy as np  # type: ignore[import-not-found]
    except ImportError:
        np = None  # type: ignore[assignment]
    if np is not None:
        written = 0
        for shift in (0, 2, 4, 6):
            carry = b""
            for chunk in source.iter_raw(info, chunk_bytes=chunk_bytes):
                carry += chunk
                complete = len(carry) - (len(carry) % 128)
                if complete:
                    codes = (
                        np.frombuffer(carry[:complete], dtype=np.dtype("u1"))
                        .reshape(-1, 128)
                        >> np.uint8(shift)
                    ) & np.uint8(3)
                    packed = (
                        (codes[:, 0:32] << np.uint8(6))
                        | (codes[:, 32:64] << np.uint8(4))
                        | (codes[:, 64:96] << np.uint8(2))
                        | codes[:, 96:128]
                    )
                    encoded = packed.astype(np.dtype("u1"), copy=False).reshape(-1).tobytes()
                    yield encoded
                    written += len(encoded)
                carry = carry[complete:]
            if carry:
                raise ReferenceModelError(f"I2 source is not 128-byte block aligned: {info.name}")
        if written != info.count:
            raise ReferenceModelError(
                f"I2 packed count mismatch for {info.name}: expected {info.count}, observed {written}"
            )
        yield struct.pack("<f", float(scale)) + bytes(28)
        return

    # Scalar fallback: mirror the same 128-value block layout as the vector
    # path without retaining more than one block of source bytes.
    for shift in (0, 2, 4, 6):
        block = bytearray()
        for raw_chunk in source.iter_raw(info, chunk_bytes=chunk_bytes):
            block.extend(raw_chunk)
            while len(block) >= 128:
                source_block = block[:128]
                del block[:128]
                encoded = bytearray()
                for offset in range(32):
                    codes = [
                        ((source_block[offset + group * 32] >> shift) & 3)
                        for group in range(4)
                    ]
                    if any(code == 3 for code in codes):
                        raise ReferenceModelError(f"reserved I2 code for {info.name}")
                    encoded.append(
                        (codes[0] << 6) | (codes[1] << 4) | (codes[2] << 2) | codes[3]
                    )
                yield bytes(encoded)
        if block:
            raise ReferenceModelError(f"I2 source is not 128-byte block aligned: {info.name}")
    yield struct.pack("<f", float(scale)) + bytes(28)


def _load_gguf_tensors(gguf_path: Path, gguf_py: Path) -> list[tuple[str, str, int, int]]:
    sys.path.insert(0, str(gguf_py))
    try:
        from gguf.gguf_reader import GGUFReader  # type: ignore[import-not-found]
    except ImportError as exc:
        raise ReferenceModelError(f"cannot import pinned gguf-py from {gguf_py}") from exc
    reader = GGUFReader(gguf_path)
    tensors = [
        (tensor.name, tensor.tensor_type.name, int(tensor.data_offset), int(tensor.data.nbytes))
        for tensor in reader.tensors
    ]
    del reader
    return tensors


def _write_tensor(
    output,
    *,
    chunks: Iterable[bytes],
    expected_size: int,
    name: str,
) -> int:
    written = 0
    for chunk in chunks:
        output.write(chunk)
        written += len(chunk)
    if written != expected_size:
        raise ReferenceModelError(
            f"rewritten tensor size mismatch for {name}: expected {expected_size}, observed {written}"
        )
    return written


def build_reference_gguf(
    source_path: str | Path,
    base_gguf: str | Path,
    output_path: str | Path,
    *,
    metadata_path: str | Path | None = None,
    gguf_py: str | Path | None = None,
    source_revision: str = "",
    model_id: str = "",
    runner_revision: str = "",
    chunk_bytes: int = DEFAULT_CHUNK_BYTES,
    force: bool = False,
) -> dict[str, Any]:
    source_file = Path(source_path).resolve()
    base_file = Path(base_gguf).resolve()
    output_file = Path(output_path).resolve()
    if not source_file.is_file() or not base_file.is_file():
        raise ReferenceModelError("source safetensors and base GGUF must both be files")
    if chunk_bytes <= 0:
        raise ReferenceModelError("chunk_bytes must be positive")
    if output_file.exists() and not force:
        raise ReferenceModelError(f"output exists; use --force to replace it: {output_file}")

    gguf_py_path = Path(gguf_py).resolve() if gguf_py else ROOT / "build" / "external-assets" / "bitnet.cpp" / "0b341e582afbf9e1011f24744b554c96a3477eb5" / "3rdparty" / "llama.cpp" / GGUF_PY
    tensor_records = _load_gguf_tensors(base_file, gguf_py_path)
    source = SafeTensorFile(source_file)
    base_hash = sha256_file(base_file)
    source_hash = sha256_file(source_file)

    output_file.parent.mkdir(parents=True, exist_ok=True)
    partial = output_file.with_name(output_file.name + ".partial")
    if partial.exists():
        partial.unlink()
    shutil.copyfile(base_file, partial)

    rewritten: list[dict[str, Any]] = []
    try:
        with partial.open("r+b") as output:
            for target_name, target_type, data_offset, data_size in tensor_records:
                source_name = _source_name(target_name)
                info = source.tensor(source_name)
                output.seek(data_offset)
                if target_type == "I2_S":
                    scale_info = source.tensor(source_name + "_scale")
                    chunks = _iter_i2_output(source, info, scale_info, chunk_bytes=chunk_bytes)
                elif target_type in {"F16", "F32"}:
                    chunks = _iter_bf16_output(
                        source, info, target_type, chunk_bytes=chunk_bytes
                    )
                else:
                    raise ReferenceModelError(
                        f"unsupported target tensor type {target_type} for {target_name}"
                    )
                written = _write_tensor(
                    output, chunks=chunks, expected_size=data_size, name=target_name
                )
                rewritten.append(
                    {
                        "target_name": target_name,
                        "source_name": source_name,
                        "target_type": target_type,
                        "output_size": written,
                    }
                )
            output.flush()
            import os

            os.fsync(output.fileno())
        partial.replace(output_file)
    except Exception:
        partial.unlink(missing_ok=True)
        raise

    output_hash = sha256_file(output_file)
    result: dict[str, Any] = {
        "schema": "trit.bitnet_reference_gguf.v1",
        "version": 1,
        "status": "complete",
        "source": {
            "path": str(source_file),
            "size": source_file.stat().st_size,
            "sha256": source_hash,
            "source_revision": source_revision,
            "model_id": model_id,
        },
        "base_gguf": {
            "path": str(base_file),
            "size": base_file.stat().st_size,
            "sha256": base_hash,
            "runner_revision": runner_revision,
        },
        "output": {
            "path": str(output_file),
            "size": output_file.stat().st_size,
            "sha256": output_hash,
        },
        "tensor_count": len(rewritten),
        "tensors": rewritten,
    }
    if metadata_path is None:
        metadata_file = output_file.with_name(output_file.name + ".v1.json")
    else:
        metadata_file = Path(metadata_path).resolve()
    metadata_file.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="locked model.safetensors")
    parser.add_argument("base_gguf", type=Path, help="pinned official GGUF container")
    parser.add_argument("output", type=Path, help="ignored derived GGUF output")
    parser.add_argument("--metadata", type=Path, default=None)
    parser.add_argument("--gguf-py", type=Path, default=None)
    parser.add_argument("--source-revision", default="")
    parser.add_argument("--model-id", default="")
    parser.add_argument("--runner-revision", default="")
    parser.add_argument("--chunk-bytes", type=int, default=DEFAULT_CHUNK_BYTES)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    try:
        result = build_reference_gguf(
            args.source,
            args.base_gguf,
            args.output,
            metadata_path=args.metadata,
            gguf_py=args.gguf_py,
            source_revision=args.source_revision,
            model_id=args.model_id,
            runner_revision=args.runner_revision,
            chunk_bytes=args.chunk_bytes,
            force=args.force,
        )
    except (OSError, ReferenceModelError, ValueError) as exc:
        print(f"reference GGUF build failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
