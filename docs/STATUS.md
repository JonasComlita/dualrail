# Project Status Dashboard

> Last synced from: `ROADMAP_STATUS.json`. Run `tools/trit-test.ps1 production` to verify.

---

## Phase Status

| Phase | ID | Status | Evidence Suites |
|-------|----|--------|----------------|
| Core VM + ISA | `core-vm-isa` | ✅ **Verified** | test_multiwidth_vm, test_ternary_ir, test_ternary_lanes, test_native_ops, test_numeric_workloads |
| Compiler + Runtime | `compiler-runtime` | ✅ **Verified** | test_phase7_compiler, test_tcl_asm, test_malloc_micro, test_layer1_hal |
| Kernel + VFS + Process | `kernel-vfs-process` | ⚠️ **In Progress** | test_os_platform, test_phase_d_kernel, test_process_handoff, test_production_layers, test_production_hardening |
| Desktop + Host Runtime | `desktop-host-runtime` | ⚠️ **In Progress** | test_host_runtime, stage_tos_release, smoke_tos_release |
| Agent-Operable Surface | `agent-operable-surface` | ⚠️ **In Progress** | AGENTS.md, TEST_MANIFEST.json, trit_tool.py, test_agent_tooling, docs/trit-stack.canvas |
| System Benchmarks | `system-benchmarks` | 🧭 **Planned** | docs/10_Benchmarks/system_benchmark_plan.md, TEST_MANIFEST.json system_benchmarks |
| Ternary Symbolic Encodings | `ternary-symbolic-encodings` | 🧭 **Planned** | docs/04_Binary_Contract/symbolic_encodings.md |

---

## Component-Level Status

### Layer 0–1: Logic & ISA
| Component | Status | Source of truth |
|-----------|--------|----------------|
| Trit encoding (2-bit-per-trit) | ✅ Stable | `ternary_backend.h`, `ternary_scalar.h` |
| Instruction word (TritWord27) | ✅ Stable | `ternary_isa.h` |
| 80 opcodes (NOP–TSTR) | ✅ Stable | `ternary_isa.h` `Opcode` enum |
| CSR file (46 registers) | ✅ Stable | `ternary_isa.h` CSR_* constants |
| Privilege modes (Kernel/Supervisor/User) | ✅ Stable | `ternary_isa.h` `PrivilegeMode` |
| Trap codes + OS cause codes | ✅ Stable | `ternary_isa.h` |

### Layer 2–4: Math & SIMD
| Component | Status | Source of truth |
|-----------|--------|----------------|
| T1, T5 (integer) | ✅ Stable | `ternary_scalar.h`, `ternary_math.h` |
| T10, T20 (floating point) | ✅ Stable | `ternary_scalar.h` |
| Triple/T40 (native word) | ✅ Stable | `ternary_scalar.h` |
| LongTriple/T50 (wide) | ✅ Stable | `ternary_scalar.h` |
| Native arithmetic (bridge-free) | ✅ Stable | `ternary_native_ops.h` |
| Transcendental ops (exp, ln, sin, cos) | ✅ Stable | `ternary_native_ops.h` |
| TritLane types (L1–L50) | ✅ Stable | `ternary_lanes.h` |
| AVX2 batch ops (TritLane20) | ✅ Stable | `ternary_simd.h` |

### Layer 5–7: VM & Assembler
| Component | Status | Source of truth |
|-----------|--------|----------------|
| VMState (registers, memory, CSRs) | ✅ Stable | `ternary_vm_state.h` |
| VM dispatcher (all 80 opcodes) | ✅ Stable | `ternary_vm.h` |
| Vector register file (8 regs × N lanes) | ✅ Stable | `ternary_vm_state.h` |
| AI accumulator | ✅ Stable | `ternary_vm_state.h` |
| MMU / address translation | ✅ Stable | `ternary_vm_state.h` |
| Two-pass assembler | ✅ Stable | `ternary_asm.h` |
| GPU kernels (CUDA/SYCL) | ⚠️ Partial | `ternary_gpu_kernels.h` |

### Layer 8–9: Compiler & Language
| Component | Status | Source of truth |
|-----------|--------|----------------|
| SSA IR (BasicBlock, Function, Module) | ✅ Stable | `ternary_compiler_ir.h` |
| Type system (primitives, structs, arrays) | ✅ Stable | `ternary_compiler_types.h` |
| Lexer | ✅ Stable | `ternary_compiler_lexer.h` |
| Parser | ✅ Stable | `ternary_compiler_parser.h` |
| IR lowering / codegen | ✅ Stable | `ternary_compiler_codegen.h` |
| Register allocator | ✅ Stable | `ternary_compiler_ir.h` `AllocationResult` |
| Optimizer (mem2reg, CSE, const-fold) | ✅ Stable | `ternary_compiler_ir.h` `OptimizerStats` |
| TCL self-hosted compiler (tcl_*.trit) | ✅ Stable | `tcl_asm.trit`, `tcl_parser.trit`, etc. |
| TCL language spec | ✅ Documented | `TCL_Spec_1.0.md` |

### Layer 10: OS Kernel
| Component | Status | Source of truth |
|-----------|--------|----------------|
| Syscall dispatch (57 services) | ✅ Stable | `kernel.trit`, `SYSCALL_MANIFEST.json` |
| Process lifecycle (fork/exec/exit/wait) | ✅ Stable | `kernel.trit`, `kernel/process.trit` |
| VFS + persistent root image | ✅ Stable | `kernel.trit`, `kernel/vfs.trit` |
| Window manager (create/move/event) | ✅ Stable | `kernel.trit` |
| IPC (send/recv/blocking) | ✅ Stable | `kernel.trit` |
| Framebuffer / GPU CSR protocol | ✅ Stable | `ternary_isa.h`, `ternary_os.h` |
| Block I/O (WAL, crash recovery) | ⚠️ Partial | `kernel/bio.trit` |
| Networking (socket/bind/connect) | ⚠️ Partial | `kernel/net.trit` |
| Crash recovery scenarios | ❌ Gap | `KNOWN_GAPS.md` |

### Layer 11: Apps
| Component | Status | Source of truth |
|-----------|--------|----------------|
| os_sdk.trit (app foundation) | ✅ Stable | `apps/os_sdk.trit` |
| libwidget.trit (GUI toolkit) | ✅ Stable | `apps/libwidget.trit` |
| desktop.trit (compositor + dock) | ✅ Stable | `apps/desktop.trit` |
| shell.trit (command interpreter) | ✅ Stable | `apps/shell.trit` |
| All 50+ bundled binaries | ✅ Stable | `APP_MANIFEST.json` |
| App golden output tests | ❌ Gap | `KNOWN_GAPS.md` |

### Layer 12: Host Runtime
| Component | Status | Source of truth |
|-----------|--------|----------------|
| .tboot image builder | ✅ Stable | `build_tos_image.cpp` |
| SDL host runner | ✅ Stable | `run_tos_sdl.cpp` |
| Diagnostics export | ✅ Stable | `tools/trit_tool.py` |
| Image inspector | ✅ Stable | `tools/trit-inspect-image.ps1` |
| Deterministic replay | Partial (checkpoint/input rewind in runtime; no file-backed restore) | `KNOWN_GAPS.md` |
| Syscall trace capture | Schema-versioned capture | `ternary_host_runtime.h` |

---

## Open Items (highest priority)

See `KNOWN_GAPS.md` for the full list. Top items:

1. File-backed checkpoint restore and separate-process execution replay
2. Guest `/bin/doctor`, `/bin/test`, `/bin/sysinfo`
3. Fuzz harnesses for malformed images + bad syscall pointers
4. Crash/power-loss VFS + WAL recovery scenarios
5. Framebuffer PNG export
6. App golden output/snapshot tests
7. Doom-class and BitNet 1.58B-class OS benchmark harnesses
8. Ternary-native symbolic encodings for text and compact value dumps
