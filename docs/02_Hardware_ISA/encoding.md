# Instruction Encoding & Layout

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_isa.h` |

---

## 📐 The 27-Trit Word
All instructions in the Trit-Stack are fixed-width **27-trit words**. This symmetry simplifies the fetch-decode pipeline and allows for a single-cycle instruction fetch on hardware.

### The Format Discriminant (`trit[26]`)
The most significant trit (MST) determines the instruction layout:

| Value | Format | Purpose |
| :--- | :--- | :--- |
| **+1** | **R-Type** | Register-register operations (Arithmetic, Logic, SIMD). |
| ** 0** | **I-Type** | Immediate operations (MOV, LOAD, STORE). |
| **-1** | **B-Type** | Control flow (JMP, BRN, CALL). |

---

## 🛠️ Format Layouts

### 1. R-Type (Register-Register)
Used for operations where both operands are in registers.

| Field | Trits | Position | Purpose |
| :--- | :--- | :--- | :--- |
| **FMT** | 1 | `[26]` | Set to `+1`. |
| **OPCODE** | 4 | `[25:22]` | The instruction identifier. |
| **Rd** | 3 | `[21:19]` | Destination Register (r0–r26). |
| **Rs1** | 3 | `[18:16]` | Source Register 1. |
| **Rs2** | 3 | `[15:13]` | Source Register 2. |
| **FUNC** | 3 | `[12:10]` | Width Selector (T1, T5, T40, etc). |
| **PAD** | 10 | `[9:0]` | Reserved for future extension. |

### 2. I-Type (Immediate)
Used for constants and memory access with offsets.

| Field | Trits | Position | Purpose |
| :--- | :--- | :--- | :--- |
| **FMT** | 1 | `[26]` | Set to `0`. |
| **OPCODE** | 4 | `[25:22]` | The instruction identifier. |
| **Rd** | 3 | `[21:19]` | Destination Register. |
| **Rs1** | 3 | `[18:16]` | Source Register (Base address). |
| **IMM16** | 16 | `[15:0]` | **Signed** 16-trit immediate value. |

*Range: ±21,523,360 (Covers full 50-trit memory space).*

### 3. B-Type (Control Flow)
Used for PC-relative branches and jumps.

| Field | Trits | Position | Purpose |
| :--- | :--- | :--- | :--- |
| **FMT** | 1 | `[26]` | Set to `-1`. |
| **OPCODE** | 4 | `[25:22]` | The instruction identifier. |
| **Rs** | 3 | `[21:19]` | Source Register (Condition check). |
| **OFF19** | 19 | `[18:0]` | **Signed** 19-trit PC-relative offset. |

*Range: ±581,130,733 instructions.*

---

## 🧬 Special Layouts

### TSEL (Ternary Select)
The `TSEL` instruction uses an extended R-type layout (R5) to fit 5 register indices:

| Field | Trits | Position |
| :--- | :--- | :--- |
| **Rd** | 3 | `[21:19]` |
| **rCond** | 3 | `[18:16]` |
| **rNeg** | 3 | `[15:13]` |
| **rZero** | 3 | `[12:10]` |
| **rPos** | 3 | `[9:7]` |

---

## 🛡️ Field Encoding Rules
Register indices (0–26) are stored using a **Bias of 13**.

**Formula**: `Stored = Index - 13`
*   **r0** (Zero) $\rightarrow$ `-13` (`- - -`)
*   **r13** $\rightarrow$ `0` (`0 0 0`)
*   **r26** $\rightarrow$ `+13` (`+ + +`)

> [!IMPORTANT]
> This bias ensures that the "Neutral" register field (all zeros) points to the center of the register file (r13), preventing accidental overwrites of `r0` during malformed instruction decode.
