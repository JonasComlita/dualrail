# ISA Instruction Word Encoding

Source of truth: `ternary_isa.h` — `TritWord27`, field constants, `InstructionWord::decode()`.

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

Extended **R4-type** (TWCMP, TCLAMP):
```
[26]  [25:22]  [21:19]  [18:16]  [15:13]  [12:10]  [9:7]  [6:0]
 fmt  opcode     Rd       Rs1      Rs2      Rs3      func   rsv(7)
```

Extended **R5-type** (TSEL, VSEL):
```
[26]  [25:22]  [21:19]  [18:16]  [15:13]  [12:10]  [9:7]   [6:0]
 fmt  opcode     Rd      rCond    rNeg     rZero    rPos    rsv(7)
```
For VSEL, the width suffix is encoded in the reserved field [2:0].

---

## I-Type Layout (trit[26] = 0)

```
[26]  [25:22]  [21:19]  [18:16]  [15:0]
 fmt  opcode     Rd       Rs1     imm16 (signed, 16 trits)
  1      4        3        3        16
```

**Vector memory overlay** (VLOAD/VSTORE):
```
[26]  [25:22]  [21:19]  [18:16]  [15:13]  [12:0]
 fmt  opcode  vRd/vSrc  rBase    func     imm13
```

**Immediate range:** ±21,523,360 (±(3^16−1)/2)

---

## B-Type Layout (trit[26] = −1)

```
[26]  [25:22]  [21:19]  [18:0]
 fmt  opcode     Rs     offset19 (signed, 19 trits)
  1      4        3        19
```

**offset19 range:** ±581,130,733 (±(3^19−1)/2)

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

Valid range: 0–79 (opcode 80+ → `TRAP_ILLEGAL_OP`).

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
