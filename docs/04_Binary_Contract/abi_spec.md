# ABI Specification (Trit-Stack)

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_isa.h`, `ternary_vm_state.h` |

---

## 🎭 Register Conventions
The Trit-Stack ABI categorizes the 27 general-purpose registers to ensure predictable function interoperation.

| Register | Name | Role | Preservation |
| :--- | :--- | :--- | :--- |
| **r0** | `zero` | Hardwired Zero | Constant |
| **r1 - r4** | `a0 - a3` | Arguments / Return Values | Caller-Saved |
| **r5 - r12** | `s0 - s7` | Static (Local) Variables | **Callee-Saved** |
| **r13 - r24**| `t0 - t11`| Temporary Variables | Caller-Saved |
| **r25** | `lr` | Link Register (Return Address) | Caller-Saved |
| **r26** | `sp` | Stack Pointer | **Callee-Saved** |

---

## 📚 The Stack Model

### Growth & Alignment
*   **Direction**: The stack grows **downward** (toward address 0).
*   **Invariance**: `sp` always points to the **next free word** in memory.
*   **Alignment**: All stack operations are word-aligned (50 trits).

### Standard Prologue / Epilogue
Every function that calls another function (a "non-leaf" function) must follow this pattern:

**Prologue**:
```asm
STORE sp, lr, 0   ; Save Link Register
ADD sp, sp, -1    ; Decrement SP
; (Optionally save s0-s7 if used)
```

**Epilogue**:
```asm
; (Optionally restore s0-s7)
ADD sp, sp, 1     ; Increment SP
LOAD lr, sp, 0    ; Restore Link Register
RET               ; Jump back to LR
```

---

## 📩 Parameter Passing
1.  **Small Data**: The first 4 ternary values (up to 50 trits each) are passed in `r1` through `r4`.
2.  **Overflow**: Parameters 5 and above are passed on the stack.
3.  **Return Values**: The primary result is returned in `r1`. Secondary results (e.g., error codes or large structs) use `r2-r4`.

---

## ⚡ Optimization: Register Shuffling
When preparing a function call, the compiler frequently needs to move values into the argument registers (`r1-r4`).
*   **The SWAP Rule**: Instead of using a sequence of `COPY` instructions, the compiler should use the native **`SWAP`** opcode to move values into the `a0-a3` slots. This saves cycles and avoids temporary register pressure.

---

## 🛡️ Vector ABI
*   **v0 - v7**: All vector registers are considered **Caller-Saved**. 
*   **Accumulator (rA)**: The accumulator is **Caller-Saved**. It must be cleared (`ACLR`) at the start of a function if used for fresh accumulation.

> [!IMPORTANT]
> **Stack Underflow**: The hardware does not automatically trap on stack underflow (crossing `SP` above the initial reset value). This must be managed by the OS or compiler-inserted checks.
