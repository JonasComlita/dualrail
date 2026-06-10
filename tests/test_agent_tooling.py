import json
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools" / "trit_tool.py"


def run_tool(*args):
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        cwd=REPO,
        text=True,
        capture_output=True,
        check=True,
    )


def function_body(source, name):
    start = source.index(f"def {name}")
    end = source.find("\ndef ", start + 1)
    return source[start:] if end < 0 else source[start:end]


def test_doctor_json_and_manifests():
    completed = run_tool("doctor", "--json", "--no-commands")
    report = json.loads(completed.stdout)
    assert report["ok"], report
    assert report["checks"]["required_files"]
    assert report["checks"]["json_manifests"]

    for name in [
        "ROADMAP_STATUS.json",
        "TEST_MANIFEST.json",
        "SYSCALL_MANIFEST.json",
        "IMAGE_FORMAT_MANIFEST.json",
        "APP_MANIFEST.json",
    ]:
        data = json.loads((REPO / name).read_text(encoding="utf-8"))
        assert data["version"] >= 1


def test_boot_image_inspector_when_release_image_exists():
    image = REPO / "build" / "release" / "TernaryOS" / "ternary-os.tboot"
    if not image.exists():
        return
    completed = run_tool("inspect-image", str(image), "--json")
    inspected = json.loads(completed.stdout)
    assert inspected["ok"], inspected
    assert inspected["segments"]["program_words"] > 0
    assert inspected["apps"]
    if inspected["format_version"] >= 2:
        assert inspected["sections"]
        assert inspected["segments"]["rootfs_words"] == 0


def test_product_runtime_does_not_compile_sources():
    for name in ["run_tos_sdl.cpp", "ternary_host_runtime.h"]:
        text = (REPO / name).read_text(encoding="utf-8")
        assert "ternary_compiler" not in text
        assert "compileSource" not in text
        assert "kernel.trit" not in text

    tool_source = (REPO / "tools" / "trit_tool.py").read_text(encoding="utf-8")
    cmd_run = function_body(tool_source, "cmd_run")
    assert "build_tos_image" not in cmd_run
    assert "compile" not in cmd_run


if __name__ == "__main__":
    test_doctor_json_and_manifests()
    test_boot_image_inspector_when_release_image_exists()
    test_product_runtime_does_not_compile_sources()
