# Trit-Stack: Full-Spectrum Balanced Ternary Computing

The Trit-Stack is a vertically integrated computing environment built from the ground up on **Balanced Ternary (-1, 0, +1)** logic. This project spans from the hardware logic gates to a high-level operating system (OS3) and specialized AI runtimes.

---

## 🏗️ The Vertical Architecture
This system is organized as a dependency stack. Each layer is independent but follows a strict "Architecture Contract" with the layers above and below.

### [Layer 0-1] The Physical & ISA Foundation
*   **Ternary Logic Gates**: Implementation of primitive balanced gates (TSEL, XSUM, CMP) using dual-rail binary encoding.
*   **Instruction Set (ISA)**: A fixed-width, 27-trit instruction word architecture.
    *   **Register File**: 27 general-purpose registers (`r0`–`r26`) plus a status/trap register (`r27`).
    *   **The 73 Opcodes**: Optimized for ternary-native operations like Trit-Zone Rounding (TZR) and symmetric comparisons.

### [Layer 2-3] The Virtual Execution Engine
*   **The VM Substrate**: A high-performance execution engine that emulates ternary hardware on binary systems. It uses a custom "T40/T50" representation to pack ternary data into 64/128-bit binary registers.
*   **Memory Model**: A sequentially consistent, trit-addressable memory space with architectural `FENCE` support.

### [Layer 4-5] Toolchain & ABI
*   **ABI (Application Binary Interface)**: Defines the "Contract" for software.
    *   **Calling Convention**: `r13-r18` for arguments, `r13` for return values, and `r25` as the Link Register.
    *   **Stack Model**: Downward-growing stack with 27-trit word alignment.
*   **Ternary IR & Assembler**: A two-pass assembler and a backend-agnostic Intermediate Representation used to bridge high-level code to ternary machine code.

### [Layer 6-7] Operating System (OS3) & Applications
*   **Kernel Substrate**: A privilege-aware kernel supporting **Kernel Mode** (+1) and **User Mode** (0).
*   **Trap Architecture**: Synchronous traps (syscalls, math errors) and asynchronous interrupts (timer, I/O) are handled via a unified vector table.
*   **BitNet Runtime**: A specialized inference engine for 1.58-bit Quantized Transformers, proving the efficiency of ternary for modern AI.

---

## 🚦 Current Project Status
| Layer | Component | Status | Notes |
| :--- | :--- | :--- | :--- |
| **Hardware** | Logic & ISA | ✅ Stable | ISA finalized in `ternary_isa.h` |
| **Engine** | Ternary VM | ✅ Stable | High-speed T40/T50 execution |
| **Toolchain** | Assembler / IR | ⚠️ Partial | Assembler functional; IR in refinement |
| **OS** | OS3 Kernel | ⏳ Draft | Privilege & Trap model defined |
| **Apps** | BitNet AI | ✅ Stable | Proof-of-concept inference passing |

---

## 🔍 Quick-Lookup References
*   **[Register Roles]**: `r13` (Return), `r25` (Link), `r26` (Stack).
*   **[Privilege]**: Mode +1 = Kernel, Mode 0 = User.
*   **[Word Size]**: 1 Word = 27 Trits.

---

## 🚀 The Vision
To demonstrate that **Balanced Ternary** is not a historical curiosity, but a modern architectural powerhouse for:
1.  **AI Efficiency**: Native 1.58-bit logic for transformer inference.
2.  **Arithmetic Symmetry**: Faster, cleaner math without "Sign Bits."
3.  **Secure Isolation**: Using ternary state bits for intrinsic memory protection.

---

The folders Tier 06 (The Language), Tier 07 (OS Substrate), and Tier 08 (Applications) are a mix of things that are partially implemented and things that are purely architectural at this stage.

Here is the current state of the implementation vs. the documentation plan:

Tier 06: The Language (In Development):
We have the Backend (ternary_backend.h) and the IR (ternary_ir.h), but we do not yet have a formal "Trit-Lang" parser or syntax guide. Documenting this tier now would be defining the specification for the future parser.
Tier 07: OS Substrate (Partially Built):
I see an OS3 folder in the repository. This suggests that the kernel and privilege mapping might already have a foundation. I need to check its contents to see how much of the "Substrate" is code vs. concept.
Tier 08: Applications & UX (Prototypes):
We have the BitNet Runtime and benchmarks, but the high-level "Shell" and "UI Framework" are likely just planned.
I'll check the OS3 directory now to see if we have a concrete kernel to document.

The OS3 folder confirms it: Tiers 07 and 08 are currently in the "Architecture & Specification" phase.

Tier 07 (OS Substrate): You have a very detailed OS_SUBSTRATE_IMPLEMENTATION_GUIDE.md and an os_fundamentals.md. These define how the kernel should work (privilege modes, security, etc.), but the actual kernel.cpp isn't in the root yet.
Tier 08 (Applications): You have design docs for curl.md, sqlite.md, and even an xv6.md (a ternary port of the classic educational OS). These are "Planned Programs" to prove the architecture.