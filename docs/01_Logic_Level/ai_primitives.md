# AI Primitives (BitNet b1.58 Logic)

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_vm.h`, `ternary_isa.h` |

---

## 🧠 BitNet b1.58 Acceleration
The Trit-Stack provides hardware-level acceleration for 1.58-bit Large Language Models (LLMs). In this regime, weights and activations are represented by single trits $\in \{-1, 0, +1\}$.

### 1. The T1 Logic Product
Standard multiplication is computationally expensive. BitNet uses a specialized **T1 Product** (Ternary XNOR) that can be implemented with minimal gates.

**Logic Table (BitNet Kernel):**
*Ref: `t1Product` in `ternary_vm.h`*

| A \ B | `-1` | ` 0` | `+1` |
| :--- | :--- | :--- | :--- |
| **-1** | `+1` | ` 0` | `-1` |
| ** 0** | ` 0` | ` 0` | ` 0` |
| **+1** | `-1` | ` 0` | `+1` |

> [!NOTE]
> This product is used by the `VDOT` (Vector Dot Product) instruction to process multiple lanes in parallel.

---

## 🌊 Activation Logic (`VACT`)
In BitNet inference, a high-precision sum must be "activated" back into a single trit before being passed to the next transformer layer. The `VACT` instruction performs a **Sign-based Activation**.

**Function**: $f(x) = sign(x)$

| Accumulator Value (T50) | Output Trit (T1) |
| :--- | :--- |
| **Positive (>0)** | `+1` |
| **Zero (0)** | `0` |
| **Negative (<0)** | `-1` |

---

## 🔋 Accumulator Management
To avoid precision loss during the dot product of thousands of trits, the Trit-Stack uses a dedicated **Accumulator Register**.

### Instructions:
*   **`ACLR`**: Resets the accumulator to numeric zero.
*   **`VDOT`**: Multiplies two T1 vector registers and adds the result to the accumulator in a single step.
*   **`ASTORE`**: Moves the final sum from the accumulator into a standard `LongTriple` register.

---

## ⚡ Performance Comparison
By using ternary-native logic instead of binary emulation for BitNet:
1.  **Gate Depth**: The T1 Product is ~70% shallower than a 4-bit binary multiplier.
2.  **Memory Density**: 1.58-bit weights are packed 5-to-a-tryte, reducing model size by 60% compared to INT8.
3.  **No Shift/Add**: `VDOT` operations require zero bit-shifting, significantly reducing power consumption.
