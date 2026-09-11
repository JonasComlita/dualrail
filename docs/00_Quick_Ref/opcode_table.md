# Opcode Table

Sources of truth: `ARCHITECTURE_MANIFEST.json` for ISA-v2 wire values and
`ternary_isa.h` for semantic `Opcode` identities.

## How Opcodes Are Encoded

The wire opcode occupies **4 trits** (positions [25:22]) of a 27-trit
instruction word and is read as an unsigned base-3 integer from 0 through 80.
The C++ semantic ID listed below is not necessarily that wire value.

`rawOp = sum over i of (trit[22+i] + 1) * 3^i` → unsigned base-3 → opcode index.

ISA-v2 direct wire opcodes are 0–14 and 16–37. Direct value 15 and values
38–79 are reserved; wire opcode 80 is `EXT`. Extended instructions carry a
format-specific selector.
Notable semantic-to-wire mappings are:

| Semantic operation | ISA-v2 wire encoding |
|---|---|
| `TINV` | direct `NEG` opcode 10 |
| `CALLR` through `TSTR` | direct opcodes 27 through 36 |
| `WAIT` | direct opcode 37 |
| Lane/vector/advanced operations | `EXT=80` plus their extension selector |
| `VCTXSTORE` / `VCTXLOAD` | private v3 `EXT=80` selectors 81 / 82 |

---

## Semantic Operation Table

| Semantic ID | Mnemonic | Format | Operation |
|----|----------|--------|-----------|
| 0  | `NOP`    | Any    | No operation |
| 1  | `HALT`   | Any    | Stop execution |
| 2  | `MOV`    | I      | `Rd ← imm16` (load 16-trit signed immediate) |
| 3  | `MOVH`   | I      | `Rd ← imm16 << 16` (load upper half) |
| 4  | `COPY`   | R      | `Rd ← Rs1` |
| 5  | `ADD`    | R      | `Rd ← Rs1 + Rs2` |
| 6  | `SUB`    | R      | `Rd ← Rs1 − Rs2` |
| 7  | `MUL`    | R      | `Rd ← Rs1 × Rs2` |
| 8  | `DIV`    | R      | `Rd ← Rs1 ÷ Rs2` (zero → TRAP_DIV_ZERO) |
| 9  | `SQRT`   | R      | `Rd ← √Rs1` |
| 10 | `NEG`    | R      | `Rd ← −Rs1` (trit flip) |
| 11 | `ABS`    | R      | `Rd ← \|Rs1\|` |
| 12 | `TCMP`   | R      | `Rd ← sign(Rs1 − Rs2) ∈ {-1,0,+1}` |
| 13 | `TMIN`   | R      | `Rd ← min(Rs1, Rs2)` |
| 14 | `TMAX`   | R      | `Rd ← max(Rs1, Rs2)` |
| 15 | `TINV`   | R      | `Rd ← −Rs1` (alias for NEG) |
| 16 | `LOAD`   | I      | `Rd ← mem[Rs1 + imm16]` |
| 17 | `STORE`  | I      | `mem[Rs1 + imm16] ← Rd` (Rd is source) |
| 18 | `JMP`    | B      | `PC ← PC + offset19` (unconditional) |
| 19 | `BRN`    | B      | Branch if `Rs.trit[0] == −1` |
| 20 | `CALL`   | B      | `r25 ← PC+1; PC ← PC + offset19` |
| 21 | `RET`    | R      | `PC ← toLong(r25)` |
| 22 | `CVT`    | R      | Convert `Rs1` to width selected by `func` |
| 23 | `TSEL`   | R5     | `Rd ← rNeg/rZero/rPos` based on `rCond.trit[0]` |
| 24 | `BRZ`    | B      | Branch if `Rs.trit[0] == 0` |
| 25 | `BRP`    | B      | Branch if `Rs.trit[0] == +1` |
| 26 | `SWAP`   | R      | Swap `Rd` and `Rs1` |
| 27 | `TLADD` | R      | Per-trit lane add (carryless) |
| 28 | `TLSUB` | R      | Per-trit lane subtract (carryless) |
| 29 | `TLNEG` | R      | Per-trit lane negation |
| 30 | `TLAND` | R      | Per-trit lattice min (ternary AND) |
| 31 | `TLOR`  | R      | Per-trit lattice max (ternary OR) |
| 32 | `VADD`  | R      | Vector add (width from func) |
| 33 | `VSUB`  | R      | Vector subtract |
| 34 | `VNEG`  | R      | Vector negate |
| 35 | `VMUL`  | R      | Vector multiply |
| 36 | `VDIV`  | R      | Vector divide |
| 37 | `VCMP`  | R      | Vector compare → L1 predicate |
| 38 | `VSEL`  | R5     | Vector select (3-way) |
| 39 | `VLOAD` | I      | Vector load from DMEM |
| 40 | `VSTORE`| I      | Vector store to DMEM |
| 41 | `VBCAST`| R      | Broadcast scalar to vector |
| 42 | `VLEN`  | R      | Write vector length to Rd |
| 43 | `ACLR`  | R      | Clear AI accumulator |
| 44 | `ALOAD` | R      | Load scalar into accumulator slot |
| 45 | `AADD`  | R      | Add scalar to accumulator |
| 46 | `ASUB`  | R      | Subtract scalar from accumulator |
| 47 | `AMUL`  | R      | Multiply accumulator |
| 48 | `ASTORE`| R      | Store accumulator → Rd |
| 49 | `VDOT`  | R      | Vector dot product (T1 × T1 → T50) |
| 50 | `VMAC`  | R      | Vector multiply-accumulate |
| 51 | `VACT`  | R      | Activation function (sign → L1) |
| 52 | `VPACK` | R      | Pack vector elements |
| 53 | `VUNPACK`| R     | Unpack vector elements |
| 54 | `VPERMUTE`| R    | Permute vector by index |
| 55 | `VBLEND`| R5     | Blend two vectors with mask |
| 56 | `VSWAP` | R      | Swap two vector registers |
| 57 | `VGATHER`| R     | Gather from DMEM using index vector |
| 58 | `VSCATTER`| R    | Scatter to DMEM using index vector |
| 59 | `TWCMP` | R4     | Three-way compare with window |
| 60 | `CALLR` | R      | Indirect call: `r25 ← PC+1; PC ← Rs1` |
| 61 | `JMPR`  | R      | Indirect jump: `PC ← Rs1` |
| 62 | `TMOD`  | R      | Ternary modulo |
| 63 | `TLSHIFT`| R     | Trit left shift |
| 64 | `TRSHIFT`| R     | Trit right shift |
| 65 | `TMAC`  | R      | Scalar multiply-accumulate |
| 66 | `TCOUNT`| R      | Count non-zero trits |
| 67 | `TSCAN` | R      | Trit scan (prefix sum) |
| 68 | `TCLAMP`| R4     | Clamp to [min, max] |
| 69 | `SYSCALL`| R     | Trap into kernel (uses CSR syscall_id) |
| 70 | `FENCE` | R      | Memory fence (func = RELAXED/ACQ_REL/SEQ_CST) |
| 71 | `VSUM`  | R      | Vector horizontal sum |
| 72 | `VHMIN` | R      | Vector horizontal min |
| 73 | `VHMAX` | R      | Vector horizontal max |
| 74 | `CSRR`  | R/I    | `Rd ← CSR[imm]` (read CSR) |
| 75 | `CSRW`  | R/I    | `CSR[imm] ← Rs1` (write CSR) |
| 76 | `ERET`  | R      | Exception return (restore privilege + jump EPC) |
| 77 | `CSRRW` | R      | `Rd ← CSR[imm]; CSR[imm] ← Rs1` (atomic RW) |
| 78 | `TLDR`  | I      | Trit load (sub-word) |
| 79 | `TSTR`  | I      | Trit store (sub-word) |
| 80 | `WAIT` | B | Retire once and wait for an architectural event |
| 81 | `TLBINV` | R | Invalidate selected TLB entries (`EXT` selector 80) |
| 82 | `VCTXSTORE` | I/private | Privileged v3 vector-context save |
| 83 | `VCTXLOAD` | I/private | Privileged v3 vector-context restore |
| 255 | `RESERVED` | — | In-memory semantic sentinel; never encoded |

---

## Width Suffix (`func` field)

Many R-type instructions use the `func` field to select the numeric width:

| func constant | Mnemonic suffix | Trit width | C++ type |
|--------------|----------------|-----------|---------|
| `FUNC_T1`  | `.t1`  | 1  | `T1` |
| `FUNC_T5`  | `.t5`  | 5  | `T5` |
| `FUNC_T10` | `.t10` | 10 | `T10` |
| `FUNC_T20` | `.t20` | 20 | `T20` |
| `FUNC_T40` | `.t40` | 40 | `Triple` (default) |
| `FUNC_T50` | `.t50` | 50 | `LongTriple` |
| `FUNC_L1`  | `.l1`  | 1  | `TritLane1` (lane mode) |
| `FUNC_L20` | `.l20` | 20 | `TritLane20` |
| …           | …      | …  | … |

**Default width is T40** (`FUNC_T40 = 12`).

---

## Three-Way Branch Pattern

```asm
TCMP  r3, r1, r2        ; r3 = sign(r1 - r2) ∈ {-1, 0, +1}
BRN   r3, negative_lbl  ; branch if r3 == -1
TINV  r4, r3            ; r4 = -r3 (positive becomes negative)
BRN   r4, positive_lbl  ; branch if original was +1
; fall through: r3 == 0
```

---

## SYSCALL Protocol

```asm
; Write syscall ID to CSR
MOV   r0, <service_id>
CSRW  csr_syscall_id, r0

; Set arguments in r13–r16
MOV   r13, <arg0>
MOV   r14, <arg1>

; Invoke
SYSCALL

; Results in r13 (status), r14 (payload), r15 (detail)
```
