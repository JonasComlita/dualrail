# Instruction Pipeline (Architectural)

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_vm.h` |

---

## 🏎️ The 4-Stage Lifecycle
The Trit-Stack ISA is designed for a conceptual 4-stage pipeline. Every instruction (R/I/B type) must adhere to this lifecycle to ensure binary compatibility across emulators and physical hardware.

### 1. Fetch (IF)
*   **Action**: The 27-trit instruction word is loaded from `IMEM` at the current `PC` address.
*   **Hardware Contract**: The `PC` must be word-aligned. There is no such thing as a "half-instruction" fetch.

### 2. Decode (ID)
*   **Action**: Field extraction occurs.
*   **Validation**: The decoder checks for `INVALID` (0b11) trits.
*   **Register Read**: Registers `Rs1` and `Rs2` are read from the file.
*   **Bias Adjustment**: The Bias-13 register indices are converted to internal 0-26 physical addresses.

### 3. Execute (EX)
*   **Action**: The ALU performs the operation (Arithmetic, Logic, or Comparison).
*   **Branching**: For `B-Type` instructions, the target address (`PC + offset`) is calculated here.
*   **Memory**: For `LOAD/STORE`, the effective address (`Rs1 + imm16`) is calculated.

### 4. Writeback (WB)
*   **Action**: The result is written to `Rd`.
*   **Invariant**: If `Rd` is `r0`, the write is discarded.
*   **PC Update**: The `PC` increments to `PC + 1` (or jumps to the target address).

---

## ⚡ Harvard Parallelism
Because the Trit-Stack is a Harvard Architecture, **Fetch (Stage 1)** and **Memory Access (Stage 3)** use independent buses. A `LOAD` or `STORE` instruction does not stall the fetching of the next instruction, allowing for a sustained 1-instruction-per-cycle throughput in the steady state.

---

## 🌪️ Bypassing & Hazards
*   **Data Hazards**: Physical hardware is expected to implement **Result Forwarding**. In the software VM, this is implicit as instructions execute atomically.
*   **Control Hazards**: Branching is resolved in **Stage 3 (EX)**. Without a branch predictor, this results in a 2-cycle penalty. The current VM spec assumes **No Delay Slots**; the instruction following a branch is only fetched after the branch target is resolved.
