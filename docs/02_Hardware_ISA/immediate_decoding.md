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

### 1. `imm16` (I-Type)
*   **Source**: Instruction trits `[15:0]`.
*   **Logic**: The 16 trits are shifted into the lower 16 slots of the destination register. All remaining trits (16 to 49) are forced to **Neutral (0)**.

### 2. `offset19` (B-Type)
*   **Source**: Instruction trits `[18:0]`.
*   **Logic**: The 19 trits are extracted and expanded to the full 50-trit width of the `PC` using zero-padding. The result is then added to the current `PC` for relative jumping.

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
> Because `imm16` covers over 21 million addresses, the Trit-Stack can address its entire data memory space using a single `LOAD` or `STORE` instruction with a zero base register (`r0`).
