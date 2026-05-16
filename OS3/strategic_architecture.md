# Strategic Architecture Decisions

**Status:** Living document — these are the high-leverage decisions that prevent painful rewrites later.

This document captures the "do the hard things early" philosophy. While `TERNARY_ARCHITECTURE_CONTRACTS.md` defines *what* the machine must do, this document explains *why* we make certain choices and what long-term payoffs they deliver.

### Core Strategic Principles
- Solve the painful problems while the system is still small.
- Prefer designs that scale naturally with ternary properties.
- Make the VM and tools "future-aware" even when the feature is not yet implemented.
- Prioritize binary compatibility and clean ABIs from day one.

### Strategic Foundations (Do These Early)

**1. Unified Trap Entry & Naked Exception Handler**  
All traps, interrupts, syscalls, and faults enter through a single path with a standardized 32-word trap frame.  
**Payoff**: Fork, context switching, signals, and debugging become trivial.

**2. Identity-Mapped MMU Bridge**  
Every memory access goes through a `translate()` hook from day one.  
**Payoff**: Virtual memory can be enabled later with minimal disruption.

**3. CSR-Based Privilege Shield**  
Strict enforcement of privilege modes and CSR access in the VM immediately.  
**Payoff**: Prevents user code from corrupting the kernel.

**4. Immutable Syscall ABI**  
Lock register-based syscall convention and ternary status returns now.  
**Payoff**: Binary compatibility across kernel versions.

**5. O(1) Scheduler Data Structures**  
Use linked-list run and sleep queues from the start.  
**Payoff**: Scheduler cost stays constant as process count grows.

**6. Dead-Man’s Timer (Asynchronous Preemption)**  
Timer fires reliably and is hard to mask.  
**Payoff**: Kernel can always regain control.

**7. Dynamic Width & Type-Aware Execution**  
Support for registers knowing their active width (T1, T10, T50, etc.).  
**Payoff**: Faster context switches and higher memory density.

**8. 9-Trit Sub-Word Packing & Ternary Alignment**  
Design around 27-trit words that naturally hold three 9-trit shorts.  
**Payoff**: Great code density and perfect page table / MMU alignment.

**9. Self-Describing Binaries**  
Every executable starts with a small fixed header (metadata, widths, stack hint).  
**Payoff**: Enables smarter loading and easier self-hosting.

### Priority Order
1. Unified Trap Entry + Trap Frame  
2. Identity-Mapped MMU Bridge  
3. CSR Privilege Shield  
4. Immutable Syscall ABI  
5. Dead-Man’s Timer  
6. Dynamic Width support in VM/IR  
7. O(1) scheduler structures  

### Current Implementation Gaps by Milestone

These are the concrete missing pieces needed to reach each major milestone. The strategic foundations above are designed specifically to make filling these gaps much easier.

#### 1. Minimal Shell
- STDIN: Mechanism for the VM to receive keyboard trits from the host.
- Command parsing: Assembly routines for tokenization.
- Program loading: Ability to locate and `CALL` binaries as sub-processes.

#### 2. Minimal OS (xv6-style)
- Timer Interrupts: Asynchronous hardware clock for preemption.
- MMU / Page Tables: Page table walker in the VM.
- Context Management: Reliable save/restore of all 27 registers.
- Process Table: Kernel data structures for PID, states, parent-child relationships.

#### 3. File Management
- Block Device Driver: Read/write fixed-size trit blocks.
- File System: Inodes, directories, allocation bitmaps.
- Disk Image: Host-side mapping of `disk.img` into VM memory.

#### 4. Desktop GUI
- Framebuffer: Dedicated memory region for pixel data.
- Display Pipeline: Host code to render framebuffer at 60 Hz.
- Input Queue: Mouse/keyboard events via CSRs.
- Font Engine: Ternary bitmaps for text rendering.

#### 5. A More Complete OS
- High-Level Compiler: LLVM or GCC backend.
- Standard Library (`libc`): `malloc`, `printf`, strings, etc.
- Dynamic Loader: Shared library support.
- Networking Stack: TCP/IP with ternary-aligned packets.
- Hardened User/Kernel Shielding: Strict trapping of unauthorized access.

#### Additional Hidden Requirements
- Debugger (GDB-equivalent): Pause, inspect, single-step compiled code.
- Build Toolchain: Ternary Makefile / build system.
- Entropy / TRNG: Hardware random number generator CSR.
- IPC: Pipes, shared memory, signals.

---

**Connection to Architecture Contracts**

This document complements `TERNARY_ARCHITECTURE_CONTRACTS.md`. The Contracts document is the *specification*. This document is the *strategic rationale and long-term vision*. Together they form a strong foundation that will scale cleanly to larger ternary architectures.

**Last Updated:** May 2026
