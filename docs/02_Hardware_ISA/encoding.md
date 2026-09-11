# ISA Instruction Word Encoding

Sources of truth: `ARCHITECTURE_MANIFEST.json` for the ISA-v2 wire contract and
`ternary_isa.h` for `TritWord27` and field access. Public words are encoded and
decoded through `VersionedInstructionCodec`; `InstructionWord`'s
`encodeSemantic*`/`decodeSemantic` helpers are private assembler staging logic.

---

## Instruction Container: `TritWord27`

Every instruction is a 27-trit word stored in a `uint64_t` using **2 bits per trit** (Scheme B encoding):

```
Bits [63:54] — always zero (padding)
Bits [53: 0] — 27 trits × 2 bits each

Bit pair encoding:
  0b00 → trit −1 (T_NEG)
  0b01 → trit  0 (T_ZER)
  0b10 → trit +1 (T_POS)
  0b11 → INVALID  → TRAP_ILLEGAL_OP on decode
```

Trit `i` occupies bits `[2i+1 : 2i]`. Trit 0 = LST (least significant). Trit 26 = MST (format discriminant).

---

## Format Discriminant (trit[26])

| trit[26] | Format | Use case |
|----------|--------|----------|
| `+1`     | **R-type** | Register-register arithmetic, logic, CSR |
| ` 0`     | **I-type** | Immediate load, memory access with offset |
| `−1`     | **B-type** | Branches, jumps, calls |

---

## R-Type Layout (trit[26] = +1)

```
[26]  [25:22]  [21:19]  [18:16]  [15:13]  [12:10]  [9:0]
 fmt  opcode     Rd       Rs1      Rs2      func     pad(10)
  1      4        3        3        3        3         10
```

ISA-v2 extension **R-type** operations replace the reserved low ten trits with
an unsigned extension selector:

```
[26]  [25:22]  [21:19]  [18:16]  [15:13]  [12:10]  [9:0]
 fmt   EXT=80     Rd       Rs1      Rs2      func     selector
```

Extended **R4-type** (TWCMP, TCLAMP, TSTR):
```
[26]  [25:22]  [21:19]  [18:16]  [15:13]  [12:10]  [9:7]  [6:0]
 fmt  opcode     Rd       Rs1      Rs2      Rs3      func   rsv(7)
```
For an ISA-v2 extension, selector trits occupy `[3:0]`; `[6:4]` remain
reserved.

Extended **R5-type** (TSEL, VSEL):
```
[26]  [25:22]  [21:19]  [18:16]  [15:13]  [12:10]  [9:7]   [6:0]
 fmt  opcode     Rd      rCond    rNeg     rZero    rPos    rsv(7)
```
For ISA-v2 extension VSEL/VBLEND, `[6:4]` contains the width and `[3:0]`
contains the extension selector.

---

## I-Type Layout (trit[26] = 0)

```
[26]  [25:22]  [21:19]  [18:16]  [15:0]
 fmt  opcode     Rd       Rs1     imm16 (signed, 16 trits)
  1      4        3        3        16
```

**ISA-v2 vector-memory extension** (VLOAD/VSTORE):
```
[26]  [25:22]  [21:19]  [18:16]  [15:13]  [12:9]   [8:0]
 fmt   EXT=80  vRd/vSrc  rBase     func    selector   imm9
```

Other I-type extensions use a selector in `[15:12]` and a signed `imm12` in
`[11:0]`.

**Immediate range:** ±21,523,360 (±(3^16−1)/2)

---

## B-Type Layout (trit[26] = −1)

```
[26]  [25:22]  [21:19]  [18:0]
 fmt  opcode     Rs     offset19 (signed, 19 trits)
  1      4        3        19
```

**offset19 range:** ±581,130,733 (±(3^19−1)/2)

ISA-v2 B-type extensions use a selector in `[18:15]` and a signed `offset15`
in `[14:0]`.

`STORE` note: The `Rd` field position is reused for the source register. STORE writes to `mem[Rs1 + imm16]` using the data in the register named in the `Rd` field position.

---

## Field Extraction

All field constants use the pattern `(lsb, width)`:

| Constant | LSB | Width | Meaning |
|----------|-----|-------|---------|
| `FIELD_FMT_LSB/W`   | 26 | 1 | Format discriminant |
| `FIELD_OP_LSB/W`    | 22 | 4 | Opcode |
| `FIELD_RD_LSB/W`    | 19 | 3 | Destination register |
| `FIELD_RS1_LSB/W`   | 16 | 3 | Source register 1 |
| `FIELD_RS2_LSB/W`   | 13 | 3 | Source register 2 |
| `FIELD_FUNC_LSB/W`  | 10 | 3 | Function select |
| `FIELD_IMM16_LSB/W` |  0 | 16 | 16-trit immediate |
| `FIELD_OFF19_LSB/W` |  0 | 19 | 19-trit branch offset |
| `FIELD_BRS_LSB/W`   | 19 | 3  | Branch source register |

---

## Opcode Encoding

The 4-trit opcode field is read as an **unsigned base-3 integer**:

```
rawOp = (trit[22] + 1) * 1
      + (trit[23] + 1) * 3
      + (trit[24] + 1) * 9
      + (trit[25] + 1) * 27
```

ISA-v2 direct opcodes are 0–14 and 16–37. Direct value 15 and values 38–79
are reserved.
Opcode 80 is `EXT`, which selects an operation through a format-specific
extension field. Any unassigned direct value or extension selector decodes as
`Opcode::RESERVED` and traps at the VM boundary.

The C++ `Opcode` enum is a semantic operation identifier. Values such as
`Opcode::CALLR == 60` and `Opcode::TLADD == 27` are not automatically their
wire opcodes: CALLR maps to direct opcode 27, while TLADD maps to `EXT=80` plus
selector 27.

---

## Private V3 Vector-Context Escape

V3 retains ISA-v2 encoding and reserves a private, feature-gated escape for
privileged vector context transfer:

```
[26]  [25:22]  [21:19]  [18:14]  [13:12]  [11:0]
 fmt   EXT=80   rBase    selector  neutral   offset12
```

Selectors 81 and 82 identify VCTXSTORE and VCTXLOAD. The decoder validates all
27 trits, requires the two reserved trits to be neutral, and exposes the
instruction only to a v3 kernel with the VECTOR_CONTEXT feature.

---

## Register Field Encoding

A 3-trit register field maps indices 0–26 to balanced range −13..+13:
```
stored_balanced_value = register_index − REG_FIELD_OFFSET
                      = register_index − 13
```

Example: `r13` → stored as 0 (T_ZER), `r0` → stored as −13, `r26` → stored as +13.

---

## Immediates Are Always Signed

All immediates use balanced ternary — there is no unsigned immediate format. Zero-padding is sign-extension. Implemented via `decodeSigned()` in `ternary_isa.h`.

---

## Atomic Ordering (FENCE func field)

| func value | Ordering |
|------------|---------|
| `FUNC_ORDER_RELAXED` (= FUNC_DEFAULT − 1) | Relaxed |
| `FUNC_ORDER_ACQ_REL` (= FUNC_DEFAULT + 0) | Acquire-Release |
| `FUNC_ORDER_SEQ_CST` (= FUNC_DEFAULT + 1) | Sequentially Consistent |
