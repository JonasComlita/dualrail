# VM Compliance Contract

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_vm.h`, `ternary_vm_state.h` |

---

## 📜 Compliance Requirements
For a software emulator to be a valid Trit-Stack implementation, it MUST adhere to the following architectural contracts.

### 1. Memory Architecture (Harvard)
The VM **must** maintain a physically separate Instruction Memory (IMEM) and Data Memory (DMEM).
*   **Isolation**: No instruction (e.g., `STORE`) can modify IMEM.
*   **Addressing**: Address `0` in IMEM is distinct from Address `0` in DMEM.
*   **Word Width**: IMEM words are 27-trit instructions; ordinary DMEM words
    and scalar registers are T40. T40 is the largest whole-trit word whose
    valid ternary state space satisfies `3^40 < 2^64` and is the architecture's
    ternary equivalent of a 64-bit computer. T50 is an explicit extended/wide
    pair, not the DMEM word width.

### 2. Register File Integrity
*   **Count**: 27 General Purpose Registers (`r0` to `r26`).
*   **r0 Constant**: `r0` is hardwired to Zero. Any attempt to write to `r0` must be silently discarded.
*   **r27 Trap**: The trap register must be outside the general file and inaccessible to standard arithmetic opcodes.

### 3. Arithmetic Determinism
Mathematical results must match the **Trit-Zone Rounding (TZR)** specification. 
*   **Rounding**: `DIV` and `SQRT` must never result in binary-style "tie-breaking."
*   **Overflow**: Any numeric overflow in `ADD`/`MUL` must return the `INVALID_DATA` sentinel.

### 4. Instruction Lifecycle
The fetch-decode-execute loop must follow these strict steps:
1.  **Fetch**: Load the 27-trit word from `IMEM[PC]`.
2.  **Verify**: Check every trit for the `0b11` (Invalid) pattern. If found, trap to `TRAP_ILLEGAL_OP`.
3.  **Execute**: Perform the operation.
4.  **Advance PC**: The PC must increment **only after** successful execution. On `HALT` or `TRAP`, the PC remains pointing at the current instruction for debugging.

---

## ⚡ Hardware Compliance Checklist
| Feature | Compliance Requirement |
| :--- | :--- |
| **Instruction Width** | Exactly 27 trits (54 binary bits). |
| **Register Bias** | Register fields must use the Bias-13 encoding. |
| **Trap Mechanism** | Writing to `r27` must set `VMStatus::TRAPPED`. |
| **Memory Bounds** | OOB access must trigger `TRAP_MEM_FAULT`. |

---

## 🧪 Implementation Reference
The **C++ Virtual Machine** in this repository (`ternary_vm.h`) is the **Golden Reference**. Any discrepancy between a hardware implementation (Verilog) and this VM is considered a bug in the hardware.
