# Hardware Contract (HDL/Verilog)

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_backend.h`, `ternary_lanes.h` |

---

## 💎 Dual-Rail Encoding
The physical representation of a trit in the Trit-Stack hardware is a **2-bit pair**. This encoding is consistent across the VM, the transport lanes (SIMD), and the planned FPGA/ASIC implementation.

| Trit Value | Bit Pair (Binary) | Hex equivalent |
| :--- | :--- | :--- |
| **Negative (-1)** | `00` | `0x0` |
| **Neutral (0)** | `01` | `0x1` |
| **Positive (+1)** | `10` | `0x2` |
| **INVALID** | `11` | `0x3` |

### Invariant: The Sticky Invalid
The bit pattern `11` is reserved as a hardware-level sentinel. 
1.  **Detection**: Any functional unit (ALU, MUX, Shifter) must check for `11`.
2.  **Propagation**: If any input is `11`, the output must be `11`.
3.  **Trap**: In the VM, fetching or moving an `INVALID` trit into a register triggers a `TRAP_ILLEGAL_OP`.

---

## ⚡ Gate-to-Silicon Mapping

### 1. Inverter (NEG)
In Dual-Rail, negation is a trivial arithmetic operation on the 2-bit pair:
**`f(x) = 2 - x`**

| Input (x) | Logic Result (2-x) | Semantic |
| :--- | :--- | :--- |
| `00` (0) | `10` (2) | $-1 \rightarrow +1$ |
| `01` (1) | `01` (1) | $0 \rightarrow 0$ |
| `10` (2) | `00` (0) | $+1 \rightarrow -1$ |

### 2. Multiplexer (TSEL)
The `TSEL` instruction maps directly to a high-efficiency 3-to-1 MUX. Because the control trit is Dual-Rail, the hardware simply uses the bit-pair to enable one of three input paths.

---

## 📦 Transport (SIMD Lanes)
To maximize throughput on binary host buses (PCIe/DDR), trits are packed into standard binary words.

*   **Lane 32**: 16 trits (32 bits)
*   **Lane 64**: 32 trits (64 bits)
*   **Lane 128**: 64 trits (128 bits)

> [!WARNING]
> **Padding Rules**: Any unused bits in a binary word must be tied to `0`. If a functional unit detects non-zero bits in a padding zone, it must signal a parity-style fault.

---

## 🛠️ Verification Interface
The `ternary_backend.h` file provides C++ implementations of these bitwise operations. Any HDL implementation (Verilog/VHDL) is considered valid ONLY if it matches the output of `sandbox::backend` bit-for-bit.
