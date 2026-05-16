# Vector & AI Engine

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_vm_state.h`, `ternary_vm.h` |

---

## 🚀 SIMD Architecture
The Trit-Stack features a native vector engine optimized for parallel ternary processing.

### 1. Vector Register File
*   **Count**: 8 vector registers (v0–v7).
*   **Lanes**: The default vector length (`VL`) is **27 trits**.
*   **Storage**: Each lane in a vector register is a `TernaryValue` (positional base-3).

### 2. Lane Width Selection
Vector instructions use the same width-prefixing as scalar instructions (e.g., `.l1`, `.l5`, `.l40`).
*   **L1 Mode**: Optimized for BitNet weight/activation pairs.
*   **L5 Mode**: Optimized for signal processing and 8-bit equivalent tryte-math.
*   **Logical Consistency**: Regardless of the width, each lane is backed by a full `TernaryValue` container.

---

## 🔋 The AI Accumulator (`rA`)
The `rA` register is the destination for high-throughput AI kernels.

*   **Format**: Full 50-trit `LongTriple`.
*   **The VDOT/VACT Cycle**:
    1.  `VDOT.l1 v1, v2`: Parallel multiply of lanes in `v1`, `v2`, summed into `rA`.
    2.  `VACT r1, rA`: Applies the ternary activation function to `rA` and moves the result to scalar `r1`.
*   **Precision**: With 50 trits of headroom, the machine can accumulate $3^{50}$ individual trit products without a single bit of overflow error.

---

## 🛡️ Vector Fault Tracking
The Trit-Stack implements **Per-Lane Fault Tracking**. If a vector instruction fails (e.g., a division by zero in lane 7), the VM does not just trap; it records the fault in the `VectorFaultState`.

*   **`vector_faults.fault_valid[i]`**: Set to `1` if lane `i` failed.
*   **`vector_faults.fault_class[i]`**: Stores the specific `TrapCode` for that lane.

> [!TIP]
> This allows high-reliability OS kernels to "mask out" faulty hardware lanes or handle software exceptions on a per-element basis without stalling the entire vector pipeline.
