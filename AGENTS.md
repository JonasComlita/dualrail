# Agent Operating Guide

This repo is intended to be agent-operable: an agent should be able to inspect state, pick a failing or incomplete area, patch it, and verify the result with minimal human input.

## First Commands

Run these from the repo root:

```powershell
tools/trit-doctor.ps1
tools/trit-test.ps1 smoke
python tools/trit_tool.py knowledge status
tools/trit-test.ps1 production
tools/trit-export-diagnostics.ps1
```

Equivalent direct Python entry point:

```powershell
python tools/trit_tool.py doctor
python tools/trit_tool.py test smoke
python tools/trit_tool.py knowledge status
python tools/trit_tool.py export-diagnostics
```

## Source Of Truth

- `TEST_MANIFEST.json`: test suites and authoritative targets.
- `ROADMAP_STATUS.json`: phase status, evidence, and open items.
- `SYSCALL_MANIFEST.json`: syscall ABI and compiler wrapper names.
- `IMAGE_FORMAT_MANIFEST.json`: `.tboot` and `.tdisk` format contract.
- `APP_MANIFEST.json`: bundled OS apps and guest paths.
- `DEBUGGING.md`: diagnostics and failure triage workflow.
- `KNOWN_GAPS.md`: known missing or partial work.
- `ACCEPTANCE_CRITERIA.md`: gates for claiming work complete.
- `docs/`: Obsidian vault for navigable explanations and architecture canvas.

## Obsidian And Graphify

The `docs/` directory is an Obsidian-friendly vault. Open `docs/` directly in
Obsidian for linked reference docs and the `trit-stack.canvas` architecture map.

Use the stable tool entry point instead of ad-hoc vault edits:

```powershell
python tools/trit_tool.py knowledge status
python tools/trit_tool.py knowledge canvas
python tools/trit_tool.py knowledge setup --check
python tools/trit_tool.py knowledge graph
```

Graphify is optional and advisory. Its raw output goes to ignored
`graphify-out/`, and archived snapshots go under ignored `docs/_graphify/runs/`.
Do not treat Graphify reports or Obsidian notes as more authoritative than
source files, manifests, or test results.

Graphify does not parse `.trit` natively, so `knowledge graph` augments
Graphify output with a project-local Trit extractor. When the `trit_ast_dump`
CMake target is built, that extractor uses the compiler parser's `ModuleAst`;
otherwise it falls back to a lighter text scan. Rebuild the graph after changing
compiler, kernel, app, or TCL sources if symbol navigation matters.

## Build And Test

Default CMake build directory is `build` unless `TRIT_BUILD_DIR` is set.

Useful targets:

```powershell
cmake --build build --target build_tos_image
cmake --build build --target test_host_runtime
cmake --build build --target trit_ast_dump
cmake --build build --target ci_production
cmake --build build --target stage_tos_release
cmake --build build --target smoke_tos_release
```

Preferred agent flow:

1. Run `tools/trit-doctor.ps1`.
2. Run `tools/trit-test.ps1 smoke`.
3. Run `python tools/trit_tool.py knowledge status`.
4. Read `ROADMAP_STATUS.json`, `KNOWN_GAPS.md`, and relevant docs vault pages.
5. Use `python tools/trit_tool.py knowledge graph --no-archive` when a structural code graph would help.
6. Pick the highest-priority failing or incomplete item.
7. Patch narrowly.
8. Run a focused suite from `TEST_MANIFEST.json`.
9. Run `tools/trit-test.ps1 production` before claiming broad OS health.
10. Export diagnostics if failure persists.
11. Update manifests or docs when the truth changes.

## What Not To Delete

- Do not delete `tests/`; it is the authoritative regression seed.
- Do not delete `apps/`, `kernel.trit`, `ternary_host_runtime.h`, or image builder/runtime files.
- Do not delete existing build artifacts unless explicitly cleaning a build.
- Do not revert unrelated dirty files. Treat them as user work.

## Adding Syscalls

When adding a syscall, update all relevant surfaces:

- Kernel dispatch in `kernel.trit`.
- Runtime IDs in `ternary_compiler_ir.h`.
- Compiler wrapper mapping in `ternary_compiler_codegen.h`.
- App SDK wrappers in `apps/os_sdk.trit` when user code should call it.
- `SYSCALL_MANIFEST.json`.
- Focused tests, usually `test_os_platform`, `test_phase_d_kernel`, or `test_native_apps`.

## Adding Apps

When adding a bundled app:

- Add source under `apps/`.
- Ensure it compiles with `apps/os_sdk.trit` and `apps/libwidget.trit` if needed.
- Add it to `build_tos_image.cpp`.
- Add it to `APP_MANIFEST.json`.
- Add focused app or process-handoff tests.
- Rebuild and inspect the release image with `tools/trit-inspect-image.ps1`.

## Release Image Validation

Use:

```powershell
cmake --build build --target stage_tos_release
tools/trit-inspect-image.ps1 build/release/TernaryOS/ternary-os.tboot
tools/trit-run.ps1 --smoke-test --frames 10 --export-diagnostics build/agent-smoke-diagnostics
```

The `.tboot` and `.tdisk` formats are described in `IMAGE_FORMAT_MANIFEST.json`.
