# Opcode Master Table

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_isa.h`, `ternary_vm.h` |

---

## 🏗️ Instruction Formats
The system uses a fixed-width **27-trit** instruction word. The Format is determined by `trit[26]`.

| Format | Trit[26] | Layout Summary |
| :--- | :--- | :--- |
| **R-Type** | `+1` | Register-to-Register (Arithmetic/Logic) |
| **I-Type** | `0` | Immediate / Memory (Load/Store) |
| **B-Type** | `-1` | Branch / Control Flow |

---

## 🛠️ System & Control
| Mnemonic | Op | Fmt | Description |
| :--- | :--- | :--- | :--- |
| `NOP` | 0 | R | No Operation. |
| `HALT` | 1 | R | Stop VM execution. |
| `SYSCALL` | 69 | R | Trigger a supervisor trap (User → Kernel). |
| `FENCE` | 70 | R | Memory barrier for multi-trit consistency. |
| `JMP` | 18 | B | Unconditional relative jump. |
| `BRN` | 19 | B | Branch if `Rs.trit[0] == -1` (Negative). |
| `BRZ` | 24 | B | Branch if `Rs.trit[0] == 0` (Zero). |
| `BRP` | 25 | B | Branch if `Rs.trit[0] == +1` (Positive). |
| `CALL` | 20 | B | Call function (`r25 = PC+1`, jump to offset). |
| `RET` | 21 | R | Return from function (`PC = r25`). |

## 📦 Data Movement & Memory
| Mnemonic | Op | Fmt | Description |
| :--- | :--- | :--- | :--- |
| `MOV` | 2 | I | Load 16-trit signed immediate into `Rd`. |
| `MOVH` | 3 | I | Load immediate into upper bits of `Rd`. |
| `COPY` | 4 | R | Register copy: `Rd = Rs1`. |
| `SWAP` | 26 | R | Swap contents of `Rd` and `Rs1`. |
| `LOAD` | 16 | I | `Rd = mem[Rs1 + imm16]`. |
| `STORE` | 17 | I | `mem[Rs1 + imm16] = Rd`. |

## ➕ Arithmetic & Logic
| Mnemonic | Op | Fmt | Description |
| :--- | :--- | :--- | :--- |
| `ADD` | 5 | R | `Rd = Rs1 + Rs2`. |
| `SUB` | 6 | R | `Rd = Rs1 - Rs2`. |
| `MUL` | 7 | R | `Rd = Rs1 * Rs2`. |
| `DIV` | 8 | R | `Rd = Rs1 / Rs2`. |
| `SQRT` | 9 | R | `Rd = sqrt(Rs1)`. |
| `NEG` | 10 | R | `Rd = -Rs1` (Trit flip). |
| `ABS` | 11 | R | `Rd = abs(Rs1)`. |
| `TCMP` | 12 | R | `Rd = sign(Rs1 - Rs2)` ∈ {-1, 0, +1}. |
| `TMIN` | 13 | R | Trit-wise minimum. |
| `TMAX` | 14 | R | Trit-wise maximum. |
| `TSEL` | 23 | R5 | Select `rNeg/rZero/rPos` based on `rCond`. |
| `TWCMP` | 59 | R4 | Windowed comparison (High-performance bounds check). |

## 🤖 AI & Vector (BitNet Support)
| Mnemonic | Op | Fmt | Description |
| :--- | :--- | :--- | :--- |
| `VMAC` | 50 | R | Vector Multiply-Accumulate. |
| `VACT` | 51 | R | Vector Activation function (ReLU-like for ternary). |
| `VDOT` | 49 | R | Dot product of two ternary vectors. |
| `AMUL` | 47 | R | Accumulator-based multiplication. |
| `VLEN` | 42 | R | Set active vector length. |

---

## 📋 Full Opcode Index (0–73)
*See `ternary_isa.h` for exact field bit-offsets.*

| Op | Mnemonic | Op | Mnemonic | Op | Mnemonic |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 0 | `NOP` | 25 | `BRP` | 50 | `VMAC` |
| 1 | `HALT` | 26 | `SWAP` | 51 | `VACT` |
| 2 | `MOV` | 27 | `TLADD` | 52 | `VPACK` |
| 3 | `MOVH` | 28 | `TLSUB` | 53 | `VUNPACK` |
| 4 | `COPY` | 29 | `TLNEG` | 54 | `VPERMUTE` |
| 5 | `ADD` | 30 | `TLAND` | 55 | `VBLEND` |
| 6 | `SUB` | 31 | `TLOR` | 56 | `VSWAP` |
| 7 | `MUL` | 32 | `VADD` | 57 | `VGATHER` |
| 8 | `DIV` | 33 | `VSUB` | 58 | `VSCATTER` |
| 9 | `SQRT` | 34 | `VNEG` | 59 | `TWCMP` |
| 10 | `NEG` | 35 | `VMUL` | 60 | `CALLR` |
| 11 | `ABS` | 36 | `VDIV` | 61 | `JMPR` |
| 12 | `TCMP` | 37 | `VCMP` | 62 | `TMOD` |
| 13 | `TMIN` | 38 | `VSEL` | 63 | `TLSHIFT` |
| 14 | `TMAX` | 39 | `VLOAD` | 64 | `TRSHIFT` |
| 15 | `TINV` | 40 | `VSTORE` | 65 | `TMAC` |
| 16 | `LOAD` | 41 | `VBCAST` | 66 | `TCOUNT` |
| 17 | `STORE` | 42 | `VLEN` | 67 | `TSCAN` |
| 18 | `JMP` | 43 | `ACLR` | 68 | `TCLAMP` |
| 19 | `BRN` | 44 | `ALOAD` | 69 | `SYSCALL` |
| 20 | `CALL` | 45 | `AADD` | 70 | `FENCE` |
| 21 | `RET` | 46 | `ASUB` | 71 | `VSUM` |
| 22 | `CVT` | 47 | `AMUL` | 72 | `VHMIN` |
| 23 | `TSEL` | 48 | `ASTORE` | 73 | `VHMAX` |
| 24 | `BRZ` | 49 | `VDOT` | | |
