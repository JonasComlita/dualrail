"""Exercise the explicit host TRITENC1 volume command lifecycle."""

from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def fnv1a(data: bytes) -> int:
    value = 1469598103934665603
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def canonical_tdisk() -> bytes:
    records = bytearray()
    for block in range(32):
        words = [0] * 27
        words[block % 27] = block + 7
        records += struct.pack("<I", block + 1)
        for word in words:
            records += struct.pack("<Q", word)
    return (
        struct.pack("<QIIQQI", 0x54524954535032, 2, 27, 4,
                    fnv1a(records), 32)
        + records
    )


def binary_path() -> Path:
    requested = os.environ.get("TRIT_VOLUME_BINARY")
    if requested:
        return Path(requested).resolve()
    for candidate in (
        ROOT / "build" / "trit_volume.exe",
        ROOT / "build" / "trit_volume",
    ):
        if candidate.is_file():
            return candidate
    raise AssertionError("trit_volume binary is missing")


def run(binary: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), *args],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def assert_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        raise AssertionError(
            f"{label} failed ({result.returncode}): "
            f"{result.stdout}\n{result.stderr}"
        )


def main() -> int:
    binary = binary_path()
    require_provider = os.environ.get("TRIT_VOLUME_CLI_REQUIRE_OPENSSL") == "1"
    with tempfile.TemporaryDirectory(prefix="trit-volume-cli-") as raw:
        root = Path(raw)
        source = root / "source.tdisk"
        envelope = root / "source.tenc"
        recovered = root / "recovered.tdisk"
        recovered_after_attach = root / "recovered-after-attach.tdisk"
        key = root / "volume.key"
        child_script = root / "check_environment.py"
        failing_script = root / "fail_child.py"
        payload = canonical_tdisk()
        source.write_bytes(payload)
        key.write_bytes(bytes(range(32)))

        encrypted = run(
            binary, "encrypt", "--key-file", str(key), "--chunk-bytes", "4096",
            str(source), str(envelope)
        )
        if encrypted.returncode != 0:
            combined = encrypted.stdout + encrypted.stderr
            if require_provider or not any(
                marker in combined.lower()
                for marker in ("provider", "nonce generation failed")
            ):
                raise AssertionError(
                    f"OpenSSL volume encryption failed ({encrypted.returncode}): "
                    f"{combined}"
                )
            print("encrypted volume CLI: fail-closed without OpenSSL")
            return 0

        inspection = run(binary, "inspect", "--json", str(envelope))
        assert_ok(inspection, "inspect")
        metadata = json.loads(inspection.stdout)
        assert metadata["version"] == 1
        assert metadata["algorithm"] == "AES-256-GCM"
        assert metadata["chunk_count"] > 1

        assert_ok(
            run(binary, "decrypt", "--key-file", str(key),
                str(envelope), str(recovered)),
            "decrypt",
        )
        assert recovered.read_bytes() == payload

        refused = run(
            binary, "decrypt", "--key-file", str(key), str(envelope),
            str(recovered)
        )
        assert refused.returncode != 0
        assert "destination exists" in refused.stderr

        child_script.write_text(
            "from pathlib import Path\n"
            "import os\n"
            "path = os.environ.get('TRIT_VOLUME_PLAINTEXT')\n"
            "assert path and Path(path).is_file()\n"
            "assert Path(path).read_bytes()\n",
            encoding="utf-8",
        )
        failing_script.write_text("raise SystemExit(7)\n", encoding="utf-8")
        attached = run(
            binary, "attach/run", "--key-file", str(key), str(envelope), "--",
            sys.executable, str(child_script),
        )
        assert_ok(attached, "attach/run")
        assert_ok(
            run(binary, "decrypt", "--key-file", str(key), "--overwrite",
                str(envelope), str(recovered_after_attach)),
            "decrypt after attach",
        )
        assert recovered_after_attach.read_bytes() == payload

        failing_child = run(
            binary, "attach/run", "--key-file", str(key), str(envelope), "--",
            sys.executable, str(failing_script),
        )
        assert failing_child.returncode != 0
        assert_ok(
            run(binary, "decrypt", "--key-file", str(key), "--overwrite",
                str(envelope), str(recovered_after_attach)),
            "decrypt after failed child",
        )
        assert recovered_after_attach.read_bytes() == payload

        temp_root = Path(tempfile.gettempdir())
        assert not list(temp_root.glob("trit-volume-attach.tdisk*"))

    print("encrypted volume CLI: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
