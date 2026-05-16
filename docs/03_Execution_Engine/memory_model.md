# Memory Model & Consistency

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_vm_state.h` |

---

## 🏛️ Harvard Architecture
The Trit-Stack strictly separates instruction execution from data manipulation.

*   **Instruction Memory (IMEM)**: A dedicated 4096-word space for **27-trit** `TritWord27` instructions.
*   **Data Memory (DMEM)**: A 65,536-word space for **50-trit** `LongTriple` data.
*   **Isolation**: It is impossible for a running program to modify its own code or execute data as code.

---

## 📦 Word-Addressing
The memory model is entirely **Word-Addressed**.

*   **PC Increment**: The `PC` increments by **exactly 1** to reach the next instruction. There is no byte-offset math.
*   **No Bytes**: There is no "byte" or "half-word" addressing.
*   **Alignment**: Every memory access is naturally aligned; a `LOAD` always retrieves exactly one 50-trit word.
*   **Address Range**: Addresses are signed 50-trit values.
    *   `0 .. SIZE-1`: Standard User-mode memory.
    *   Negative Addresses: Reserved for Kernel-mode mapping.

---

## ⛓️ Sequential Consistency
The VM implements **Sequential Consistency (SC)**. 

1.  **Strict Ordering**: Instructions are executed one-by-one in the order they appear in the instruction stream.
2.  **No Reordering**: The VM does not perform out-of-order execution or speculative memory access.
3.  **FENCE**: The `FENCE` instruction acts as a global memory barrier. In the current single-threaded implementation, it is a `NOP`, but it serves as an architectural marker for future multi-core synchronization.

---

## 🛡️ Fault Handling
Memory access is strictly bounds-checked.
*   **Out-of-Range**: Accessing an address outside the allocated memory triggers `TRAP_MEM_FAULT`.
*   **Privilege Fault**: Accessing a negative address from a non-privileged context triggers `TRAP_MEM_FAULT`.
