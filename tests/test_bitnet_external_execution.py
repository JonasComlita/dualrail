"""Focused offline tests for real BitNet conversion/reference contracts."""

from __future__ import annotations

import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CONVERTER = _load("bitnet_converter", ROOT / "bitnet_weights" / "extract_bitnet.py")
REFERENCE = _load("bitnet_reference", ROOT / "bitnet_weights" / "official_reference.py")


def _tiny_safetensors() -> bytes:
    payload = bytearray()
    entries: dict[str, dict[str, object]] = {}

    def add(name: str, dtype: str, shape: list[int], data: bytes) -> None:
        begin = len(payload)
        payload.extend(data)
        entries[name] = {
            "dtype": dtype,
            "shape": shape,
            "data_offsets": [begin, len(payload)],
        }

    # Four consecutive official I2 values, packed least-significant-pair first:
    # -1, 0, +1, 0 -> codes 0, 1, 2, 1.
    add("layer.q_proj.weight", "U8", [1, 1], bytes([0 | (1 << 2) | (2 << 4) | (1 << 6)]))
    add("layer.q_proj.weight_scale", "BF16", [1], struct.pack("<H", 0x4000))
    add("small", "BF16", [2], struct.pack("<HH", 0x3F80, 0x4000))
    header = json.dumps(entries, separators=(",", ":")).encode("utf-8")
    return struct.pack("<Q", len(header)) + header + bytes(payload)


def _decode_first_trits(path: Path, count: int = 4) -> list[int]:
    (raw,) = struct.unpack("<Q", path.read_bytes()[:8])
    values: list[int] = []
    for _ in range(count):
        values.append(raw % 3 - 1)
        raw //= 3
    return values


def test_packed_u8_is_decoded_low_pair_order_with_logical_count() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        source = root / "model.safetensors"
        output = root / "converted"
        source.write_bytes(_tiny_safetensors())
        result = CONVERTER.convert_bitnet_to_trit(
            source,
            output,
            expected_sha256=CONVERTER.sha256_file(source),
            expected_size=source.stat().st_size,
            source_revision="fixture-revision",
            model_id="fixture-model",
            skip_elements=1,
        )
        assert result["status"] == "complete"
        record = json.loads(
            (output / CONVERTER.METADATA_NAME).read_text(encoding="utf-8")
        )["tensors"]["layer.q_proj.weight"]
        assert record["quantization"] == "bitnet_u8_2bit"
        assert record["count"] == 4
        assert record["shape"] == [4, 1]
        assert record["scale"] == 2.0
        assert _decode_first_trits(output / "layer_q_proj_weight.t40") == [-1, 0, 1, 0]


def test_official_packed_u8_expands_in_row_plane_order() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        source = root / "model.safetensors"
        output = root / "converted"
        payload = bytearray()
        entries: dict[str, dict[str, object]] = {}

        def add(name: str, dtype: str, shape: list[int], data: bytes) -> None:
            begin = len(payload)
            payload.extend(data)
            entries[name] = {
                "dtype": dtype,
                "shape": shape,
                "data_offsets": [begin, len(payload)],
            }

        # Packed rows A and B.  The official expansion is the four planes
        # [A0,B0], [A1,B1], [A2,B2], [A3,B3], not byte-local [A0..A3,B0..B3].
        add("layer.q_proj.weight", "U8", [2, 1], bytes([
            0 | (1 << 2) | (2 << 4) | (1 << 6),
            2 | (0 << 2) | (1 << 4) | (2 << 6),
        ]))
        add("layer.q_proj.weight_scale", "BF16", [1], struct.pack("<H", 0x4000))
        header = json.dumps(entries, separators=(",", ":")).encode("utf-8")
        source.write_bytes(struct.pack("<Q", len(header)) + header + bytes(payload))

        CONVERTER.convert_bitnet_to_trit(
            source,
            output,
            expected_sha256=CONVERTER.sha256_file(source),
            expected_size=source.stat().st_size,
            source_revision="fixture-revision",
            model_id="fixture-model",
            skip_elements=1,
        )
        assert _decode_first_trits(output / "layer_q_proj_weight.t40", 8) == [
            -1, 1, 0, -1, 1, 0, 0, 1
        ]


def test_conversion_resume_repairs_corruption_and_rejects_source_change() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        source = root / "model.safetensors"
        output = root / "converted"
        source.write_bytes(_tiny_safetensors())
        digest = CONVERTER.sha256_file(source)
        kwargs = dict(
            expected_sha256=digest,
            expected_size=source.stat().st_size,
            source_revision="fixture-revision",
            model_id="fixture-model",
            skip_elements=1,
        )
        CONVERTER.convert_bitnet_to_trit(source, output, **kwargs)
        artifact = output / "layer_q_proj_weight.t40"
        original = artifact.read_bytes()
        artifact.write_bytes(bytes([original[0] ^ 0x01]) + original[1:])
        resumed = CONVERTER.convert_bitnet_to_trit(source, output, **kwargs)
        assert resumed["resumed_count"] >= 1
        assert artifact.read_bytes() == original
        assert not artifact.with_name(artifact.name + ".part").exists()

        source.write_bytes(_tiny_safetensors()[:-1] + b"X")
        try:
            CONVERTER.convert_bitnet_to_trit(source, output, **kwargs)
        except CONVERTER.ConversionError as exc:
            assert "SHA-256" in str(exc)
        else:
            raise AssertionError("source corruption was accepted")


def test_reference_requires_two_matching_token_runs_and_records_hash() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        model = root / "ggml-model-i2_s.gguf"
        model.write_bytes(b"GGUF fixture")
        output = root / "reference.v1.json"
        completed = subprocess.CompletedProcess([], 0, stdout="token_ids: 10, 11, 12\n", stderr="")
        with patch.object(REFERENCE.subprocess, "run", side_effect=[completed, completed]):
            evidence = REFERENCE.run_reference(
                runner=sys.executable,
                runner_revision="test-runner-revision",
                model=model,
                prompt="fixed prompt",
                length=3,
                threads=4,
                input_token_ids=(128000, 42),
                expected_model_sha256=REFERENCE.sha256_file(model),
                output=output,
            )
        assert evidence["status"] == "pass"
        assert evidence["matching_runs"] == 2
        assert evidence["token_ids"] == [10, 11, 12]
        assert json.loads(output.read_text(encoding="utf-8"))["token_hash"].startswith("sha256:")

        mismatch_output = root / "mismatch.json"
        second = subprocess.CompletedProcess([], 0, stdout="token_ids: 10, 11, 99\n", stderr="")
        with patch.object(REFERENCE.subprocess, "run", side_effect=[completed, second]):
            try:
                REFERENCE.run_reference(
                    runner=sys.executable,
                    runner_revision="test-runner-revision",
                    model=model,
                    prompt="fixed prompt",
                    length=3,
                    threads=4,
                    input_token_ids=(128000, 42),
                    output=mismatch_output,
                )
            except REFERENCE.ReferenceError as exc:
                assert "different token IDs" in str(exc)
            else:
                raise AssertionError("reference mismatch was accepted")
        assert json.loads(mismatch_output.read_text(encoding="utf-8"))["status"] == "mismatch"


if __name__ == "__main__":
    for name, value in sorted(globals().items()):
        if name.startswith("test_") and callable(value):
            value()
    print("BitNet external execution contracts: PASS")
