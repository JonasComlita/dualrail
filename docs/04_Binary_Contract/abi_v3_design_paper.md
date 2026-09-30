# Executable and function ABI v3

**Status:** current architecture rationale
**Updated:** 2026-09-29

This page explains why the platform has one coordinated current boundary. The
root manifests and implementation files are authoritative.

## Current version axes

| Layer | Current contract |
| --- | --- |
| Instruction encoding | ISA v2 |
| Executable envelope | 20-word executable header ABI v3 |
| Compiler function boundary | `trit.compiler.function-abi.v3` |
| Syscalls | Syscall ABI v2 |
| Vectors | Vector ABI 1, VLEN 27, eight registers |
| Boot image | tBoot v3 |
| Disk image | tDisk v2 |

The axes are coordinated but intentionally separate. ISA v2 is the instruction
wire format; it does not mean that executable metadata is ABI v2. The executable
header records the ISA, function ABI, syscall ABI, feature bits, exact text/data
geometry, and the fixed vector context fields together.

## Why ABI v3 is the only executable boundary

The v3 header makes aggregate returns explicit with a caller-owned `sret`
pointer, declares vector geometry before execution, and gives the kernel an
exact 279-word process-owned vector context. That removes hidden conventions
between compiler, linker, loader, and scheduler.

The compiler emits `.isa 2` and `.execheader3`. The assembler, VM, native OS,
and image builder validate the same tuple. A non-current executable header is a
hard rejection: it is not guessed, decoded as another layout, or rewritten by
the runtime.

## Image boundary

tBoot v3 carries the ISA-v2 program and v3 executable metadata. tDisk v2 is the
only current persistent block format. The host runtime rejects unknown or
corrupt images and reports that the source must be rebuilt. There is no offline
migration executable in the supported tree.

## Evidence and ownership

The current contract is checked by:

- `ARCHITECTURE_MANIFEST.json` and `IMAGE_FORMAT_MANIFEST.json`;
- `executable_header_v3.h`, `ternary_compiler.h`, `ternary_vm_state.h`,
  `ternary_os.h`, and `ternary_host_runtime.h`;
- `tests/current_only_conformance.cpp`;
- `cmake --build build_current_cleanup --target ci_production`.

Historical v1/v2 design material explains the reasons for the clean cutover,
but it is not a compatibility promise and must not be used to restore an old
loader, benchmark, or migration path.
