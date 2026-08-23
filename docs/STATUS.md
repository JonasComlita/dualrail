# Project Status Dashboard

> Synced from `ROADMAP_STATUS.json` and `KNOWN_GAPS.md` on 2026-08-22.
> Run `tools/trit-test.ps1 production` to verify the authoritative gate.

---

## Phase Status

| Phase | ID | Status | Evidence Suites |
|-------|----|--------|----------------|
| Core VM + ISA | `core-vm-isa` | ✅ **Verified** | test_multiwidth_vm, test_ternary_ir, test_ternary_lanes, test_native_ops, test_numeric_workloads |
| Compiler + Runtime | `compiler-runtime` | ✅ **Verified** | test_phase7_compiler, test_compiler_abi_contract, test_compiler_corpus_gate, test_executable_abi_v3, test_host_runtime |
| Kernel + VFS + Process | `kernel-vfs-process` | ✅ **Verified** | test_os_platform, test_phase_d_kernel, test_process_handoff, test_redo_wal_v2, test_vfs_rename, next_scheduler_acceptance |
| Desktop + Host Runtime | `desktop-host-runtime` | ✅ **Verified** | test_host_runtime, stage_tos_release, smoke_tos_release, runtime checkpoint/input-journal provenance |
| Agent-Operable Surface | `agent-operable-surface` | ✅ **Verified** | test_agent_tooling, source-manifest checks, app validation, replay adapters, knowledge tooling |
| System Benchmarks | `system-benchmarks` | ✅ **Verified** | system_benchmark_gate, system_release_gate, performance baseline, external asset/execution tests |
| Encrypted Host Volumes | `encrypted-host-volumes` | ⚠️ **In Progress** | TRITENC1 implementation and focused tests pass; independent-review findings remain open |
| Ternary Symbolic Encodings | `ternary-symbolic-encodings` | ✅ **Verified** | test_symbolic_encodings, test_tcl_asm, symbolic guest and diagnostic tests |

---

## Component-Level Status

### Layer 0–1: Logic & ISA
| Component | Status | Source of truth |
|-----------|--------|----------------|
| Trit encoding (2-bit-per-trit) | ✅ Stable | `ternary_backend.h`, `ternary_scalar.h` |
| Instruction word (TritWord27) | ✅ Stable | `ternary_isa.h` |
| 64-bit-equivalent scalar design goal (T40) | ✅ Stable | `ARCHITECTURE_MANIFEST.json`, generated `architecture_v2.md` |
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
| TCL native compiler subset (tcl_*.trit) | ✅ Verified subset | `tcl_asm.trit`, `tcl_parser.trit`, etc.; `tcl_frontend.trit` remains the native-pipeline facade |
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
| Block I/O (WAL, crash recovery) | ✅ Verified | `kernel/bio.trit`, `test_redo_wal_v2`, `test_vfs_rename`, `test_migration_v2` |
| Networking (socket/bind/connect) | ⚠️ Partial | `kernel/net.trit` |
| Crash recovery scenarios | ✅ Verified current scope | `test_redo_wal_v2`, `test_vfs_rename`, `test_production_hardening` |

### Layer 11: Apps
| Component | Status | Source of truth |
|-----------|--------|----------------|
| os_sdk.trit (app foundation) | ✅ Stable | `apps/os_sdk.trit` |
| libwidget.trit (GUI toolkit) | ✅ Stable | `apps/libwidget.trit` |
| desktop.trit (compositor + dock) | ✅ Stable | `apps/desktop.trit` |
| shell.trit (command interpreter) | ✅ Stable | `apps/shell.trit` |
| All 50+ bundled binaries | ✅ Stable | `APP_MANIFEST.json` |
| App golden output tests | ✅ Verified | `next_apps_golden_snapshots`, `test_native_apps`, `test_consumer_shell_productization` |

### Layer 12: Host Runtime
| Component | Status | Source of truth |
|-----------|--------|----------------|
| .tboot image builder | ✅ Stable | `build_tos_image.cpp` |
| SDL host runner | ✅ Stable | `run_tos_sdl.cpp` |
| Diagnostics export | ✅ Stable | `tools/trit_tool.py` |
| Image inspector | ✅ Stable | `tools/trit-inspect-image.ps1` |
| Deterministic replay | ✅ Verified current scope (schema adapters, file-backed bundles, process replay, differential comparison, checkpoint and input-journal provenance) | `ROADMAP_STATUS.json`, `tools/trit-replay.ps1` |
| Syscall trace capture | Schema-versioned capture | `ternary_host_runtime.h` |

---

## Open Items

`KNOWN_GAPS.md` records one current implementation/review gap:

1. Complete the independent review of the host-only `TRITENC1` encrypted-volume
   lifecycle. High-severity findings concerning nonce uniqueness,
   replay/rollback, plaintext crash cleanup, key-file races, provider policy,
   and fuzz bounds must be resolved before the feature can be considered
   production-ready. A format-affecting remediation requires a new envelope
   version.

External Freedoom and BitNet payloads remain provenance-locked in the ignored
local cache; acquiring that cache on a fresh machine is an operational
requirement, not an open implementation gap.
