"""Bounded, dependency-free safetensors metadata and tensor streaming helpers.

This module intentionally does not use ``safetensors.torch.load_file`` or
``torch.load``.  A BitNet source can be several gigabytes, so conversion opens
the source for one tensor at a time and reads only bounded byte chunks.  The
header is validated before any tensor is exposed; malformed offsets, overlaps,
shape mismatches, and truncated payloads fail closed.
"""

from __future__ import annotations

import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any, BinaryIO, Iterator, Mapping


MAX_HEADER_BYTES = 128 * 1024 * 1024
DEFAULT_CHUNK_BYTES = 1024 * 1024


class SafeTensorError(ValueError):
    """A malformed or unsupported safetensors source."""


@dataclass(frozen=True)
class TensorInfo:
    name: str
    dtype: str
    shape: tuple[int, ...]
    begin: int
    end: int

    @property
    def count(self) -> int:
        value = 1
        for dimension in self.shape:
            value *= dimension
        return value

    @property
    def byte_length(self) -> int:
        return self.end - self.begin


_DTYPES: dict[str, tuple[str, int]] = {
    "BOOL": ("<?", 1),
    "U8": ("<B", 1),
    "I8": ("<b", 1),
    "U16": ("<H", 2),
    "I16": ("<h", 2),
    "F16": ("<e", 2),
    "BF16": ("<H", 2),
    "U32": ("<I", 4),
    "I32": ("<i", 4),
    "F32": ("<f", 4),
    "U64": ("<Q", 8),
    "I64": ("<q", 8),
    "F64": ("<d", 8),
}


def _read_exact(stream: BinaryIO, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def sha256_file(path: Path, chunk_bytes: int = DEFAULT_CHUNK_BYTES) -> str:
    if chunk_bytes <= 0:
        raise ValueError("chunk_bytes must be positive")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(chunk_bytes):
            digest.update(chunk)
    return digest.hexdigest()


class SafeTensorFile:
    """Validated safetensors header plus reopenable tensor iterators."""

    def __init__(self, path: Path, *, max_header_bytes: int = MAX_HEADER_BYTES):
        self.path = Path(path)
        self.file_size = self.path.stat().st_size
        self.header_bytes = 0
        self.payload_start = 0
        self.metadata: dict[str, Any] = {}
        self.tensors: dict[str, TensorInfo] = {}
        self._load(max_header_bytes)

    def _load(self, max_header_bytes: int) -> None:
        if self.file_size < 8:
            raise SafeTensorError("safetensors file is shorter than its 8-byte header")
        with self.path.open("rb") as stream:
            prefix = _read_exact(stream, 8)
            if len(prefix) != 8:
                raise SafeTensorError("safetensors header length is truncated")
            (header_bytes,) = struct.unpack("<Q", prefix)
            if header_bytes > max_header_bytes:
                raise SafeTensorError(
                    f"safetensors header exceeds configured limit ({max_header_bytes} bytes)"
                )
            if header_bytes > self.file_size - 8:
                raise SafeTensorError("safetensors header extends beyond file")
            raw_header = _read_exact(stream, header_bytes)
        try:
            header = json.loads(raw_header.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise SafeTensorError(f"invalid safetensors JSON header: {exc}") from exc
        if not isinstance(header, dict):
            raise SafeTensorError("safetensors header must be a JSON object")
        metadata = header.get("__metadata__", {})
        if not isinstance(metadata, dict):
            raise SafeTensorError("safetensors __metadata__ must be an object")

        payload_start = 8 + int(header_bytes)
        ranges: list[tuple[int, int, str]] = []
        parsed: dict[str, TensorInfo] = {}
        for name, value in header.items():
            if name == "__metadata__":
                continue
            if not isinstance(name, str) or not isinstance(value, dict):
                raise SafeTensorError(f"tensor entry {name!r} is not an object")
            dtype = value.get("dtype")
            shape = value.get("shape")
            offsets = value.get("data_offsets")
            if not isinstance(dtype, str) or not dtype:
                raise SafeTensorError(f"tensor {name!r} has no dtype")
            if not isinstance(shape, list) or not all(
                isinstance(item, int) and not isinstance(item, bool) and item >= 0
                for item in shape
            ):
                raise SafeTensorError(f"tensor {name!r} has an invalid shape")
            if not isinstance(offsets, list) or len(offsets) != 2 or not all(
                isinstance(item, int) and not isinstance(item, bool) for item in offsets
            ):
                raise SafeTensorError(f"tensor {name!r} has invalid data_offsets")
            begin, end = offsets
            if begin < 0 or end < begin:
                raise SafeTensorError(f"tensor {name!r} has descending data_offsets")
            payload_bytes = self.file_size - payload_start
            if end > payload_bytes:
                raise SafeTensorError(f"tensor {name!r} payload extends beyond file")
            info = TensorInfo(name, dtype, tuple(shape), begin, end)
            dtype_info = _DTYPES.get(dtype)
            if dtype_info is not None and info.byte_length != info.count * dtype_info[1]:
                raise SafeTensorError(
                    f"tensor {name!r} byte length does not match dtype and shape"
                )
            ranges.append((begin, end, name))
            parsed[name] = info

        ordered = sorted(ranges)
        for previous, current in zip(ordered, ordered[1:]):
            if current[0] < previous[1]:
                raise SafeTensorError(
                    f"tensor ranges overlap: {previous[2]} and {current[2]}"
                )
        self.header_bytes = int(header_bytes)
        self.payload_start = payload_start
        self.metadata = dict(metadata)
        self.tensors = parsed

    def tensor(self, name: str) -> TensorInfo:
        try:
            return self.tensors[name]
        except KeyError as exc:
            raise SafeTensorError(f"tensor is not present: {name}") from exc

    def tensor_names(self) -> list[str]:
        return sorted(self.tensors)

    def iter_raw(
        self,
        tensor: str | TensorInfo,
        *,
        chunk_bytes: int = DEFAULT_CHUNK_BYTES,
    ) -> Iterator[bytes]:
        """Yield exactly one tensor's raw payload in bounded chunks."""

        if chunk_bytes <= 0:
            raise ValueError("chunk_bytes must be positive")
        info = self.tensor(tensor) if isinstance(tensor, str) else tensor
        with self.path.open("rb") as stream:
            stream.seek(self.payload_start + info.begin)
            remaining = info.byte_length
            while remaining:
                chunk = stream.read(min(chunk_bytes, remaining))
                if not chunk:
                    raise SafeTensorError(f"tensor payload is truncated: {info.name}")
                remaining -= len(chunk)
                yield chunk

    def iter_raw_span(
        self,
        tensor: str | TensorInfo,
        begin: int,
        end: int,
        *,
        chunk_bytes: int = DEFAULT_CHUNK_BYTES,
    ) -> Iterator[bytes]:
        """Yield a bounded byte span inside one tensor without loading it."""

        if chunk_bytes <= 0:
            raise ValueError("chunk_bytes must be positive")
        info = self.tensor(tensor) if isinstance(tensor, str) else tensor
        if begin < 0 or end < begin or end > info.byte_length:
            raise SafeTensorError(f"tensor span is out of bounds: {info.name}")
        with self.path.open("rb") as stream:
            stream.seek(self.payload_start + info.begin + begin)
            remaining = end - begin
            while remaining:
                chunk = stream.read(min(chunk_bytes, remaining))
                if not chunk:
                    raise SafeTensorError(f"tensor span is truncated: {info.name}")
                remaining -= len(chunk)
                yield chunk

    def iter_values(
        self,
        tensor: str | TensorInfo,
        *,
        chunk_bytes: int = DEFAULT_CHUNK_BYTES,
    ) -> Iterator[int | float | bool]:
        """Decode a supported tensor without retaining the tensor in memory."""

        info = self.tensor(tensor) if isinstance(tensor, str) else tensor
        dtype_info = _DTYPES.get(info.dtype)
        if dtype_info is None:
            raise SafeTensorError(f"streaming conversion does not support dtype {info.dtype}")
        fmt, item_size = dtype_info
        carry = b""
        emitted = 0
        for chunk in self.iter_raw(info, chunk_bytes=chunk_bytes):
            carry += chunk
            complete = len(carry) - (len(carry) % item_size)
            if complete:
                for (value,) in struct.iter_unpack(fmt, carry[:complete]):
                    if info.dtype == "BF16":
                        bits = int(value) << 16
                        value = struct.unpack("<f", struct.pack("<I", bits))[0]
                    emitted += 1
                    yield value
                carry = carry[complete:]
        if carry:
            raise SafeTensorError(f"tensor payload is not aligned: {info.name}")
        if emitted != info.count:
            raise SafeTensorError(
                f"tensor element count mismatch for {info.name}: expected {info.count}, observed {emitted}"
            )

    def read_values(self, tensor: str | TensorInfo) -> list[int | float | bool]:
        """Small-fixture convenience API; conversion uses :meth:`iter_values`."""

        return list(self.iter_values(tensor))


def inspect_safetensors(path: Path) -> dict[str, Any]:
    """Return JSON-safe metadata for diagnostics and conversion manifests."""

    source = SafeTensorFile(path)
    return {
        "path": str(path),
        "size": source.file_size,
        "sha256": sha256_file(path),
        "header_bytes": source.header_bytes,
        "tensor_count": len(source.tensors),
        "metadata": source.metadata,
        "tensors": [
            {
                "name": info.name,
                "dtype": info.dtype,
                "shape": list(info.shape),
                "count": info.count,
                "data_offsets": [info.begin, info.end],
            }
            for info in (source.tensors[name] for name in source.tensor_names())
        ],
    }
