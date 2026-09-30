# Agent Operating Guide

## Core Engineering Principles

- Think from first principles. Start with the desired behavior, constraints, and
  evidence, then derive the simplest implementation that satisfies them.
- Apply KISS (Keep It Simple). Prefer the smallest clear design with the fewest
  moving parts, and do not introduce abstractions, dependencies, or machinery
  until the problem justifies them.

## Testing Principles

- NEVER write unit tests after you write code.
- Highly prefer E2E tests as the sole testing mechanism. Use them to verify that
  complex features work. At the end of E2E tests, produce a verifiable and
  repeatable artifact.
- If a system must be tested in isolation, FIRST write down all the ways it
  could fail, THEN write the code and tests needed to expose those failures.
- When writing E2E tests, do not choose the simplest possible scenario merely
  to prove that the feature works. Use a medium-to-hard scenario that exercises
  meaningful behavior and realistic interactions.

When implementing with test-driven development:

- Tautological tests are harmful.
- Change-detector tests are harmful.
- Do not create regression tests for bug fixes without a genuine gap in behavior
  testing.

## How To: Review → Repair → Validate

Use this loop for implementation work:

1. Review: inspect the task context, current artifact, requirements, and likely
   failure modes. Return structured, evidence-based findings before editing.
2. Repair: apply the smallest focused change that addresses the findings while
   preserving unrelated user work.
3. Validate: run the relevant checks, strongly preferring a medium-to-hard E2E
   scenario. Produce a verifiable, repeatable artifact from E2E validation and
   report the commands, results, and any remaining issues.
4. Iterate: use unresolved findings and validation failures as the input to the
   next repair pass. Claim completion only when the behavior is validated, or
   clearly state the exact blocker.

This repo is intentionally agent-operable. The supported surface is one CMake
graph, one current-platform conformance executable, and one small Python entry
point. Do not recreate the deleted legacy test suites, wrapper scripts, or image
migration programs.

## First commands

Run these from the repository root:

```powershell
python tools/trit_tool.py doctor
python tools/trit_tool.py contract-check
python tools/trit_tool.py test
python tools/trit_tool.py export-diagnostics
```

The equivalent build gate is:

```powershell
cmake -S . -B build_current_cleanup
cmake --build build_current_cleanup --target ci_production
```

## Current platform boundary

The only supported platform contract is:

- ISA encoding v2.
- Executable and function ABI v3; syscall ABI v2.
- `.tboot` format v3.
- `.tdisk` format v2.

Compilers, assemblers, loaders, the VM, the native VFS, and image builders must
emit and accept those versions only. Retired executable/image codecs are not
part of the current source graph; old callers must be rebuilt from source and
old artifacts must be rejected rather than decoded, installed, migrated, or
silently reinterpreted.

## Source of truth

- `ARCHITECTURE_MANIFEST.json`: ISA, ABI, and generated-contract inputs.
- `IMAGE_FORMAT_MANIFEST.json`: `.tboot` and `.tdisk` wire formats.
- `SYSCALL_MANIFEST.json`: syscall IDs and wrapper names.
- `APP_MANIFEST.json`: bundled OS apps and guest paths.
- `TEST_MANIFEST.json`: the current conformance and production gates.
- `ROADMAP_STATUS.json`: phase status, evidence, and open items.
- `KNOWN_GAPS.md`: known missing or partial work.
- `README.md`: progress and current project context.
- `tcl_native_rewrite.md` and `TCL_Spec_1.0.md`: language/compiler direction.
- `architecture_contract.h` and `.trit`: checked-in contract snapshot.
- `docs/`: navigable architecture and explanatory reference material.

TreatCode website sources and generated site artifacts are outside the core
platform build graph. Obsidian and Graphify outputs are advisory reference
material, not authority for implementation or version support.

## Build graph

```powershell
cmake -S . -B build_current_cleanup
cmake --build build_current_cleanup --target tritc build_tos_image
cmake --build build_current_cleanup --target current_validate
ctest --test-dir build_current_cleanup --output-on-failure
cmake --build build_current_cleanup --target ci_production
```

SDL release targets (`run_tos_sdl`, `stage_tos_release`,
`smoke_tos_release`, and `package_tos_release`) are optional and exist only
when SDL2 is available. They must consume the same current-only image formats.

## What not to delete

- `apps/`, `kernel.trit`, the compiler/runtime headers, and image builders.
- The current manifests listed above and the generated architecture snapshot.
- `README.md`, `ROADMAP_STATUS.json`, `KNOWN_GAPS.md`, and the two TCL design
  documents, which record project direction or unresolved risk.
- `docs/` reference material unless a source-path move requires a link repair.

The old `tests/`, `tests_next/`, and `tools/` contents were intentionally
retired. Keep only the replacement conformance source and `tools/trit_tool.py`
needed by the current graph; do not add legacy wrappers or one-off fixtures.

## Adding syscalls or apps

When the current platform genuinely gains a syscall, update the kernel dispatch,
runtime IDs, compiler wrapper mapping, SDK wrapper (when user code calls it),
`SYSCALL_MANIFEST.json`, and the current conformance path together.

When adding a bundled app, add its source under `apps/`, compile it with the
current SDK, register it in `build_tos_image.cpp` and `APP_MANIFEST.json`, and
verify the produced current `.tboot`/`.tdisk` image.

## Release validation

```powershell
cmake --build build_current_cleanup --target stage_tos_release
cmake --build build_current_cleanup --target smoke_tos_release
```

The release image must remain ISA v2, executable ABI v3, `.tboot` v3, and
`.tdisk` v2. Older artifacts are rejected and are not converted in place.

<!-- BEGIN BEADS CODEX SETUP: generated by bd setup codex -->
## Beads Issue Tracker

Use Beads (`bd`) for durable task tracking in repositories that include it. Use the `beads` skill at `.agents/skills/beads/SKILL.md` (project install) or `~/.agents/skills/beads/SKILL.md` (global install) for Beads workflow guidance, then use the `bd` CLI for issue operations.

### Quick Reference

```bash
bd ready                # Find available work
bd show <id>            # View issue details
bd update <id> --claim  # Claim work
bd close <id>           # Complete work
bd prime                # Refresh Beads context
```

### Rules

- Use `bd` for all task tracking; do not create markdown TODO lists.
- Run `bd prime` when Beads context is missing or stale. Codex 0.129.0+ can load Beads context automatically through native hooks; use `/hooks` to inspect or toggle them.
- Keep persistent project memory in Beads via `bd remember`; do not create ad hoc memory files.

**Architecture in one line:** issues live in a local Dolt DB; sync uses `refs/dolt/data` on your git remote; `.beads/issues.jsonl` is a passive export. See https://github.com/gastownhall/beads/blob/main/docs/core-concepts/sync-concepts.md for details and anti-patterns.
<!-- END BEADS CODEX SETUP -->
