# Trit-Stack Documentation Hub

> **For agents:** Start here. Read this file, then read the section that matches your task.
> The source-of-truth JSON manifests (`TEST_MANIFEST.json`, `SYSCALL_MANIFEST.json`, etc.) always override docs when there is a conflict.

---

## What This Is

A **vertically integrated, balanced-ternary computing stack** built entirely from scratch — from logic gates through virtual machine, compiler, operating system, and GUI desktop. All arithmetic is in base-3 ({-1, 0, +1}); every layer reflects this.

The project namespace is `sandbox::` in C++, and `trit` in file extensions (`.trit` source files, `.tboot` images, `.tdisk` disks).

For a dependency-ordered, file-by-file review and optimization plan, use
[`STACK_REVIEW_ORDER.md`](STACK_REVIEW_ORDER.md).

---

## Layer Map (Bottom to Top)

| Layer | What | Files |
|-------|------|-------|
| **0 — Logic** | Trit encoding, gate primitives, backend helpers | `ternary_backend.h` |
| **1 — ISA** | 80 opcodes, 3 instruction formats, 27 GPRs, CSRs | `ternary_isa.h` |
| **2 — Math types** | T1/T5 integers, T10/T20/T40/T50 floats, UInt128 | `ternary_scalar.h`, `ternary_math.h`, `ternary_uint128.h` |
| **3 — Native ops** | Bridge-free ternary arithmetic (add/mul/div/sqrt/exp/ln) | `ternary_native_ops.h` |
| **4 — Lane/SIMD** | 2-bit-per-trit packed lane types + AVX2 batch ops | `ternary_lanes.h`, `ternary_simd.h` |
| **5 — VM State** | Register file, memory, CSR state, vector registers | `ternary_vm_state.h` |
| **6 — VM Dispatcher** | Fetch-decode-execute loop, all 80 opcodes | `ternary_vm.h` |
| **7 — Assembler** | Two-pass assembler, symbol table, instruction encoding | `ternary_asm.h` |
| **8 — Compiler IR** | SSA IR, type system, optimizer, register allocator | `ternary_compiler_*.h`, `ternary_ir.h` |
| **9 — TCL language** | Ternary C-Like language (source files: `.trit`) | `TCL_Spec_1.0.md`, `tritc.cpp`, `tcl_*.trit` |
| **10 — OS kernel** | Process, VFS, IPC, window manager, 57 syscalls | `kernel.trit`, `kernel/`, `ternary_os.h` |
| **11 — Apps/SDK** | GUI apps, shell, SDK library, widget toolkit | `apps/`, `apps/os_sdk.trit`, `apps/libwidget.trit` |
| **12 — Host runtime** | Image loader, SDL runner, boot image builder | `ternary_host_runtime.h`, `build_tos_image.cpp`, `run_tos_sdl.cpp` |
| **AI** | BitNet 1.58-bit transformer inference engine | `ternary_transformer_runtime.h` |
| **Benchmarks** | Long-horizon Doom-class and BitNet-class OS capability tests | `docs/10_Benchmarks/` |

---

## Section Index

### Quick Reference (for agent loops)
- **[00_Quick_Ref/opcode_table.md](00_Quick_Ref/opcode_table.md)** — All 80 opcodes
- **[00_Quick_Ref/register_map.md](00_Quick_Ref/register_map.md)** — r0–r27, ABI roles
- **[00_Quick_Ref/trit_encoding.md](00_Quick_Ref/trit_encoding.md)** — Encoding schemas
- **[00_Quick_Ref/glossary.md](00_Quick_Ref/glossary.md)** — Terminology
- **[00_Quick_Ref/common_patterns.md](00_Quick_Ref/common_patterns.md)** — Assembly cookbook
- **[00_Quick_Ref/conversion_table.md](00_Quick_Ref/conversion_table.md)** — Decimal ↔ stored hex

### Logic & Gates
- **[01_Logic_Level/gates.md](01_Logic_Level/gates.md)** — Ternary gate primitives
- **[01_Logic_Level/arithmetic.md](01_Logic_Level/arithmetic.md)** — Math primitives

### ISA & Hardware
- **[02_Hardware_ISA/encoding.md](02_Hardware_ISA/encoding.md)** — Instruction word format
- **[02_Hardware_ISA/interrupts.md](02_Hardware_ISA/interrupts.md)** — Traps, CSRs, privilege

### Execution Engine
- **[03_Execution_Engine/decoded_trace_and_native_jit.md](03_Execution_Engine/decoded_trace_and_native_jit.md)** — portable micro-ops, cache guards, and gated W^X x86-64 JIT
- **[03_Execution_Engine/vm_state.md](03_Execution_Engine/vm_state.md)** — VMState structure
- **[03_Execution_Engine/memory_model.md](03_Execution_Engine/memory_model.md)** — DMEM, FENCE, addressing
- **[03_Execution_Engine/vector_engine.md](03_Execution_Engine/vector_engine.md)** — Vector registers, VDOT

### Binary / ABI Contract
- **[04_Binary_Contract/abi_spec.md](04_Binary_Contract/abi_spec.md)** — Calling convention
- **[04_Binary_Contract/asm_syntax.md](04_Binary_Contract/asm_syntax.md)** — Assembler syntax
- **[04_Binary_Contract/symbolic_encodings.md](04_Binary_Contract/symbolic_encodings.md)** - ASCII, UTF-8, hex, and planned ternary-native symbolic encodings

### Compiler Infrastructure
- **[05_Compiler_Infra/ternary_ir.md](05_Compiler_Infra/ternary_ir.md)** — SSA IR nodes
- **[05_Compiler_Infra/codegen.md](05_Compiler_Infra/codegen.md)** — IR → assembly pipeline

### Language, OS, Apps
- **[06_Language/tcl_language.md](06_Language/tcl_language.md)** — TCL syntax and features
- **[07_OS_Substrate/kernel_overview.md](07_OS_Substrate/kernel_overview.md)** — Kernel architecture
- **[07_OS_Substrate/syscall_table.md](07_OS_Substrate/syscall_table.md)** — All 57 syscalls
- **[08_Applications/app_sdk.md](08_Applications/app_sdk.md)** — App SDK and widget toolkit
- **[08_Applications/bundled_apps.md](08_Applications/bundled_apps.md)** — App inventory

### Host & Build
- **[09_Host_Runtime/image_format.md](09_Host_Runtime/image_format.md)** — .tboot/.tdisk formats
- **[09_Host_Runtime/build_and_test.md](09_Host_Runtime/build_and_test.md)** — Build system and test runner

### System Benchmarks
- **[10_Benchmarks/system_benchmark_plan.md](10_Benchmarks/system_benchmark_plan.md)** - Doom-class and BitNet-class benchmark plan
- **[10_Benchmarks/doom.md](10_Benchmarks/doom.md)** - Interactive OS benchmark target
- **[10_Benchmarks/bitnet.md](10_Benchmarks/bitnet.md)** - Inference and data-movement benchmark target

---

## Critical Conventions

| Thing | Value |
|-------|-------|
| Trit values | -1 (T_NEG), 0 (T_ZER), +1 (T_POS) |
| Trit encoding in wire format | `0b00`=−1, `0b01`=0, `0b10`=+1, `0b11`=INVALID |
| Instruction word | 27 trits packed as 54 bits in `uint64_t` |
| Native word size | **T40** = 40-trit float in `uint64_t` |
| General registers | r0 (zero) … r26, r27 (trap, read-only) |
| Link register | **r25** |
| Stack pointer | **r26** (grows downward) |
| Return value | **r13** |
| Args | r13–r16 |
| Privilege modes | Kernel=T_NEG, Supervisor=T_ZER, User=T_POS |
| Source extension | `.trit` |
| Boot image | `.tboot` (magic `0x31544f4f424f5354`) |
| Disk image | `.tdisk` (magic `0x54524954535031`) |

---

## Source-of-Truth Files (agent must check before editing)

| File | Authority over |
|------|---------------|
| `ternary_isa.h` | Opcode values, field positions, trap codes, CSR IDs |
| `SYSCALL_MANIFEST.json` | Syscall IDs and calling convention |
| `APP_MANIFEST.json` | Guest binary paths and stack sizes |
| `IMAGE_FORMAT_MANIFEST.json` | .tboot/.tdisk wire format |
| `TEST_MANIFEST.json` | Test suites and commands |
| `ROADMAP_STATUS.json` | Phase completion status |
| `KNOWN_GAPS.md` | Missing/partial work |

---

## Project Status Summary

| Phase | Status |
|-------|--------|
| Core VM + ISA | ✅ Verified |
| Compiler + runtime | ✅ Verified |
| Kernel + VFS + process | ⚠️ In progress |
| Desktop + host runtime | ⚠️ In progress |
| Agent-operable surface | 🌱 Seeded |
| System benchmarks | Planned |

See `ROADMAP_STATUS.json` for evidence and open items. See `KNOWN_GAPS.md` for actionable work.
