# Ternary Arithmetic & Rounding (TZR)

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_native_ops.h`, `ternary_math.h` |

---

## 📐 Trit-Zone Rounding (TZR)
The most significant mathematical advantage of the Trit-Stack is **Trit-Zone Rounding**. Unlike binary systems which must handle the "midpoint tie" (0.5), balanced ternary arithmetic is naturally decisive.

### The No-Midpoint Principle
For any $N$-trit fractional part, there are $3^N$ possible states. Since $3^N$ is always odd, there is exactly one "Zero" state and an even number of non-zero states ($3^N - 1$). These non-zero states split perfectly into two halves:

1.  **Zone -1 (Down)**: $(3^N - 1) / 2$ states that are closer to the lower integer.
2.  **Zone 0 (Exact)**: Exactly 1 state ($0$).
3.  **Zone +1 (Up)**: $(3^N - 1) / 2$ states that are closer to the upper integer.

**Result**: TZR is the most numerically stable rounding mode possible in computer science, as it eliminates the need for "Round to Even" or other arbitrary tie-breaking logic.

---

## ➕ Integer Primitives (T1 / T5)
Integer arithmetic in the Trit-Stack uses **Modular Balanced Arithmetic**.

*   **ADD / SUB**: Performed with carry propagation. Overflow results in the `INVALID_DATA` sentinel (0xFF).
*   **MUL**: Standard balanced multiplication.
*   **DIV**: Integer division with TZR truncation (Round to Nearest).
*   **NEG / ABS**: Symmetry around zero means `NEG` is a simple bit-flip and never overflows (unlike binary `INT_MIN`).

---

## 🚀 Floating Point Primitives (T10 - T50)
The high-precision types (`Triple`, `LongTriple`) implement native ternary floating point logic.

### 1. Addition / Subtraction
*   **Alignment**: Mantissas are shifted to match exponents.
*   **Normalization**: The result is shifted until the most significant trit is non-zero, and the exponent is adjusted.
*   **Precision**: Uses "Guard Trits" (defined in `TernaryFloatFormat`) to prevent precision loss during alignment.

### 2. Multiplication
*   **Algorithm**: Mantissas are multiplied to produce a $2 \times MantissaTrits$ product.
*   **Rounding**: The product is rounded back to the target width using TZR.
*   **Exponents**: Simply summed ($E_{result} = E_a + E_b$).

### 3. Square Root (`SQRT`)
*   **Algorithm**: Uses a native ternary Newton-Raphson iteration.
*   **Performance**: Converges faster than binary equivalents due to the symmetric nature of the ternary search space.

---

## 🛡️ Exception Handling
Arithmetic errors do not crash the VM but instead produce **Sentinels**:

*   **Overflow**: $3^{max}$ (Returned on sum too large).
*   **Underflow**: Returned on result too small for the exponent range.
*   **Invalid**: Returned on math errors (e.g., `SQRT(-1)` or `DIV/0`).

> [!TIP]
> Use the `TCMP` instruction to check for these sentinels before performing critical jumps.
