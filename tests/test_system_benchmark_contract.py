"""Fast contract checks for the deterministic Doom/BitNet portfolio gates."""

from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_schema_requires_sustained_portfolio_metrics() -> None:
    schema = json.loads((ROOT / "BENCHMARK_SCHEMA.json").read_text(encoding="utf-8"))
    branches = schema["allOf"]
    doom = next(
        branch for branch in branches
        if branch.get("if", {}).get("properties", {}).get("workload", {})
        .get("properties", {}).get("name", {}).get("const") == "doom-class-os"
    )
    bitnet = next(
        branch for branch in branches
        if branch.get("if", {}).get("properties", {}).get("workload", {})
        .get("properties", {}).get("name", {}).get("const") == "bitnet-class-os"
    )
    assert "asset_shards" in doom["then"]["properties"]["workload"]["required"]
    assert "draw_calls" in doom["then"]["properties"]["graphics"]["required"]
    assert "model_shards" in bitnet["then"]["properties"]["workload"]["required"]
    assert "pressure_pages" in bitnet["then"]["properties"]["memory"]["required"]
    assert "vector_kernel_invocations" in bitnet["then"]["properties"]["compute"]["required"]
    assert schema["properties"]["probe"] == {"type": "boolean"}
    assert any(
        branch.get("if", {}).get("properties", {}).get("probe", {}).get("const") is True
        for branch in branches
    )


def test_source_profiles_preserve_two_warmups_and_seven_samples() -> None:
    doom = (ROOT / "benchmarks" / "benchmark_doom_os.cpp").read_text(encoding="utf-8")
    bitnet = (ROOT / "benchmarks" / "benchmark_bitnet_os.cpp").read_text(encoding="utf-8")
    support = (ROOT / "benchmarks" / "system_benchmark_support.h").read_text(encoding="utf-8")
    assert "kGateFrames = 7" in doom
    assert "kLargeFrames = 81" in doom
    assert "kGateShards = 3" in bitnet
    assert "kLargeShards = 9" in bitnet
    assert "kGateComputeRepetitions = 243" in bitnet
    assert "TRIT_BENCH_PROBE" in doom and "TRIT_BENCH_PROBE" in bitnet
    tool = (ROOT / "tools" / "trit_system_bench.py").read_text(encoding="utf-8")
    assert "MAX_SAMPLE_SECONDS = 60.0" in tool
    assert "validate_probe_report" in tool
    assert "kWarmups = 2" in support
    assert "kIterations = 7" in support
    assert "kMaximumAcceptedCv = 0.03" in support


if __name__ == "__main__":
    test_schema_requires_sustained_portfolio_metrics()
    test_source_profiles_preserve_two_warmups_and_seven_samples()
    print("system benchmark contract: PASS")
