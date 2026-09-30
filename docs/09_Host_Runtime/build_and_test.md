# Build and test

The core platform has one CMake graph and one current-only validation entry
point. The authoritative versions are ISA v2, executable/function ABI v3,
`.tboot` v3, and `.tdisk` v2.

Source of truth: `CMakeLists.txt`, `ARCHITECTURE_MANIFEST.json`,
`IMAGE_FORMAT_MANIFEST.json`, `TEST_MANIFEST.json`, and `AGENTS.md`.

## Build

```powershell
cmake -S . -B build_current_cleanup
cmake --build build_current_cleanup --target tritc build_tos_image
```

The optional SDL targets are enabled only when SDL2 is available:

```powershell
cmake --build build_current_cleanup --target stage_tos_release
cmake --build build_current_cleanup --target smoke_tos_release
```

## Validation

```powershell
python tools/trit_tool.py doctor
python tools/trit_tool.py contract-check
cmake --build build_current_cleanup --target current_validate
ctest --test-dir build_current_cleanup --output-on-failure
cmake --build build_current_cleanup --target ci_production
python tools/trit_tool.py export-diagnostics
```

`trit_current_conformance` is the single checked-in conformance executable. It
round-trips an ABI-v3 executable header and verifies the current version tuple.
`ci_production` builds the compiler and image builder, runs that conformance
check, and executes CTest. There are no legacy smoke wrappers, benchmark
targets, migration utilities, or v1/v2 image fixtures in the supported graph.

## Image boundary

`IMAGE_FORMAT_MANIFEST.json` is authoritative. The host runtime accepts only
`.tboot` v3 and `.tdisk` v2. Older image versions fail closed with an actionable
error; they are not migrated or guessed in place.

The release image is produced by `build_tos_image` and, when SDL is installed,
staged with `stage_tos_release`. The same current-only contract is used by the
compiler, assembler, VM, native VFS, and release builder.

## Diagnostics

```powershell
python tools/trit_tool.py export-diagnostics
```

This writes `build_current_cleanup/current-platform-diagnostics.json`. For runtime-specific
diagnostics, use the optional SDL release target and retain the generated image
and report together. Graphify and Obsidian remain advisory navigation layers;
they are not part of the production gate.
