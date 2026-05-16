# Literal Pooling & Constant Management

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_asm.h`, `ternary_ir.h` |

---

## 🎯 The Immediate Limit
The Trit-Stack instruction format provides exactly 16 trits for immediate values (`imm16`). This creates a challenge for the 50-trit architecture, which the compiler must solve through a three-tiered emission strategy.

---

## 🏗️ Emission Tiers

### Tier 1: Small Constants (1–16 trits)
Values between $-21,523,360$ and $+21,523,360$.
*   **Emission**: `MOV rd, imm`
*   **Hardware**: The 16-trit value is zero-padded to 50 trits.

### Tier 2: Medium Constants (17–32 trits)
Values up to $~1.8 \times 10^{15}$.
*   **Strategy**: Split the value into two 16-trit halves.
*   **Emission**:
    1.  `MOV  rd, low_half`
    2.  `MOVH rd, high_half` (High-Half is shifted 16 slots left and added).

### Tier 3: Large Constants (> 32 trits)
Full-precision 50-trit `LongTriple` values.
*   **Strategy**: **Literal Pooling**. The value is placed in the `.data` section, and the compiler emits a memory access.
*   **Emission**: `LOAD rd, r0, pool_label`

---

## 🏊 The Literal Pool Pattern
To save memory, the compiler should perform **Constant Deduplication**. If multiple functions use the same 50-trit constant (e.g., a mathematical constant like $\pi$ or a BitNet scale factor), only one instance is stored in the `.data` segment.

### Example Assembly Output:
```asm
.text
    ; Loading a 50-trit constant
    LOAD r1, r0, CONST_PI
    HALT

.data
CONST_PI:
    .word 314159265358979323846 ; (Encoded as 50-trit value)
```

---

## ⚡ Performance Impact
*   **Tier 1**: 1 Cycle.
*   **Tier 2**: 2 Cycles.
*   **Tier 3**: 1 Cycle + Memory Latency (Stall if cache misses).

> [!TIP]
> **Compiler Choice**: The compiler should always favor the lowest possible Tier. For BitNet weights (-1, 0, +1), Tier 1 is always used, ensuring maximum inference performance.
