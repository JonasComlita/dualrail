import argparse
import contextlib
import hashlib
import io
import json
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
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

    with tempfile.TemporaryDirectory() as temp_dir:
        diagnostics = Path(temp_dir)
        (diagnostics / "syscall_trace.jsonl").write_text(
            json.dumps(event) + "\n", encoding="utf-8")
        (diagnostics / "input_journal.jsonl").write_text(
            json.dumps({
                "schema": "trit.input_journal.v1",
                "sequence": 0,
                "cycle": 0,
                "kind": 2,
                "value0": 0,
                "value1": 0,
                "value2": 0,
                "text": "A",
            }) + "\n", encoding="utf-8")
        (diagnostics / "checkpoint.json").write_text(
            json.dumps({
                "schema": "trit.runtime_checkpoint.v1",
                "available": True,
                "sequence": 0,
                "input_event_count": 0,
                "cycle": 0,
                "pc": 0,
            }) + "\n", encoding="utf-8")
        completed = run_tool("replay", str(diagnostics), "--json")
        report = json.loads(completed.stdout)
        assert report["valid"], report
        assert report["artifacts"]["input_event_count"] == 1


def test_replay_versioned_adapters_and_capabilities():
    fixture_root = REPO / "tests" / "fixtures" / "replay"
    current = fixture_root / "current_trace.jsonl"
    future_minor = fixture_root / "future_minor_trace.jsonl"
    unsupported_major = fixture_root / "unsupported_major_trace.jsonl"

    current_report = json.loads(run_tool("replay", str(current), "--json").stdout)
    assert current_report["valid"], current_report
    current_capability = current_report["schema_capabilities"]["syscall_trace"]
    assert current_capability["status"] == "supported", current_capability
    assert current_capability["future_minor"] is False, current_capability

    future_report = json.loads(run_tool("replay", str(future_minor), "--json").stdout)
    assert future_report["valid"], future_report
    future_capability = future_report["schema_capabilities"]["syscall_trace"]
    assert future_capability["status"] == "future_minor", future_capability
    assert future_capability["future_minor"] is True, future_capability
    assert future_capability["unknown_fields_ignored"] == 1, future_capability

    equivalent = run_tool(
        "replay", str(current), "--against", str(future_minor), "--json"
    )
    equivalent_report = json.loads(equivalent.stdout)
    assert equivalent_report["valid"], equivalent_report
    assert equivalent_report["comparison"]["match"], equivalent_report
    assert equivalent_report["schema_capabilities"]["against"]["syscall_trace"]["future_minor"]

    rejected = subprocess.run(
        [sys.executable, str(TOOL), "replay", str(unsupported_major), "--json"],
        cwd=REPO,
        text=True,
        capture_output=True,
        check=False,
    )
    assert rejected.returncode != 0
    rejected_report = json.loads(rejected.stdout)
    assert not rejected_report["valid"], rejected_report
    assert any("unsupported trit.syscall_trace schema major v2" in issue
               for issue in rejected_report["issues"]), rejected_report
    rejected_capability = rejected_report["schema_capabilities"]["syscall_trace"]
    assert rejected_capability["status"] == "unsupported_major", rejected_capability
    assert rejected_capability["unsupported_majors"] == [2], rejected_capability


def test_replay_compares_separate_process_results():
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
        root = Path(temp_dir)
        left = root / "left"
        right = root / "right"
        for bundle in (left, right):
            bundle.mkdir()
            (bundle / "syscall_trace.jsonl").write_text(
                json.dumps(event) + "\n", encoding="utf-8"
            )
            (bundle / "input_journal.jsonl").write_text("", encoding="utf-8")
            (bundle / "checkpoint.json").write_text(
                json.dumps(
                    {
                        "schema": "trit.runtime_checkpoint.v1",
                        "available": True,
                        "sequence": 0,
                        "input_event_count": 0,
                        "cycle": 0,
                        "pc": 0,
                    }
                )
                + "\n",
                encoding="utf-8",
            )

        def fake_run(command, **_kwargs):
            # The helper's deterministic result differs only for the second
            # bundle, allowing the test to exercise the differential failure
            # path without requiring a native build.
            bundle = Path(command[1])
            pc = 12 if bundle.name == "right" else 11
            payload = {
                "ok": True,
                "status": "halted",
                "steps": 4,
                "pc": pc,
                "cycles": 9,
                "require_halt": False,
                "description": "checkpoint replay",
            }
            return SimpleNamespace(returncode=0, stdout=json.dumps(payload), stderr="")

        args = argparse.Namespace(
            trace=str(left),
            against=str(right),
            execute=True,
            build_dir=str(root / "build"),
            steps=32,
            require_halt=False,
            json=True,
        )
        output = io.StringIO()
        with patch.object(
            trit_tool, "find_executable", return_value=root / "fake-helper"
        ), patch.object(
            trit_tool.subprocess, "run", side_effect=fake_run
        ), contextlib.redirect_stdout(output):
            assert trit_tool.cmd_replay(args) == 1
        report = json.loads(output.getvalue())
        assert not report["valid"], report
        assert report["execution_comparison"]["match"] is False
        assert report["execution_comparison"]["mismatches"] == [
            {"field": "pc", "left": 11, "right": 12}
        ]


def test_structural_fuzz_is_deterministic_and_fail_closed():
    boot = REPO / "build" / "ternary-os.tboot"
    disk = REPO / "build" / "ternary-os.tdisk"
    if not boot.exists() or not disk.exists():
        return
    completed = run_tool(
        "fuzz",
        "--boot-image",
        str(boot),
        "--disk-image",
        str(disk),
        "--iterations",
        "12",
        "--seed",
        "12345",
        "--skip-tests",
        "--json",
    )
    report = json.loads(completed.stdout)
    assert report["ok"], report
    assert report["schema"] == "trit.structural_fuzz.v1"
    assert report["inputs"]["boot"]["rejected_cases"] >= 1
    assert report["inputs"]["disk"]["rejected_cases"] >= 1
    if report["inputs"]["checkpoint_replay"].get("available"):
        assert report["inputs"]["checkpoint_replay"]["rejected_cases"] >= 1

    repeated = json.loads(
        run_tool(
            "fuzz",
            "--boot-image",
            str(boot),
            "--disk-image",
            str(disk),
            "--iterations",
            "12",
            "--seed",
            "12345",
            "--skip-tests",
            "--json",
        ).stdout
    )
    assert report["inputs"]["boot"]["cases"] == repeated["inputs"]["boot"]["cases"]
    assert report["inputs"]["disk"]["cases"] == repeated["inputs"]["disk"]["cases"]
    assert (
        report["inputs"]["checkpoint_replay"]["cases"] ==
        repeated["inputs"]["checkpoint_replay"]["cases"]
    )


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


def test_knowledge_source_fingerprints_are_content_based_and_filter_outputs():
    with tempfile.TemporaryDirectory() as temp_dir:
        source = Path(temp_dir) / "source.trit"
        source.write_text("fn stable() {}\n", encoding="utf-8")
        first = trit_tool.knowledge_source_snapshot([source])
        source.write_text("fn changed() {}\n", encoding="utf-8")
        second = trit_tool.knowledge_source_snapshot([source])
        comparison = trit_tool.compare_knowledge_snapshots(first, second)
        assert not comparison["ok"], comparison
        assert comparison["changed"] == [str(source)], comparison

    assert trit_tool.knowledge_source_path_ignored(REPO / "build" / "generated.cpp")
    assert trit_tool.knowledge_source_path_ignored(REPO / "graphify-out" / "graph.json")
    assert not trit_tool.knowledge_source_path_ignored(REPO / "tools" / "trit_tool.py")


def test_knowledge_graphify_legacy_and_stale_runs_warn_actionably():
    with tempfile.TemporaryDirectory() as temp_dir:
        archive = Path(temp_dir) / "runs"
        run = archive / "20990101T000000Z-test"
        run.mkdir(parents=True)
        (run / "run.json").write_text(json.dumps({"created_at": "20990101T000000Z"}), encoding="utf-8")
        (run / "manifest.json").write_text("{}", encoding="utf-8")
        with patch.object(trit_tool, "GRAPHIFY_ARCHIVE_DIR", archive):
            legacy = trit_tool.graphify_freshness_status(
                {"schema": trit_tool.KNOWLEDGE_FRESHNESS_SCHEMA, "files": [], "fingerprint": "empty"}
            )
        assert legacy["state"] == "legacy_unverified", legacy
        assert "cannot be proven current" in legacy["message"]

        run2 = archive / "20990102T000000Z-test"
        run2.mkdir(parents=True)
        (run2 / "run.json").write_text(
            json.dumps(
                {
                    "created_at": "20990102T000000Z",
                    "source_snapshot": {
                        "schema": trit_tool.KNOWLEDGE_FRESHNESS_SCHEMA,
                        "fingerprint": "stale",
                        "files": [{"path": "tools/trit_tool.py", "sha256": "stale"}],
                    },
                }
            ),
            encoding="utf-8",
        )
        with patch.object(trit_tool, "GRAPHIFY_ARCHIVE_DIR", archive):
            stale = trit_tool.graphify_freshness_status()
        assert stale["state"] == "stale", stale
        assert "rerun `python tools/trit_tool.py knowledge graph`" in stale["message"]


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


def _string_words(value):
    return [len(value), *map(ord, value)]


def _manifest_registry_words(manifest):
    gui = [entry for entry in manifest["bundled_apps"] if entry.get("gui_registry")]
    words = [len(gui)]
    for entry in gui:
        words.extend(_string_words(entry["id"]))
        words.extend(_string_words(entry["title"]))
        words.extend(_string_words(entry["guest_path"]))
        words.extend([1, 1])
    return words


def test_app_package_and_registry_validation():
    report = json.loads(run_tool("apps", "validate", "--json").stdout)
    assert report["ok"], report
    assert report["schema"] == "trit.app_validation_report.v1"
    assert report["checks"]["manifest"]["entry_count"] >= 50
    assert report["checks"]["builder_alignment"]["ok"]
    assert report["checks"]["registry"]["expected_gui_entry_count"] == 10

    manifest = json.loads((REPO / "APP_MANIFEST.json").read_text(encoding="utf-8"))
    payload = [84, 82, 73, 84]
    package_words = [90909, 1, 42, 1]
    package_words.extend(_string_words("base"))
    package_words.extend([0])
    package_words.extend(_string_words("/etc/motd"))
    package_words.extend(
        [len(payload), trit_tool.stable_app_word_hash(payload), 0]
    )

    with tempfile.TemporaryDirectory() as temp_dir:
        temp = Path(temp_dir)
        registry_path = temp / "registry.json"
        registry_path.write_text(json.dumps(_manifest_registry_words(manifest)), encoding="utf-8")
        registry = json.loads(run_tool("apps", "validate", "--registry", str(registry_path), "--json").stdout)
        assert registry["ok"], registry
        assert registry["checks"]["registry"]["sources"][0]["decoded"]["record_count"] == 10

        malformed_registry = _manifest_registry_words(manifest)[:-1]
        malformed_path = temp / "registry-truncated.json"
        malformed_path.write_text(json.dumps(malformed_registry), encoding="utf-8")
        rejected = subprocess.run(
            [sys.executable, str(TOOL), "apps", "validate", "--registry", str(malformed_path), "--json"],
            cwd=REPO,
            text=True,
            capture_output=True,
            check=False,
        )
        assert rejected.returncode != 0
        assert not json.loads(rejected.stdout)["ok"]

        package_path = temp / "base.manifest.json"
        package_path.write_text(json.dumps(package_words), encoding="utf-8")
        package = json.loads(run_tool("apps", "validate", "--package-manifest", str(package_path), "--json").stdout)
        assert package["ok"], package
        assert package["checks"]["packages"]["items"][0]["decoded"]["name"] == "base"

        duplicate = list(package_words)
        duplicate[4] = 5
        duplicate_path = temp / "bad.manifest.json"
        duplicate_path.write_text(json.dumps(duplicate), encoding="utf-8")
        bad_package = subprocess.run(
            [sys.executable, str(TOOL), "apps", "validate", "--package-manifest", str(duplicate_path), "--json"],
            cwd=REPO,
            text=True,
            capture_output=True,
            check=False,
        )
        assert bad_package.returncode != 0
        assert not json.loads(bad_package.stdout)["ok"]

    disk = REPO / "build" / "release" / "TernaryOS" / "ternary-os.tdisk"
    if disk.exists():
        installed = json.loads(run_tool("apps", "validate", "--disk-image", str(disk), "--json").stdout)
        assert installed["ok"], installed
        assert installed["checks"]["disk"]["file_count"] > 0


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


def test_trit_adapter_crosslinks_manifests_image_sections_and_test_sources():
    graph = trit_tool.extract_trit_graph(prefer_ast=False)
    cross_links = graph["cross_links"]
    assert cross_links["schema"] == "trit.graph_adapter.v2"
    assert cross_links["syscall_services"] == len(
        json.loads((REPO / "SYSCALL_MANIFEST.json").read_text(encoding="utf-8"))["services"]
    )
    assert cross_links["app_bundles"] == len(
        json.loads((REPO / "APP_MANIFEST.json").read_text(encoding="utf-8"))["bundled_apps"]
    )
    assert cross_links["image_sections"] == cross_links["app_bundles"] + 1
    assert cross_links["test_coverage_edges"] > 0
    for relation in ("syscall_id", "owns", "produces", "covers"):
        edges = [edge for edge in graph["edges"] if edge.get("relation") == relation]
        assert edges, relation
        assert all(edge.get("source_file") and edge.get("source_location") for edge in edges)
        assert all(edge.get("confidence") in {"EXTRACTED", "INFERRED"} for edge in edges)

    node_ids = [node["id"] for node in graph["nodes"]]
    assert len(node_ids) == len(set(node_ids))
    edge_keys = [
        (
            edge.get("source"),
            edge.get("target"),
            edge.get("relation"),
            edge.get("source_file"),
            edge.get("source_location"),
            edge.get("context"),
        )
        for edge in graph["edges"]
    ]
    assert len(edge_keys) == len(set(edge_keys))

    with tempfile.TemporaryDirectory() as temp_dir:
        graph_path = Path(temp_dir) / "graph.json"
        first = trit_tool.augment_graph_with_trit(graph_path)
        second = trit_tool.augment_graph_with_trit(graph_path)
        assert first["cross_links"]["relation_counts"] == second["cross_links"]["relation_counts"]
        persisted = json.loads(graph_path.read_text(encoding="utf-8"))
        persisted_ids = [node["id"] for node in persisted["nodes"]]
        assert len(persisted_ids) == len(set(persisted_ids))


if __name__ == "__main__":
    test_doctor_json_and_manifests()
    test_boot_image_inspector_when_release_image_exists()
    test_product_runtime_does_not_compile_sources()
    test_replay_validates_and_compares_syscall_traces()
    test_replay_compares_separate_process_results()
    test_structural_fuzz_is_deterministic_and_fail_closed()
    test_v1_fixture_provenance_and_checksums()
    test_knowledge_obsidian_and_graphify_integration()
    test_knowledge_source_fingerprints_are_content_based_and_filter_outputs()
    test_knowledge_graphify_legacy_and_stale_runs_warn_actionably()
    test_treatcode_registry_and_coverage()
    test_app_package_and_registry_validation()
    test_trit_adapter_augments_graphify_graph()
