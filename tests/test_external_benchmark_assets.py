"""Offline tests for the external Doom/BitNet asset inventory."""

from __future__ import annotations

import importlib.util
import json
import struct
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "external_asset_importer", ROOT / "tools" / "import_external_benchmark_assets.py"
)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def _tiny_wad() -> bytes:
    # IWAD with one empty, project-authored marker lump.
    return b"IWAD" + struct.pack("<II", 1, 12) + struct.pack("<II", 12, 0) + b"TRITTEST"


def _tiny_safetensors() -> bytes:
    header = json.dumps(
        {
            "__metadata__": {"source": "test"},
            "weight": {
                "dtype": "I8",
                "shape": [3],
                "data_offsets": [0, 3],
            },
        },
        separators=(",", ":"),
    ).encode("utf-8")
    return struct.pack("<Q", len(header)) + header + b"\x00\x01\x02"


def test_checked_in_inventory_is_valid_without_network() -> None:
    manifest = json.loads(
        (ROOT / "benchmarks" / "assets" / "external_assets.v1.json").read_text(
            encoding="utf-8"
        )
    )
    assert MODULE.validate_manifest(manifest, root=ROOT) == []
    issues = MODULE.validate_manifest(manifest, root=ROOT, require_present=True)
    assert any("not present" in issue for issue in issues)
    assert all(asset["status"] != "present" for asset in manifest["assets"][:3])


def test_wad_bounds_and_names_are_checked() -> None:
    result = MODULE.validate_wad_bytes(_tiny_wad())
    assert result["ok"] is True
    assert result["format"] == "IWAD"
    assert result["lump_count"] == 1
    assert result["lumps"][0]["name"] == "TRITTEST"
    broken = bytearray(_tiny_wad())
    broken[8:12] = struct.pack("<I", 0xFFFF)
    assert MODULE.validate_wad_bytes(bytes(broken))["ok"] is False


def test_safetensors_header_is_checked_without_loading_payload() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "model-00001-of-00002.safetensors"
        path.write_bytes(_tiny_safetensors())
        result = MODULE.validate_model_file(path)
        assert result["ok"] is True
        assert result["format"] == "safetensors"
        assert result["tensor_count"] == 1


def test_staged_slice_import_records_shard_inventory() -> None:
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "source"
        source.mkdir()
        for index in (1, 2):
            (source / f"model-{index:05d}-of-00002.safetensors").write_bytes(
                _tiny_safetensors()
            )
        result = MODULE.import_source(
            source,
            kind="bitnet",
            stage="sliced",
            asset_id="test-sliced",
            source_url="https://huggingface.co/microsoft/bitnet-b1.58-2B-4T",
            license_name="MIT",
        )
        assert result["status"] == "present"
        assert len(result["files"]) == 2
        assert MODULE.validate_shard_set(result["files"], "sliced") == []


def test_full_slice_set_rejects_missing_shard() -> None:
    files = [
        {"name": "model-00001-of-00003.safetensors"},
        {"name": "model-00003-of-00003.safetensors"},
    ]
    issues = MODULE.validate_shard_set(files, "full")
    assert any("missing" in issue for issue in issues)


if __name__ == "__main__":
    for name, value in sorted(globals().items()):
        if name.startswith("test_") and callable(value):
            value()
    print("external benchmark assets: PASS")
