# Current stack review order

This is the structural map for agents working on the platform. Read the
contract first, then follow the producer-to-consumer path. A file belongs in
the current platform only when it is built, consumed, or named by one of these
surfaces.

## 1. Contract and generated snapshot

1. `ARCHITECTURE_MANIFEST.json`
2. `IMAGE_FORMAT_MANIFEST.json`
3. `SYSCALL_MANIFEST.json`
4. `architecture_contract.h`
5. `architecture_contract.trit`
6. `APP_MANIFEST.json`
7. `ROADMAP_STATUS.json`
8. `KNOWN_GAPS.md`

The current tuple is ISA v2, executable/function ABI v3, tBoot v3, tDisk v2,
syscall ABI v2, and vector ABI 1. Older executable/image versions are history,
not loadable compatibility targets.

## 2. Instruction and execution layers

1. `ternary_isa.h`
2. `ternary_vm_state.h`
3. `ternary_vm.h`
4. `ternary_asm.h`
5. `architecture_v2_assembler_types.h`
6. `architecture_v2_assembler_directives.h`
7. `v2_first_silicon_bringup.tasm`

The `v2` name here identifies the current ISA wire format. Do not infer an
executable ABI from it: executable metadata is always the v3 envelope.

## 3. Executable and compiler boundary

1. `executable_header_v3.h`
2. `ternary_compiler_types.h`
3. `ternary_compiler_ir.h`
4. `ternary_compiler_lexer.h`
5. `ternary_compiler_parser.h`
6. `ternary_compiler_codegen.h`
7. `ternary_compiler.h`
8. `tritc.cpp`

The compiler emits `.isa 2` plus `.execheader3`. The linker rejects any
function or executable ABI other than v3; no old header codec or migrator is
linked into the toolchain.

## 4. Kernel and native OS

1. `kernel.trit`
2. `native_kernel_boot.tasm`
3. `native_kernel_trap_stub.tasm`
4. `ternary_os.h`
5. `ternary_host_runtime.h`
6. `build_tos_image.cpp`
7. `run_tos_sdl.cpp`

The native loader validates only the v3 executable descriptor. The host image
writer emits only tBoot v3 and tDisk v2. Invalid or non-current artifacts are
rejected and must be rebuilt from current source.

## 5. Language and user space

1. `TCL_Spec_1.0.md`
2. `tcl_native_rewrite.md`
3. `tcl_token.trit` through `tcl_backend.trit`
4. `ulib_mini.trit`, `ulib.trit`, and SDK sources
5. `apps/`

Use `SYSCALL_MANIFEST.json` and `APP_MANIFEST.json` when changing the public
language/runtime or bundled applications. The website sources are consumers,
not alternate platform authority.

## 6. Build and validation

The supported tooling surface is deliberately consolidated:

```text
cmake -S . -B build_current_cleanup
cmake --build build_current_cleanup --target current_validate
cmake --build build_current_cleanup --target ci_production
python tools/trit_tool.py doctor
python tools/trit_tool.py contract-check
python tools/trit_tool.py export-diagnostics
```

`tests/current_only_conformance.cpp` is the current contract check. The old
`tests/`, `tests_next/`, benchmark, wrapper-script, and migration trees were
retired; do not restore them as parallel authorities. Add a focused current
check only when a contract change cannot be verified by the existing CMake
target.

## Organization rule

Keep source beside the layer it implements, keep one manifest per contract,
and keep derived snapshots clearly marked as generated or checked-in snapshots.
Progress and risk documents remain at the root. Supplemental website, Graphify,
and Obsidian material must not become hidden build or compatibility inputs.
