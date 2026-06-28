# Agent Handoff Guide

This file is the first stop for any agent working in `tests_next/`.

## Non-Negotiables

- Do not delete or rewrite the historical `tests/` suite as part of this work.
- Do not create another monolithic test file.
- Every test must have a stable ID using dotted names, for example
  `vm.mmu.zero_pte_faults`.
- Every test must be tied to at least one claim in
  `manifests/coverage_matrix.json`.
- Prefer deterministic tests. If randomness is needed, record the seed.
- A feature is considered covered only when it has a focused test, a grouped
  subsystem path, and at least one integration path if it crosses layers.
- Tests may also reveal optimization or simplification opportunities. Keep
  those observations tied to deterministic evidence such as counters,
  differential runs, fault masks, allocation behavior, or stable timings, and
  record follow-up work in `manifests/status.json`.

## Test ID Format

Use:

```text
<layer>.<feature>.<behavior>
```

Examples:

```text
isa.branch.brp_encoding
vm.mmu.zero_pte_faults
compiler.match.tsel_side_effect_guard
vfs.recovery.commit_replay
gui.compositor.z_order_occlusion
full_system.desktop_reboot_persistence
```

## Per-Test Metadata

Each new test should record:

- `id`
- `layer`
- `feature`
- `kind`: `focused`, `group`, `integration`, `full_system`, `fuzz`, or `perf`
- `source`: `new`, `ported`, or `expanded`
- `old_refs`: old tests that previously covered part of the claim
- `proves`: short list of invariants
- `gate`: intended gate such as `ci_fast`, `ci_full`, or `ci_release`

Use `manifests/test_case.template.json` as the shape.

## Handoff Notes

Before ending work, update `manifests/status.json`:

- add completed test IDs;
- move newly discovered gaps into `next_tasks`;
- record optimization or improvement opportunities discovered by the tests;
- record blockers with enough detail for another agent to continue;
- record the exact commands that were run.

Do not rely on chat history as the only handoff.
