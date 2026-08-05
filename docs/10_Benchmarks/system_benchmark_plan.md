# System Benchmark Plan

This directory defines long-horizon benchmark goals for proving Ternary OS as a
complete operating system, not just a collection of passing component tests.

The benchmark pair is intentionally asymmetric:

- Doom-class workload: interactive real-time graphics, input, timers, asset IO,
  scheduling, memory allocation, and eventually audio.
- BitNet 1.58B-class workload: large model loading, memory pressure, disk
  throughput, vector/kernel performance, accelerator paths, and long-running
  process stability.

The deterministic synthetic pair is part of the current `production` and
release gates. Larger external Doom/BitNet assets remain follow-on workload
profiles, not prerequisites for this compact correctness and stability gate.

## Benchmark Suite Contract

`TEST_MANIFEST.json` exposes the manual `system_benchmarks` suite with runnable
deterministic targets:

- `benchmark_doom_os`
- `benchmark_bitnet_os`

Both targets are real CMake executables and emit
`trit.benchmark_result.v1` JSON under `build/benchmarks/` by default.

## Required Artifact Shape

Each benchmark should emit a JSON artifact with:

- benchmark id and version
- git commit or source identity
- host profile and build profile
- workload inputs
- pass/fail correctness result
- load time
- steady-state throughput
- memory high-water mark
- disk IO counters when relevant
- scheduler/frame/timer counters when relevant
- diagnostics path when failure occurs

The artifact format should be stable enough for regression comparison.

## Staged Rollout

1. Define tiny synthetic harnesses that exercise the same OS surfaces.
2. Package harness assets into `.tdisk` or release image artifacts.
3. Add host-side metric extraction and JSON reports.
4. Add manual CMake targets.
5. Add baseline capture tooling.
6. Only after repeatability is proven, consider CI or release-gate use.

## Authority

These docs are planning guidance. The source of truth for runnable suites is
`TEST_MANIFEST.json`; the source of truth for acceptance gates is
`ACCEPTANCE_CRITERIA.md`.
