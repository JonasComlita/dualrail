import json
import subprocess
import sys
import tempfile
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools" / "trit_tool.py"
sys.path.insert(0, str(REPO / "tools"))
import trit_tool  # noqa: E402


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
        "ARCHITECTURE_MANIFEST.json",
        "ROADMAP_STATUS.json",
        "TEST_MANIFEST.json",
        "SYSCALL_MANIFEST.json",
        "IMAGE_FORMAT_MANIFEST.json",
        "APP_MANIFEST.json",
    ]:
        data = json.loads((REPO / name).read_text(encoding="utf-8"))
        assert data["version"] >= 1

    generated = subprocess.run(
        [sys.executable, str(REPO / "tools" / "generate_architecture_contract.py"), "--check"],
        cwd=REPO,
        text=True,
        capture_output=True,
        check=False,
    )
    assert generated.returncode == 0, generated.stderr or generated.stdout

    architecture = json.loads(
        (REPO / "ARCHITECTURE_MANIFEST.json").read_text(encoding="utf-8")
    )
    contract = architecture["architecture"]
    assert contract["instruction_trits"] == 27
    assert contract["scalar_word_trits"] == 40
    assert contract["base_page_words"] == 729
    assert contract["superpage_words"] == 19683


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


def test_knowledge_obsidian_and_graphify_integration():
    completed = run_tool("knowledge", "status", "--json")
    report = json.loads(completed.stdout)
    assert report["ok"], report
    assert report["obsidian"]["canvas"]["ok"], report
    assert report["markdown"]["broken_links"] == []
    assert report["graphify"]["ignore"].endswith(".graphifyignore")

    for item in report["obsidian"]["required_files"]:
        assert item["exists"], item

    setup = json.loads(run_tool("knowledge", "setup", "--check", "--json").stdout)
    assert setup["ok"], setup

    canvas_check = json.loads(run_tool("knowledge", "canvas", "--check", "--json").stdout)
    assert canvas_check["ok"], canvas_check

    canvas = json.loads((REPO / "docs" / "trit-stack.canvas").read_text(encoding="utf-8"))
    assert canvas["nodes"], canvas
    assert canvas["edges"], canvas
    assert any(node.get("file") == "README.md" for node in canvas["nodes"])
    assert any(node.get("file") == "_graphify/README.md" for node in canvas["nodes"])


def test_trit_adapter_augments_graphify_graph():
    trit_graph = trit_tool.extract_trit_graph()
    assert trit_graph["extractor"] in {"ast", "regex"}, trit_graph
    assert trit_graph["files"] > 10, trit_graph
    assert trit_graph["functions"] > 100, trit_graph
    assert trit_graph["constants"] > 100, trit_graph
    assert trit_graph["call_edges"] > 100, trit_graph
    if trit_graph["extractor"] == "ast":
        assert trit_graph["modules"] == trit_graph["files"], trit_graph
        assert trit_graph["structs"] > 0, trit_graph

    labels = {node["label"] for node in trit_graph["nodes"]}
    assert "kernel.trit" in labels
    assert "kernel_init()" in labels
    assert "os_open()" in labels

    with tempfile.TemporaryDirectory() as temp_dir:
        graph_path = Path(temp_dir) / "graph.json"
        graph_path.write_text(
            json.dumps({"nodes": [], "edges": [], "hyperedges": [], "input_tokens": 0, "output_tokens": 0}),
            encoding="utf-8",
        )
        summary = trit_tool.augment_graph_with_trit(graph_path)
        assert summary["ok"], summary
        assert summary["extractor"] in {"ast", "regex"}, summary
        assert summary["functions"] == trit_graph["functions"]
        graph = json.loads(graph_path.read_text(encoding="utf-8"))
        assert any(node.get("_origin") == "trit-adapter" for node in graph["nodes"])
        assert any(edge.get("context") == "trit_call" for edge in graph["edges"])


if __name__ == "__main__":
    test_doctor_json_and_manifests()
    test_boot_image_inspector_when_release_image_exists()
    test_product_runtime_does_not_compile_sources()
    test_knowledge_obsidian_and_graphify_integration()
    test_trit_adapter_augments_graphify_graph()
