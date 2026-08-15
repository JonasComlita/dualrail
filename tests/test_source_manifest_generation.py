"""Focused checks for the source-contract manifest generator."""

from __future__ import annotations

import hashlib
import json
import shutil
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools" / "generate_source_manifests.py"
sys.path.insert(0, str(REPO / "tools"))
import generate_source_manifests as generator  # noqa: E402


def _run(*args: str):
    import subprocess

    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        cwd=REPO,
        text=True,
        capture_output=True,
        check=False,
    )


def _fixture(tmp_path: Path) -> Path:
    root = tmp_path / "repo"
    root.mkdir()
    for name in (
        "ARCHITECTURE_MANIFEST.json",
        "SYSCALL_MANIFEST.json",
        "APP_MANIFEST.json",
        "IMAGE_FORMAT_MANIFEST.json",
        "ternary_compiler_ir.h",
        "ternary_compiler_codegen.h",
        "ternary_vm_state.h",
        "ternary_host_runtime.h",
        "build_tos_image.cpp",
    ):
        destination = root / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / name, destination)
    shutil.copytree(REPO / "apps", root / "apps")
    (root / "generated").mkdir(parents=True, exist_ok=True)
    shutil.copy2(REPO / "generated" / "source_contract_manifest.json", root / "generated" / "source_contract_manifest.json")
    return root


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_current_sidecar_is_canonical_and_check_is_clean():
    result = _run("--check")
    assert result.returncode == 0, result.stderr or result.stdout
    sidecar = json.loads((REPO / "generated" / "source_contract_manifest.json").read_text(encoding="utf-8"))
    assert sidecar["schema"] == "trit.source_contract_manifest.v1"
    assert sidecar["apps"]["count"] == 59
    assert sidecar["apps"]["gui_count"] == 10
    assert sidecar["syscalls"]["compiler_wrappers"]
    assert {item["constant"] for item in sidecar["syscalls"]["legacy_conflicts"]} == {
        "SYSCALL_EXIT",
        "SYSCALL_SLEEP_UNTIL_TICK",
        "SYSCALL_WAITPID",
        "SYSCALL_YIELD",
    }


def test_check_rejects_root_manifest_drift(tmp_path):
    root = _fixture(tmp_path)
    path = root / "SYSCALL_MANIFEST.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    data["services"][-1]["id"] = 58
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    result = _run("--check", "--repo", str(root))
    assert result.returncode != 0
    assert "SYSCALL_MANIFEST.json" in result.stderr


def test_check_rejects_source_duplicate_id(tmp_path):
    root = _fixture(tmp_path)
    path = root / "ternary_compiler_ir.h"
    source = path.read_text(encoding="utf-8")
    source = source.replace(
        "static constexpr int sys_rename = 59;",
        "static constexpr int sys_rename = 59;\nstatic constexpr int accidental_duplicate = 1;",
    )
    path.write_text(source, encoding="utf-8")
    result = _run("--check", "--repo", str(root))
    assert result.returncode != 0
    assert "multiple canonical constants" in result.stderr


def test_write_only_refreshes_sidecar_and_preserves_root_manifests(tmp_path):
    root = _fixture(tmp_path)
    manifest_paths = [root / name for name in ("SYSCALL_MANIFEST.json", "APP_MANIFEST.json", "IMAGE_FORMAT_MANIFEST.json")]
    before = {path: _sha256(path) for path in manifest_paths}
    output = root / "generated" / "refreshed.json"
    result = _run("--write", "--repo", str(root), "--output", str(output))
    assert result.returncode == 0, result.stderr or result.stdout
    assert output.is_file()
    assert before == {path: _sha256(path) for path in manifest_paths}


if __name__ == "__main__":
    import tempfile

    test_current_sidecar_is_canonical_and_check_is_clean()
    with tempfile.TemporaryDirectory() as directory:
        test_check_rejects_root_manifest_drift(Path(directory))
    with tempfile.TemporaryDirectory() as directory:
        test_check_rejects_source_duplicate_id(Path(directory))
    with tempfile.TemporaryDirectory() as directory:
        test_write_only_refreshes_sidecar_and_preserves_root_manifests(Path(directory))
