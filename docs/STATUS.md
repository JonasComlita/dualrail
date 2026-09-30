# Current project status

This page is a short operational summary. The authoritative phase state is
[`ROADMAP_STATUS.json`](../ROADMAP_STATUS.json); architectural contracts live
in [`ARCHITECTURE_MANIFEST.json`](../ARCHITECTURE_MANIFEST.json) and
[`IMAGE_FORMAT_MANIFEST.json`](../IMAGE_FORMAT_MANIFEST.json).

## Current platform

- ISA v2 instruction encoding and VM.
- Executable header and function ABI v3.
- tBoot v3 and tDisk v2 image formats.
- Syscall ABI v2 and vector ABI 1 are part of that current platform.
- Non-current executable or image versions are rejected. There is no runtime
  compatibility loader or offline migration program.

## Current validation surface

| Surface | Entry point | Purpose |
| --- | --- | --- |
| Contract check | `python tools/trit_tool.py contract-check` | Verify the version tuple and current-only manifests. |
| Conformance | `cmake --build build_current_cleanup --target current_validate` | Build and run the small current-platform conformance executable. |
| Production gate | `cmake --build build_current_cleanup --target ci_production` | Build the compiler/image path and run CTest. |
| Diagnostics | `python tools/trit_tool.py export-diagnostics` | Record the current contract and build state. |

The supported graph is intentionally small: one CMake/CTest graph, one
conformance executable, and one Python command entry point. Historical test,
benchmark, wrapper-script, and migration trees are not part of the current
platform.

## Current gaps

See [`KNOWN_GAPS.md`](../KNOWN_GAPS.md) for unresolved work and
[`docs/12_Future_Architecture/platform_evolution_plan.md`](12_Future_Architecture/platform_evolution_plan.md)
for future hardware and product scope. TreatCode website material, Graphify,
and the Obsidian canvas remain supplemental and are not platform authority.
