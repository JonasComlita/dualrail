# Vector ABI Specification

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_vm_state.h` |

---

## 🚀 SIMD Calling Convention
The Vector ABI extends the standard scalar ABI to handle parallel ternary processing and high-precision accumulation.

### 1. Vector Register Roles
The 8 vector registers are assigned specific architectural roles:

| Register | Name | Role | Preservation |
| :--- | :--- | :--- | :--- |
| **v0** | `va0` | Argument 0 / Return Value | Caller-Saved |
| **v1 - v3** | `va1 - va3` | Arguments 1 - 3 | Caller-Saved |
| **v4 - v7** | `vt0 - vt3` | Temporary Vectors | Caller-Saved |

### 2. The Accumulator Protocol (`rA`)
The high-precision 50-trit accumulator is a global shared resource.
*   **Ownership**: The accumulator is **Caller-Saved**.
*   **Initialization**: Any function intending to use `VDOT` or `VMAC` for a fresh calculation must explicitly execute `ACLR` (Accumulator Clear) in its prologue.
*   **Persistence**: A function may return a "running sum" in `rA` if documented in the API signature.

---

## ⚙️ Control State Management

### Vector Length (`VLEN`)
The hardware `vector_length` setting affects all subsequent vector operations.
*   **Preservation**: `VLEN` is **Callee-Saved**.
*   **Contract**: If a function modifies the vector length to optimize a local loop, it must save the original length and restore it before the `RET` instruction.

### Lane Faults
The `VectorFaultState` is **Volatile**. 
*   Functions are not required to preserve or clear the lane fault bits of the caller. 
*   If a function completes successfully, the caller can assume the faults in the return registers are either zero or relevant to the new result.

---

## 📦 Data Alignment
*   **Memory Pointers**: When passing pointers to vector data (DMEM), the pointers should be word-aligned.
*   **Strides**: For matrix operations, the row-stride should ideally be a multiple of the hardware `VLEN` (default 27) to maximize throughput.

> [!WARNING]
> **Register Spilling**: Spilling a single vector register to the stack requires 27 (or `VLEN`) `STORE` instructions. High-performance code should be structured to avoid vector spills entirely through register-pressure analysis at the compiler level.
