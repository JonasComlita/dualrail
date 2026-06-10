# File and Directory Index

Complete index of every file an agent needs to understand the codebase.
Sorted by architectural layer (bottom-up).

---

## Root Directory

```
/
├── ternary_isa.h              # ISA ROSETTA STONE — opcodes, formats, CSRs, traps
├── ternary_backend.h          # Layer 0: Device-safe trit/lane helpers (CUDA-compatible)
├── ternary_scalar.h           # Layer 2: TernaryScalar<N> — canonical positional type
├── ternary_uint128.h          # Layer 2: 128-bit unsigned integer (for T50)
├── ternary_math.h             # Layer 2: Type aliases (T10/T20/Triple/LongTriple) + bridge ops
├── ternary_native_ops.h       # Layer 3: Bridge-free arithmetic (included by ternary_math.h)
├── ternary_lanes.h            # Layer 4: TritLane<N> SIMD transport types
├── ternary_simd.h             # Layer 4: AVX2 batch ops for TritLane20
├── ternary_vm_state.h         # Layer 5: VMState — registers, memory, CSR, vector regs, MMU
├── ternary_vm.h               # Layer 6: Fetch-decode-execute dispatcher
├── ternary_asm.h              # Layer 7: Two-pass assembler
├── ternary_ir.h               # Layer 7: Low-level IR (assembler IR)
├── ternary_backend.h          # Layer 0: Backend helpers shared by CPU/GPU/FPGA
│
├── ternary_compiler.h         # Layer 8: Top-level compiler driver
├── ternary_compiler_ir.h      # Layer 8: SSA IR (Instr, BasicBlock, Function, Module)
├── ternary_compiler_ast.h     # Layer 8: AST node definitions
├── ternary_compiler_lexer.h   # Layer 8: Lexer (Token types, tokenize())
├── ternary_compiler_parser.h  # Layer 8: Parser (parse() → ModuleAst)
├── ternary_compiler_codegen.h # Layer 8: IR → assembly lowering, register allocator
├── ternary_compiler_types.h   # Layer 8: Type system (TypeKind, TypeRef, layouts)
├── tritc.cpp                  # Layer 8: Compiler CLI entry point
│
├── kernel.trit                # Layer 10: MAIN KERNEL — 5700+ lines, 57 syscalls
├── ternary_os.h               # Layer 10: Host-side OS interface definitions
│
├── TCL_Spec_1.0.md            # Language spec for TCL (Ternary C-Like)
├── tcl_lexer.trit             # Self-hosted TCL lexer
├── tcl_token.trit             # Self-hosted token definitions
├── tcl_ast.trit               # Self-hosted AST
├── tcl_parser.trit            # Self-hosted parser
├── tcl_type.trit              # Self-hosted type checker
├── tcl_ir.trit                # Self-hosted IR
├── tcl_infer.trit             # Self-hosted type inference
├── tcl_backend.trit           # Self-hosted code generator
├── tcl_asm.trit               # Self-hosted assembler
├── tcl_frontend.trit          # Self-hosted compiler front-end
├── tcl_pointer_dataflow.trit  # Self-hosted pointer/dataflow analysis
├── tcl_native_rewrite.md      # Notes on native/FFI rewrite approach
│
├── ulib.trit                  # Standard library (libc-equivalent, ~92KB)
├── ulib_hmap.trit             # Hash map implementation
├── ulib_c.trit                # C compat layer
├── ulib_mini.trit             # Minimal stdlib subset
├── math_utils.trit            # Extra math utilities
│
├── ternary_host_runtime.h     # Layer 12: Host-side VM runner, image loader
├── ternary_transformer_runtime.h # AI: BitNet 1.58-bit transformer inference
├── ternary_gpu_kernels.h      # GPU: CUDA/SYCL compute kernels
├── ternary_gpu_validation.h   # GPU: Kernel validation
├── ternary_device_allocators.h # GPU: Device memory allocators
├── ternary_montgomery.h       # Crypto: Montgomery multiplication
├── ternary_consumer_shell.h   # Shell: Consumer-facing shell productization
│
├── build_tos_image.cpp        # Image builder — compiles all apps, bundles rootfs
├── run_tos_sdl.cpp            # SDL runner — boots .tboot in a window
├── CMakeLists.txt             # Build system (CMake)
│
├── AGENTS.md                  # Agent operating guide (READ FIRST)
├── ROADMAP_STATUS.json        # Phase completion with evidence
├── TEST_MANIFEST.json         # Test suites and commands
├── SYSCALL_MANIFEST.json      # Syscall ABI (IDs, calling convention)
├── APP_MANIFEST.json          # Bundled app inventory
├── IMAGE_FORMAT_MANIFEST.json # .tboot / .tdisk wire format
├── KNOWN_GAPS.md              # Missing/partial work
├── ACCEPTANCE_CRITERIA.md     # Gates for claiming work complete
├── DEBUGGING.md               # Diagnostics and failure triage workflow
├── README.md                  # Project overview
```

---

## `kernel/` — Kernel Subsystems

```
kernel/
├── bio.trit        # Block I/O — WAL, disk read/write, crash recovery
├── hal.trit        # Hardware Abstraction Layer — timer, interrupt routing
├── net.trit        # Networking — socket, bind, connect, send, recv
├── process.trit    # Process management — PCB, scheduler, signals
└── vfs.trit        # Virtual Filesystem — inode, directory, file ops
```

---

## `apps/` — Bundled Applications

```
apps/
├── os_sdk.trit        # App SDK — syscall wrappers, heap, string, env
├── libwidget.trit     # Widget toolkit — Window, Button, Label, Canvas, ListBox
│
├── init.trit          # PID 1 — boot supervisor, starts desktop
├── desktop.trit       # Desktop environment — dock, compositor, app launcher
├── shell.trit         # Command shell (/bin/shell, /bin/sh)
├── terminal.trit      # Terminal emulator (GUI window around shell)
├── calculator.trit    # GUI calculator
├── paint.trit         # Bitmap paint application
├── text_editor.trit   # Text editor (/bin/text_editor, /bin/edit)
├── file_manager.trit  # File manager GUI (/bin/file_manager)
├── task_manager.trit  # Task manager (/bin/task_manager, /bin/top, /bin/tasks)
├── settings.trit      # System settings panel
├── about.trit         # About dialog
├── help.trit          # Help viewer
├── tcc.trit           # In-OS TCL compiler (invokable from shell)
│
├── bin_core.trit      # Core /bin utilities: cat, echo, pwd, cp, mv, rm, mkdir, etc.
├── ls.trit            # /bin/ls
├── ps.trit            # /bin/ps
├── kill.trit          # /bin/kill
├── clear.trit         # /bin/clear
├── date.trit          # /bin/date
├── sleep.trit         # /bin/sleep
├── mount.trit         # /bin/mount
├── fsck.trit          # /bin/fsck
├── sync.trit          # /bin/sync
├── reboot.trit        # /bin/reboot
├── shutdown.trit      # /bin/shutdown
├── login.trit         # /bin/login
├── passwd.trit        # /bin/passwd
├── service_stub.trit  # Daemon stubs (sessiond, window_server, logd, etc.)
└── window_probe.trit  # Window system diagnostic probe
```

---

## `tests/` — Test Files

```
tests/
├── test_isa_asm.cpp           # ISA encoding + assembler round-trip
├── test_multiwidth_vm_main.cpp # VM width correctness (T1–T50)
├── test_ternary_ir.cpp        # IR assembler and linker
├── test_ternary_lanes.cpp     # Lane type + SIMD correctness
├── test_native_ops.cpp        # Bridge-free arithmetic
├── test_numeric_workloads.cpp # Numeric regression suite
├── test_phase7_compiler.cpp   # Full compiler pipeline
├── test_tcl_asm.cpp           # TCL assembler
├── test_malloc_micro.cpp      # Allocator micro-contracts
├── test_layer1_hal.cpp        # HAL + CSR access
├── test_os_platform.cpp       # OS platform tests (process, VFS, IPC)
├── test_phase_d_kernel.cpp    # Kernel functionality
├── test_phase_c5_pred.cpp     # Predicate/branch tests
├── test_process_handoff.cpp   # Process spawn → exec → handoff
├── test_production_layers.cpp # Production health checks
├── test_production_hardening.cpp # Error injection, fault handling
├── test_host_runtime.cpp      # Host image load + boot
├── test_native_apps.cpp       # Bundled app compilation + run
├── test_vm_widths.cpp         # Multi-width VM scenarios
├── test_consumer_shell_productization.cpp # Shell product tests
├── test_benchmark.cpp         # Performance benchmarks
├── test_uint128.cpp           # UInt128 arithmetic
├── test_formats.cpp           # Image format validation
├── test_tiny_transformer_runtime.cpp # BitNet inference
├── test_no_bridge.cpp         # Validate no binary bridge used
│
├── test_backend_smoke.trit    # Backend smoke (in TCL)
├── test_compiler_frontend_smoke.trit # Frontend smoke
├── test_parser_smoke.trit     # Parser smoke
└── int128_compat.h            # Helper for 128-bit compat
```

---

## `tools/` — Agent/Build Tools

```
tools/
├── trit_tool.py              # Master tool (doctor, test, export-diagnostics, etc.)
├── trit-doctor.ps1           # Environment health check
├── trit-test.ps1             # Test runner (smoke / os / production)
├── trit-build-image.ps1      # Build .tboot release image
├── trit-inspect-image.ps1    # Inspect .tboot/.tdisk files
├── trit-run.ps1              # Run release image (SDL)
├── trit-export-diagnostics.ps1 # Export diagnostics bundle
├── trit-bench.ps1            # Run benchmarks
├── trit-replay.ps1           # Replay traces (placeholder)
└── trit-fuzz.ps1             # Fuzz harness (placeholder)
```

---

## `docs/` — Documentation

```
docs/
├── README.md                  # Navigation hub (you are here)
├── INDEX.md                   # This file
├── STATUS.md                  # Project status dashboard
│
├── 00_Quick_Ref/              # Cheat sheets
├── 01_Logic_Level/            # Gate and arithmetic primitives
├── 02_Hardware_ISA/           # Instruction encoding and ISA details
├── 03_Execution_Engine/       # VM state and dispatch
├── 04_Binary_Contract/        # ABI and assembler syntax
├── 05_Compiler_Infra/         # SSA IR and code generation
├── 06_Language/               # TCL language
├── 07_OS_Substrate/           # Kernel and syscalls
├── 08_Applications/           # App SDK and bundled apps
└── 09_Host_Runtime/           # Build system, image format, host tools
```