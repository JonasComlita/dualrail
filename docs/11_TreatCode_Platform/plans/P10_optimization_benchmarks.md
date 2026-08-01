# P10 — Optimization and Benchmark Lab

## Metadata

- **Plan ID:** P10
- **Version:** 1
- **Status:** `in_progress`
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
- [ ] Performance owner approves the benchmark protocol.

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

- **Verified commit:**
- **Evidence artifact:**
- **Human approvals:** Performance owner
- **Date:**
