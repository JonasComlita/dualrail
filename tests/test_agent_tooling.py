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


if __name__ == "__main__":
    test_doctor_json_and_manifests()
    test_boot_image_inspector_when_release_image_exists()
