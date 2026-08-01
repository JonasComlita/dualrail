import hashlib
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
        "STACK_MANIFEST.json",
        "CAPABILITY_MANIFEST.json",
        "CONTRACT_MANIFEST.json",
        "DECISION_MANIFEST.json",
        "STACK_COVERAGE_REPORT.json",
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


def test_replay_validates_and_compares_syscall_traces():
    event = {
        "schema": "trit.syscall_trace.v1",
        "sequence": 0,
        "pc": 7,
        "physical_pc": 7,
        "syscall_id": 19,
        "process_id": 0,
        "before_privilege": 0,
        "after_privilege": 0,
        "args": [3, 0, 0, 0],
        "results": [4096, 0, 0],
        "before_status": 0,
        "after_status": 0,
        "trap": 0,
        "trap_code": 0,
        "trap_cause": 0,
        "cycle_before": 7,
        "cycle_after": 8,
    }
    with tempfile.TemporaryDirectory() as temp_dir:
        first = Path(temp_dir) / "first.jsonl"
        second = Path(temp_dir) / "second.jsonl"
        first.write_text(json.dumps(event) + "\n", encoding="utf-8")
        second.write_text(json.dumps(event, sort_keys=True) + "\n", encoding="utf-8")
        completed = run_tool("replay", str(first), "--against", str(second), "--json")
        report = json.loads(completed.stdout)
        assert report["valid"], report
        assert report["comparison"]["match"], report

        bad = dict(event)
        bad["sequence"] = 2
        second.write_text(json.dumps(bad) + "\n", encoding="utf-8")
        rejected = subprocess.run(
            [sys.executable, str(TOOL), "replay", str(first), "--against", str(second)],
            cwd=REPO,
            text=True,
            capture_output=True,
            check=False,
        )
        assert rejected.returncode != 0
        assert "sequence" in rejected.stderr


def test_v1_fixture_provenance_and_checksums():
    fixture_root = REPO / "tests" / "fixtures" / "v1"
    manifest = json.loads(
        (fixture_root / "manifest.json").read_text(encoding="utf-8")
    )
    assert manifest["source_commit"] == "f5b5b3d"
    assert manifest["source_tag"] == "trit-v1-final"
    assert manifest["historical_only"]
    assert not manifest["runtime_execution_allowed"]

    roles = set()
    for artifact in manifest["artifacts"]:
        path = fixture_root / artifact["path"]
        assert path.is_file(), artifact
        roles.update(artifact.get("roles", []))
        if "sha256" in artifact:
            digest = hashlib.sha256(path.read_bytes()).hexdigest().upper()
            assert digest == artifact["sha256"], artifact
            assert path.stat().st_size == artifact["bytes"], artifact
    assert {
        "instruction",
        "executable",
        "syscall",
        "boot-image",
        "disk-image",
        "wal-recovery-seed",
        "offline-migration",
    } <= roles


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


def test_treatcode_registry_and_coverage():
    registry = json.loads(run_tool("website", "registry", "validate", "--json").stdout)
    assert registry["ok"], registry
    assert registry["checks"]["stack_layers"] == 21
    assert registry["checks"]["capabilities"] >= 21

    coverage = json.loads(
        run_tool("website", "registry", "coverage", "--strict", "--json").stdout
    )
    assert coverage["ok"], coverage
    assert coverage["coverage"]["covered_layers"] == 21

    stack = json.loads((REPO / "STACK_MANIFEST.json").read_text(encoding="utf-8"))
    capabilities = json.loads((REPO / "CAPABILITY_MANIFEST.json").read_text(encoding="utf-8"))
    encoding_names = {
        item["encoding_name"]
        for item in capabilities["capabilities"]
        if item.get("capability_class") == "symbolic_encoding"
    }
    assert {"ASCII", "UTF-8", "hexadecimal", "TASCII-81"} <= encoding_names
    encrypted = [
        item for item in capabilities["capabilities"]
        if item.get("design_family") == "encrypted_volume"
    ]
    assert {item["encoding_family"] for item in encrypted} == {"compatibility", "ternary_native"}
    assert len(stack["layers"]) == 21


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
    test_replay_validates_and_compares_syscall_traces()
    test_v1_fixture_provenance_and_checksums()
    test_knowledge_obsidian_and_graphify_integration()
    test_treatcode_registry_and_coverage()
    test_trit_adapter_augments_graphify_graph()
