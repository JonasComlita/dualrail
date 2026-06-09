# Agent Operating Guide

This repo is intended to be agent-operable: an agent should be able to inspect state, pick a failing or incomplete area, patch it, and verify the result with minimal human input.

## First Commands

Run these from the repo root:

```powershell
tools/trit-doctor.ps1
tools/trit-test.ps1 smoke
tools/trit-test.ps1 production
tools/trit-export-diagnostics.ps1
```

Equivalent direct Python entry point:

```powershell
python tools/trit_tool.py doctor
python tools/trit_tool.py test smoke
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

## Build And Test

Default CMake build directory is `build` unless `TRIT_BUILD_DIR` is set.

Useful targets:

```powershell
cmake --build build --target build_tos_image
cmake --build build --target test_host_runtime
cmake --build build --target ci_production
cmake --build build --target stage_tos_release
cmake --build build --target smoke_tos_release
```

Preferred agent flow:

1. Run `tools/trit-doctor.ps1`.
2. Run `tools/trit-test.ps1 smoke`.
3. Read `ROADMAP_STATUS.json` and `KNOWN_GAPS.md`.
4. Pick the highest-priority failing or incomplete item.
5. Patch narrowly.
6. Run a focused suite from `TEST_MANIFEST.json`.
7. Run `tools/trit-test.ps1 production` before claiming broad OS health.
8. Export diagnostics if failure persists.
9. Update manifests or docs when the truth changes.

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
