# Project Glossary & Terminology

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_math.h`, `ternary_vm_state.h` |

---

## 🏗️ Core Units

*   **Trit**: The fundamental unit of Balanced Ternary. Can hold values **-1**, **0**, or **+1**.
*   **Tryte (T5)**: Exactly **5 trits**. This is the standard data unit for byte-aligned storage, as $3^5 = 243$ fits within a binary 8-bit byte (256 states). Defined in `ternary_math.h` as `struct T5`.
*   **Word**: The standard instruction width, exactly **27 trits** ($3^{27}$ states).
*   **Triple (T40)**: A 40-trit floating point format designed to fit into a 64-bit binary register ($3^{40} < 2^{64}$).
*   **LongTriple (T50)**: A 50-trit floating point format designed to fit into a 128-bit binary register ($3^{50} < 2^{128}$).

---

## ⚙️ Architectural Terms

*   **Substrate**: The "glue" layer that allows the Ternary VM to interface with a binary Host OS (Windows/Linux). Includes syscall passthroughs and memory mapping.
*   **Architecture Contract**: A formal definition of how two layers (e.g., ISA and VM) interact. This ensures that the FPGA and the C++ Emulator behave identically.
*   **TZR (Trit-Zone Rounding)**: A specialized ternary rounding mode used in the ALU to maintain numeric stability.
*   **Dual-Rail**: A hardware encoding where 2 bits represent 1 trit. Used for instruction decoding.
*   **Positional**: A math encoding where a trit's value depends on its power-of-3 position. Used for ALU calculations.

---

## 🛡️ Systems Terms

*   **OS3**: The 3rd-generation Ternary Operating System currently in design.
*   **Trap**: A hardware-initiated jump to the kernel vector table due to an error (Divide by Zero) or intent (`SYSCALL`).
*   **Fence**: A synchronization instruction that ensures all prior memory operations are completed before continuing.
*   **Kernel Mode**: The high-privilege state (Trit +1) where all instructions are legal.
*   **User Mode**: The restricted state (Trit 0) where code must use traps to access system services.
