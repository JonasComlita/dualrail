# Acceptance Criteria

Use these gates when deciding whether a change is complete.

## Narrow Code Change

- Focused target from `TEST_MANIFEST.json` passes.
- Existing behavior covered by nearby tests is not weakened.
- Manifests are updated if source-of-truth contracts changed.
- `python tools/trit_tool.py knowledge status` passes when docs, manifests, or agent tooling are touched.

## Kernel Or Syscall Change

- `tools/trit-test.ps1 os` passes, or any failing target is documented with diagnostics.
- `SYSCALL_MANIFEST.json` matches `kernel.trit`, `ternary_compiler_ir.h`, and `apps/os_sdk.trit`.
- Failure paths return clear status/detail values.
- Diagnostics expose enough process or VM state to debug a failure.

## Host Runtime Or Image Change

- `test_host_runtime` passes.
- `tools/trit-inspect-image.ps1` validates generated `.tboot` output.
- Runtime diagnostics export all files listed in `DEBUGGING.md`.
- `smoke_tos_release` or `tools/trit-run.ps1 --smoke-test` passes when SDL is available.

## Agent Tooling Or Documentation Change

- `python tools/trit_tool.py knowledge status --json` passes.
- `python tools/trit_tool.py knowledge setup --check --json` passes.
- `ctest --test-dir build -R test_agent_tooling --output-on-failure` passes.
- If Graphify or `.trit` graph extraction changed, `python tools/trit_tool.py knowledge graph --no-archive --json` succeeds and reports the expected extractor.
- Obsidian and Graphify outputs remain advisory; source files, root manifests, and test results still decide completion.

## Production Health Claim

Run:

```powershell
tools/trit-test.ps1 production
```

A production health claim is not complete until this gate passes or the exact failing target and diagnostics path are recorded.

## Full OS Capability Benchmark Claim

Doom-class and BitNet 1.58B-class benchmarks are long-running manual gates, not
current production gates. A claim that the OS is ready for full-system benchmark
tracking requires:

- The `system_benchmarks` suite in `TEST_MANIFEST.json` names real runnable targets.
- Doom or the Doom-compatible harness runs as an OS workload with framebuffer, input, timer, filesystem, and deterministic metric output.
- BitNet or the BitNet-compatible harness runs as an OS workload with model/data packaging, inference output validation, and deterministic metric output.
- Benchmark artifacts record at least load time, steady-state throughput, memory high-water mark, and relevant IO counters.
- Regressions are compared against a checked-in or archived baseline, and any expected regression is documented.
- These benchmarks stay manual until runtime, packaging, and host variability are controlled well enough for CI.

## Symbolic Encoding Change

- `docs/04_Binary_Contract/symbolic_encodings.md` is updated when ASCII, UTF-8, hexadecimal, trit literal, or ternary-native text behavior changes.
- Host-facing ASCII/UTF-8 and hexadecimal behavior remains backward compatible unless the break is explicitly documented.
- New ternary-native encodings include conversion tests for valid values, invalid values, round trips, and lossy boundaries.
- Dump tools that add ternary-native notation keep hexadecimal output available for host debugging.
- Guest-visible encodings are not considered stable ABI until tests and manifests name them explicitly.
