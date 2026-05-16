# VM State & Cycle Semantics

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_vm_state.h`, `ternary_vm.h` |

---

## 🏗️ The VMState Container
The `VMState` struct maintains the architectural state of a Trit-Stack core.

### 1. General Purpose Registers (GPRs)
The machine features 27 general-purpose registers (`r0`..`r26`).
*   **Capacity**: Each register holds a 50-trit `LongTriple`.
*   **Hardwired Zero (`r0`)**: Physically tied to zero. Reads return `0`; writes are discarded.
*   **Link Register (r25)**: Updated automatically by the `CALL` instruction to store the return address (`PC + 1`).
*   **Stack Pointer (r26)**: Conventionally used for stack management; initialized to the end of DMEM.

> [!NOTE]
> **Bias-13 Encoding**: While the machine has 27 physical registers, the instruction fields use a Bias-13 encoding (range -13 to +13) to protect `r0` at the center of the range.

### 2. Control & Status Registers
*   **Program Counter (PC)**: A 50-trit address pointing to the next **27-trit instruction word** in IMEM.
*   **Trap Register (r27)**: A dedicated 5-trit register for fault reporting.
*   **Status**: A machine-state enum (`RUNNING`, `HALTED`, `TRAPPED`).

---

## 🔄 Execution Cycle
The VM executes a strict 4-phase cycle per instruction.

1.  **Fetch**: Load `IMEM[PC]`.
2.  **Decode**: Field extraction and `INVALID` (0b11) check.
3.  **Execute**: Opcode logic (ALU, Logic, Branch).
4.  **PC Advance**: The `PC` is incremented to `PC + 1` **only if** the instruction succeeds.

### Error Handling
If an instruction triggers a trap (e.g., `TRAP_MEM_FAULT`), the machine enters the `TRAPPED` state, populates `r27`, and **freezes the PC**. This allows debuggers to inspect the exact instruction that failed.

---

## 🧊 Machine Reset
A `reset()` operation restores the machine to a deterministic "power-on" state:
*   **PC = 0**: Restarts from the first instruction.
*   **SP (r26) = DMEM_SIZE - 1**: Initializes the stack to the top of data memory.
*   **Status = RUNNING**: Clears any previous halt/trap states.
*   **Registers = 0**: All registers (except SP) are set to zero.
