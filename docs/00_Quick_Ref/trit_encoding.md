# Trit Encoding & Representation

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_math.h`, `ternary_isa.h`, `ternary_scalar.h` |

---

## ⚡ The Two Faces of Trit Representation
The Trit-Stack uses two distinct encodings depending on whether the system is **Decoding** an instruction or **Calculating** a result.

### 1. Dual-Rail Encoding (Instruction / Hardware Level)
Used by `TritWord27` and the **Virtual Machine's ISA decoder**. Each trit occupies **2 binary bits**. This is designed to simulate simple bit-masking and shift operations that will eventually be implemented on FPGA/ASIC.

| Bit Pattern | Trit Value | Semantic Meaning |
| :--- | :--- | :--- |
| `0b00` | `-1` | Negative (T_NEG) |
| `0b01` | ` 0` | Neutral (T_ZER) |
| `0b10` | `+1` | Positive (T_POS) |
| `0b11` | `N/A` | **INVALID / MALFORMED** |

> [!IMPORTANT]
> Any `0b11` pattern encountered during instruction fetch or register-to-register moves triggers a `TRAP_ILLEGAL_OP` and immediately halts user-mode execution.

### 2. Positional Base-3 (Arithmetic / VM Level)
Used by `TernaryScalar` (T40/T50) for high-performance math. This encoding maps balanced ternary values into a large binary integer using a **Power-of-3** sum.

**The Formula:**
Value = $\sum_{i=0}^{n-1} trit_i \times 3^i$

| Digit Value | Trit Meaning |
| :--- | :--- |
| `0` | Balanced `0` |
| `1` | Balanced `+1` |
| `2` | Balanced `-1` |

---

The VM registers (`TernaryValue`) pack trits into host 64-bit and 128-bit integers using the positional encoding. Multiple widths are supported for tiered precision:

*   **T5 (Tryte)**: 5 trits. Fits in a **binary uint8_t** ($3^5 = 243$).
*   **T10**: 10 trits. Fits in a **binary uint16_t** ($3^{10} = 59,049$).
*   **T20**: 20 trits. Fits in a **binary uint32_t** ($3^{20} = 3,486,784,401$).
*   **T40 (Triple)**: 40 trits. Fits in a **binary uint64_t** ($3^{40} < 2^{64}$).
*   **T50 (LongTriple)**: 50 trits. Fits in a **binary uint128_t** ($3^{50} < 2^{128}$).

### T50 Register Layout (Internal)
A T50 register is split into a **Mantissa** and an **Exponent**, allowing it to represent massive ranges while maintaining ternary precision.

| Trit Range | Field | Role |
| :--- | :--- | :--- |
| `0 – 40` | **Mantissa** | The significand of the ternary value. |
| `41 – 49` | **Exponent** | The power-of-3 multiplier. |

---

## 📐 Conversion Table: Balanced to Positional
When moving from the **ISA (Dual-Rail)** to the **ALU (Positional)**, the VM performs a conversion:

| Balanced Trit | Dual-Rail (Bin) | Positional (Base-3) |
| :--- | :--- | :--- |
| `-1` | `00` | `2` |
| `0` | `01` | `0` |
| `+1` | `10` | `1` |

---

## 🛡️ Sentinel Values
Certain bit patterns in the positional representation are reserved for "Special" hardware states:
*   **Overflow**: $3^n$ (The value just past the maximum capacity).
*   **Underflow**: Used for values smaller than the minimum representable exponent.
*   **Invalid**: Used for math errors (like `SQRT(-1)`).
