# Immediate Decoding & Sign-Extension

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_isa.h` |

---

## 💎 The Zero-Padding Advantage
In traditional binary systems (2's complement), expanding a small signed number to a larger width requires **Sign-Extension** (copying the MSB into all upper bits).

In **Balanced Ternary**, sign-extension is mathematically identical to **Zero-Padding**.

### Mathematical Proof
A 3-trit balanced number $[t_0, t_1, t_2]$ represents the value:
$$V = t_0(3^0) + t_1(3^1) + t_2(3^2)$$

Expanding this to 5 trits by padding with zeros:
$$V' = t_0(3^0) + t_1(3^1) + t_2(3^2) + 0(3^3) + 0(3^4)$$
$$V' = V$$

**Conclusion**: The hardware decoder for Trit-Stack is significantly simpler than RISC-V or x86 decoders because it never needs to "smear" a sign bit. 

---

## 🛠️ Decoder Logic

### 1. `imm16` (direct I-Type)
*   **Source**: Instruction trits `[15:0]`.
*   **Logic**: The 16 balanced trits are decoded as a signed host integer and
    converted to the native T40 scalar value. Conceptually, widening pads trits
    16 through 39 with **Neutral (0)**.

ISA-v2 I-type extensions instead reserve `[15:12]` for their selector and use
a signed `imm12`. VLOAD/VSTORE reserve `[12:9]` for the selector and use a
signed `imm9`.

### 2. `offset19` (direct B-Type)
*   **Source**: Instruction trits `[18:0]`.
*   **Logic**: The 19 trits are decoded as a signed host integer and added to
    the word-indexed instruction-memory PC. The VM stores its PC as an integer;
    it is not a T50 scalar register.

ISA-v2 B-type extensions reserve `[18:15]` for their selector and use a signed
`offset15`.

---

## 🔌 Hardware Implementation
In the Dual-Rail FPGA implementation, "Zero-Padding" is a wiring-level constant. Any trit slot in the destination register or ALU input that is not occupied by the immediate is tied to the **`0b01` (Neutral)** bit pattern.

> [!NOTE]
> The canonical implementation of this logic can be found in `isa::decodeSigned()` within `ternary_isa.h`.

---

## 📊 Range Table

| Field | Trits | Min (Decimal) | Max (Decimal) |
| :--- | :--- | :--- | :--- |
| **imm16** | 16 | -21,523,360 | +21,523,360 |
| **offset19** | 19 | -581,130,733 | +581,130,733 |

> [!TIP]
> `imm16` covers the default 1,000,000-word DMEM from a zero base register. It
> does not cover the full mathematical T40 address range or every potentially
> configured memory size; larger addresses use a nonzero base register.
