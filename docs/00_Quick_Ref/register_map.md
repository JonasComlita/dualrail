# Register & ABI Quick Reference

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_vm_state.h`, `ternary_isa.h` |

---

## 🏗️ Register File Architecture
The Trit-Stack uses a **27-register general-purpose file** plus a dedicated hardware status register (`r27`). Every register is a `TernaryValue` which can store up to 50 trits of precision.

### Register Map (r0–r27)

| Register | Mnemonic | Primary Role | Preservation |
| :--- | :--- | :--- | :--- |
| **r0** | `ZERO` | **Hardwired Zero**: Reads always return 0; writes are ignored. | - |
| **r1 – r12** | `s0 – s11` | **Saved Temporaries**: Used for long-lived variables. | **Callee-Saved** |
| **r13** | `v0 / a0` | **Return Value / Argument 0**: First param and result. | Caller-Saved |
| **r14 – r18** | `a1 – a5` | **Arguments 1-5**: Function parameters. | Caller-Saved |
| **r19 – r24** | `t0 – t5` | **Temporaries**: Volatile scratch space. | Caller-Saved |
| **r25** | `ra` | **Link Register**: Stores return address for `CALL`. | Caller-Saved |
| **r26** | `sp` | **Stack Pointer**: Points to the current top of stack. | **Callee-Saved** |
| **r27** | `st` | **Trap / Status**: Dedicated fault/privilege record. | VM-Managed |

---

## ⚡ The Status Register (r27)
Unlike general registers, `r27` follows a strict **T5 (5-trit) Fault Record** format.

| Trit Index | Field | Meaning |
| :--- | :--- | :--- |
| **0** | `fault_valid` | `0` = No Trap, `+1` = Trap Active. |
| **1** | `fault_class` | `-1` = Div-by-Zero, `0` = Mem Fault, `+1` = Illegal Op. |
| **2** | `privilege` | `0` = User Mode, `+1` = Kernel Mode. |
| **3-4** | `reserved` | Reserved for future interrupt masking. |

---

## 📚 Calling Convention (The Contract)

### 1. Stack Anatomy
The stack grows **downward** from high memory to low memory. The Stack Pointer (`r26`) always points to the last *used* word.

```text
Higher Addresses
+-----------------------+
|  Previous Stack Frame |
+-----------------------+ <--- Old SP
|  Saved Link Reg (ra)  |
+-----------------------+
|  Saved s0 - s11       | (Only if used by callee)
+-----------------------+
|  Local Variables      |
+-----------------------+
|  Overflow Arguments   | (Args 6+ if applicable)
+-----------------------+ <--- Current SP
Lower Addresses
```

### 2. Parameter Passing
*   **Registers**: First 6 arguments go in `r13, r14, r15, r16, r17, r18`.
*   **Stack**: Remaining arguments are pushed onto the stack in reverse order (Right-to-Left).
*   **Return**: The result is always returned in `r13`.

### 3. Word Alignment
All instruction-related pointers (PC, Link Register) and the Stack Pointer **must** be aligned to 27-trit word boundaries. Misalignment triggers a `TRAP_MEM_FAULT`.

---

## 🛡️ Privilege Model
The Trit-Stack implements hardware-level separation:
*   **Kernel Mode (Trit +1)**: Can execute privileged instructions (`HALT`, `RFE`, direct I/O) and modify the Trap Vector Table.
*   **User Mode (Trit 0)**: Restricted. Any attempt to touch kernel memory or execute privileged ops triggers an `ILLEGAL_OP` trap, shifting the CPU to Kernel Mode.

