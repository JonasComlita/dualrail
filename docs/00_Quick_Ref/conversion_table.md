# Ternary Conversion & Reference Table

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_scalar.h`, `ternary_math.h` |

---

## 🔢 Small Magnitude Conversions (3-Trit)
This table shows how decimal values map to the **Positional Base-3** storage used by the VM. 
*Stored Value = $\sum (trit_i + 1) \times 3^i$*

| Decimal | Balanced Ternary | Digits (Base-3) | Stored Int | Hex |
| :--- | :--- | :--- | :--- | :--- |
| **-13** | `- - -` | `0 0 0` | 0 | `0x00` |
| **-12** | `- - 0` | `0 0 1` | 1 | `0x01` |
| **-11** | `- - +` | `0 0 2` | 2 | `0x02` |
| **-10** | `- 0 -` | `0 1 0` | 3 | `0x03` |
| **-9** | `- 0 0` | `0 1 1` | 4 | `0x04` |
| **-4** | `0 - -` | `1 0 0` | 9 | `0x09` |
| **-3** | `0 - 0` | `1 0 1` | 10 | `0x0A` |
| **-2** | `0 - +` | `1 0 2` | 11 | `0x0B` |
| **-1** | `0 0 -` | `1 1 0` | 12 | `0x0C` |
| **0** | `0 0 0` | `1 1 1` | 13 | `0x1D` |
| **1** | `0 0 +` | `1 1 2` | 14 | `0x1E` |
| **2** | `0 + -` | `1 2 0` | 15 | `0x0F` |
| **3** | `0 + 0` | `1 2 1` | 16 | `0x10` |
| **4** | `0 + +` | `1 2 2` | 17 | `0x11` |
| **9** | `+ 0 0` | `2 1 1` | 22 | `0x16` |
| **13** | `+ + +` | `2 2 2` | 26 | `0x1A` |

---

## 📦 The Tryte (T5) Reference
The **T5 Tryte** is the standard 8-bit aligned unit. It follows strict balanced logic where the numeric zero is biased to the center of the range.

| Feature | Decimal | Stored (uint8_t) | Hex |
| :--- | :--- | :--- | :--- |
| **Minimum** | `-121` | `0` | `0x00` |
| **Numeric Zero** | `0` | `121` | `0x79` |
| **Maximum** | `+121` | `242` | `0xF2` |
| **Sentinel: Underflow** | N/A | `254` | `0xFE` |
| **Sentinel: Overflow** | N/A | `255` | `0xFF` |

---

## 🛡️ The Zero Sentinel (T40 / T50)
For higher-precision types based on `TernaryScalar<N>`, the system uses a **Zero Sentinel** pattern for performance.

*   **Logic**: `isZero()` returns true if `data == 0`.
*   **Physical State**: A `TernaryScalar` with `data == 0` has positional digits that would physically decode as **all -1s**.
*   **Numeric Behavior**: `unpack()`, `getTritRaw()`, and arithmetic expand this reserved state as neutral trits representing numeric `0.0`.
*   **Raw Storage Behavior**: `unpackPositional()` is the explicit escape hatch for storage formats, such as packed ternary model weights, that need the physical all-negative decoding.
*   **Construction Rule**: `pack()` remains a positional encoder and rejects the all-negative collision as overflow. Numeric constructors and arithmetic create canonical zero directly as raw zero.

| Type | Biased Zero (All trits=0) | Zero Sentinel (data=0) |
| :--- | :--- | :--- |
| **T5 (Tryte)** | `121` (Canonical Zero) | `-121` (Numeric Min) |
| **T40 / T50** | `(3^N-1)/2` (Regular Value) | **0** (Canonical Zero) |

---

## ⚡ Quick Conversion Logic
To calculate the positional payload of a balanced integer **N** for an
**X-trit** width before applying a numeric format's reserved-state rules:

1.  **Find the Bias**: $B = \frac{3^X - 1}{2}$
2.  **Add the Bias**: $Stored = N + B$
3.  **Result**: This is the value you will see in a memory hex dump.

*Example for T5 (Tryte):*
*   To store **5**: $5 + 121 = 126$ (`0x7E`).
*   To store **-10**: $-10 + 121 = 111$ (`0x6F`).

This bias formula describes strict positional integer storage. T10–T50 numeric
formats additionally canonicalize floating zero to raw `0`.
