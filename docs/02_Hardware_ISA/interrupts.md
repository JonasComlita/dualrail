# Traps, Exceptions & Interrupts

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_isa.h`, `ternary_vm.h` |

---

## 🛑 The Synchronous Trap Model
The Trit-Stack uses a synchronous trap system. When a hardware fault occurs, the processor immediately populates the **Trap Register (`r27`)** and halts execution.

### The Trap Record (`r27`)
Unlike general-purpose registers, `r27` has a fixed hardware layout for fault reporting:

| Trit Index | Field | Semantic |
| :--- | :--- | :--- |
| **trit[0]** | **Validity** | `0` = No Fault; `+1` = Fault Present |
| **trit[1]** | **Class** | The specific exception type (see below) |
| **trit[4:2]** | **Reserved** | Future context expansion |

---

## 📂 Fault Classes
The fault class (stored in `trit[1]`) identifies the reason for the halt.

| Value | Class Name | Triggering Condition |
| :--- | :--- | :--- |
| **-1** | `TRAP_DIV_ZERO` | Division by zero in a `DIV` instruction. |
| ** 0** | `TRAP_MEM_FAULT` | Accessing memory outside the legal bounds. |
| **+1** | `TRAP_ILLEGAL_OP` | Decoding an unknown opcode or an `INVALID` (0b11) trit. |

---

## 🛡️ Privilege Modes (OS3 Hook)
Hardware enforcement of privilege levels is managed through the **Address Sign Rule**.

*   **User Mode**: Can only access addresses where $Sign(Addr) \geq 0$.
*   **Kernel Mode**: Can access all addresses, including **Negative Memory**.
*   **Enforcement**: Any User-mode instruction that attempts to `LOAD/STORE` or `JMP` to a negative address triggers a hardware `TRAP_MEM_FAULT`.

---

## 📡 Asynchronous Interrupts (Planned)
The future FPGA implementation includes three hardware interrupt lines:

1.  **INT0 (Timer)**: Triggered by the hardware tick counter.
2.  **INT1 (IO)**: Triggered by the UART/Substrate interface.
3.  **NMI (Non-Maskable)**: Triggered by power-fail or critical hardware error.

> [!IMPORTANT]
> **Trap Stickiness**: Once a trap is set in `r27`, it remains until a hardware `RESET` or a privileged `RFI` (Return From Interrupt) instruction is executed. The VM will not advance the PC while a trap is active.
