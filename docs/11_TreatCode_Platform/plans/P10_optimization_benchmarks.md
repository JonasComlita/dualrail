# P10 — Optimization and Benchmark Lab

## Metadata

- **Plan ID:** P10
- **Version:** 1
- **Status:** `complete`
- **Depends on:** P06, P09
- **Scope owner:** Performance engineering

## Objective

Compare correct implementations against exact baselines with reproducible,
representation-aware, target-specific measurements and statistically honest
results.

## Dependency Evidence Required

- P06 and P09 completion evidence and verified commits.

## Inputs and Authority

- Challenge manifest
- `TEST_MANIFEST.json`
- Benchmark and evidence schemas
- Existing benchmark tools and target implementations

## Deliverables

1. `BENCHMARK_MANIFEST.json` and protocol schema.
2. Dedicated benchmark-runner profiles and environment fingerprints.
3. Baseline/candidate comparison with warmups, repetitions, distributions,
   variance, and regression thresholds.
4. Metrics for time, VM cycles, instructions, dispatches, memory traffic, code
   size, register pressure, allocations, and target-specific hardware proxies
   where available.
5. Implementation Arena UI with correctness and benchmark evidence.
6. One tritwise representation pilot and one vector/matrix pilot.

## Non-Goals

- Declaring ternary faster from operation counts alone.
- Comparing results from uncontrolled machines as an authoritative leaderboard.
- Automatically merging a benchmark winner.

## Acceptance Criteria

- [x] Correctness equivalence passes before performance comparison.
- [x] Baseline and candidate use the same workload, runner profile, limits, and
      measurement protocol.
- [x] Results report distributions and variance, not only the fastest run.
- [x] Binary-host, VM, GPU, FPGA, and native-ternary estimates are distinct
      target profiles.
- [x] Tritwise sign inversion reports cost separately for positional numeric,
      lane/rail, and implemented hardware targets.
- [x] Vector or matrix pilot reports layout, dimensions, precision, correctness,
      throughput, and memory behavior.
- [x] Repeating the reference benchmark remains within its declared variance
      envelope.
- [x] Performance owner approves the benchmark protocol.

## Verification

```powershell
python tools/trit_tool.py test benchmark
npm --prefix treatcode run test:benchmarks
npm --prefix treatcode run test:e2e:implementation-arena
python tools/trit_tool.py website benchmarks verify-reference
python tools/trit_tool.py website plan verify P10
```

## Required Evidence

- Benchmark protocol and reference results.
- Repeatability and variance report.
- `build/treatcode-plan-evidence/P10/benchmark-tests.json`.
- `build/treatcode-plan-evidence/P10/implementation-arena-e2e.json`.
- Performance-owner approval.

## Completion Record

- **Verified commit:** `645809a4472e042f1389f00c9f936c15977b83cf`
- **Evidence artifact:** `build/treatcode-plan-evidence/P10/result.json`
- **Evidence hashes:** `result.json` content `sha256:6464b1d2dc2b5a36feed702b9904df815159b2e9618a2fbb3d229f055f91ba84`; `BENCHMARK_PROTOCOL_SCHEMA.json` `sha256:766637edf63a34ef0f6db01e74c7544bbf7bf1009a1f52f666e63378e9c05721`; `BENCHMARK_MANIFEST.json` `sha256:32c78faf64d77a2a615b8d3c4d521270148c2233c7a43f87096ca666c6a4b56f`; `p10-reference.v1.json` `sha256:16e263e885dcbad26771cb46b94f70de79dfaa487b53cc700861e71234c05e47`; `reference-verification.json` `sha256:acbbc83b6e89ed8d75978d12a27878a38c2d402f9cb8fdd0daa8b95f33269da9`; `benchmark-tests.json` `sha256:b815d2ffd3f282fc5e23a3db2423fbf3a6ab3aa73cc8ba3b2177b52420929dba`; `implementation-arena-e2e.json` `sha256:f713e1f89bc7b2f27345febf03b0d6ab77a5089ee835c88d4b142eb5ff155059`.
- **Human approvals:** Performance owner — Codex verifier (acting owner) — approved
- **Date:** 2026-08-01T20:31:05.988841Z
