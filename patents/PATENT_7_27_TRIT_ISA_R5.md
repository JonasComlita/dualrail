# System and Method for a 27-Trit Balanced Ternary Instruction Word with Single-Trit Format Discrimination and Five-Register Overlay Encoding

## Filing-Support Draft

This document is a technical disclosure draft for a provisional patent
application. It is not legal advice and is not a substitute for review by a
registered patent practitioner. The purpose of this draft is to disclose the
invention with enough implementation detail, variations, and source-code support
to preserve later claim options.

## Title

System and Method for a 27-Trit Balanced Ternary Instruction Word with
Single-Trit Format Discrimination and Five-Register Overlay Encoding

## Field

The disclosure relates to processor instruction encoding, virtual machines,
balanced ternary instruction sets, assembler and decoder implementations,
vector and scalar execution units, field-programmable gate arrays,
application-specific integrated circuits, and compact instruction formats using
trit-addressed or trit-packed instruction words.

## Background

Instruction sets commonly use fixed-width binary instruction words divided into
opcode fields, register fields, immediates, function fields, and reserved bits.
When an instruction set grows to include multiple operand counts, scalar and
vector operations, typed arithmetic widths, predicated operations, and memory
addressing modes, a binary encoding may need additional opcode space, prefixes,
extension words, or multiple instruction formats to represent the required
information.

Balanced ternary processors can use a single trit to represent three states.
This allows one format discriminator trit to select among three instruction
formats. However, merely replacing binary fields with ternary fields does not
solve instruction pressure when a processor needs both conventional
three-register operations and special operations requiring more than three
register operands, such as a three-way select instruction naming a destination,
a condition register, and three source registers.

There is therefore a need for a fixed-width balanced ternary instruction word
that uses a single ternary format discriminator, preserves compact field
extraction, supports scalar and vector typed operations, and overlays
additional register fields for selected instructions without adding a second
instruction word or a fourth instruction format.

## Summary

The disclosed architecture provides a fixed-width twenty-seven-trit instruction
word. In one embodiment, the instruction word is encoded as a `TritWord27`
transport value using two-bit-per-trit lane packing. A single format trit
selects among three base instruction formats:

- an R-type register-register format;
- an I-type register-immediate format; and
- a B-type branch format.

In one embodiment, the format trit uses balanced ternary values:

```text
+1 -> R-type
 0 -> I-type
-1 -> B-type
```

The remaining trits hold opcode, register, function, immediate, offset, or
reserved fields. Field extraction is performed by trit-field slicing of the
packed instruction word rather than by decimal parsing, string interpretation,
or base-3 division during decode.

In one embodiment, the R-type format is:

```text
[fmt:1 | opcode:4 | Rd:3 | Rs1:3 | Rs2:3 | func:3 | reserved:10]
```

In one embodiment, the I-type format is:

```text
[fmt:1 | opcode:4 | Rd:3 | Rs1:3 | imm16:16]
```

In one embodiment, the B-type format is:

```text
[fmt:1 | opcode:4 | Rs:3 | offset19:19]
```

In one embodiment, selected R-type opcodes use an extended five-register
overlay, referred to as R5:

```text
[fmt:1 | opcode:4 | Rd:3 | rCond:3 | rNeg:3 | rZero:3 | rPos:3 | reserved:7]
```

The opcode itself indicates that the R5 overlay is active. No additional
format-discriminator value is consumed. The R5 overlay allows one instruction
word to name five registers, such as the destination, predicate, negative-arm,
zero-arm, and positive-arm registers used by ternary select operations.

In another embodiment, vector R5 instructions use part of the reserved field as
a width or function selector:

```text
[fmt:1 | opcode:4 | vD:3 | vCond:3 | vNeg:3 | vZero:3 | vPos:3 | func:3 | reserved:4]
```

In another embodiment, vector memory instructions use an I-type overlay:

```text
[fmt:1 | opcode:4 | vReg:3 | rBase:3 | func:3 | imm13:13]
```

The same 27-trit word can therefore encode scalar arithmetic, vector
arithmetic, scalar branches, typed conversion, ternary select, vector select,
and vector memory operations without changing instruction word length.

## Technical Improvement and Prior-Art Distinction

The invention improves processor and virtual-machine operation by preserving a
fixed twenty-seven-trit fetch unit while supporting three base instruction
formats, typed scalar and vector operations, and five-register predicated
operations. The improvement is not merely an instruction set or a ternary
numbering scheme. The specific mechanism is the combination of a single
balanced ternary format discriminator, fixed trit-field slicing, opcode-driven
R5 overlay decoding, suffix/function fields for type widths, and vector memory
overlays that avoid consuming additional opcode space or fetching extension
words.

This distinguishes the mechanism from ordinary binary RISC encodings, prefix
instruction schemes, variable-length instruction encodings, conventional VLIW
formats, historical ternary arithmetic machines, and ordinary packed data
words. Those systems may provide fixed-width instructions or ternary arithmetic,
but they do not combine a 27-trit instruction word, a three-state format trit,
field-slice decoding, a five-register overlay selected by opcode, and typed
scalar/vector width selection within the same fixed instruction word.

## Definitions

- **Trit:** a ternary digit having one of three values.
- **Balanced ternary trit:** a trit representing `-1`, `0`, or `+1`.
- **TritWord27:** a twenty-seven-trit instruction or lane word.
- **Format discriminator trit:** a trit that selects among three instruction
  formats.
- **R-type format:** a register-register instruction layout.
- **I-type format:** a register-immediate instruction layout.
- **B-type format:** a branch or control-flow instruction layout.
- **R5 overlay:** an opcode-selected R-type field interpretation containing
  five register fields.
- **Function field:** a field used to select an arithmetic width, lane width,
  conversion direction, or sub-operation.
- **Width suffix:** an assembler suffix such as `.t20` or `.l20` that lowers
  into a function field.
- **Vector-memory overlay:** an I-type interpretation that stores a vector
  register field, scalar base register field, width function field, and signed
  immediate in one word.

## Brief Description of the Drawings

### Figure 1: 27-Trit Instruction Word

```text
trit 26                                                        trit 0
+--------+----------------------------------------------------------+
| fmt:1  |                  format-specific payload                 |
+--------+----------------------------------------------------------+

fmt = +1 -> R-type
fmt =  0 -> I-type
fmt = -1 -> B-type
```

### Figure 2: Base Instruction Formats

```text
R-type:
[fmt:1 | opcode:4 | Rd:3 | Rs1:3 | Rs2:3 | func:3 | reserved:10]

I-type:
[fmt:1 | opcode:4 | Rd:3 | Rs1:3 | imm16:16]

B-type:
[fmt:1 | opcode:4 | Rs:3 | offset19:19]
```

### Figure 3: R5 Overlay

```text
R5:
[fmt:1 | opcode:4 | Rd:3 | rCond:3 | rNeg:3 | rZero:3 | rPos:3 | reserved:7]

opcode selects R5 decoding
no second instruction word is fetched
```

### Figure 4: VSEL Width in Reserved Field

```text
VSEL R5:
[fmt:1 | opcode:4 | vD:3 | vCond:3 | vNeg:3 | vZero:3 | vPos:3 | func:3 | rsvd:4]
```

### Figure 5: Vector Memory Overlay

```text
VLOAD/VSTORE:
[fmt:1 | opcode:4 | vReg:3 | rBase:3 | func:3 | imm13:13]
```

### Figure 6: Decode Path

```text
fetch TritWord27
      |
      v
extract fmt trit
      |
      +-- +1: decode R-type
      |       |
      |       +-- opcode is R5-capable: decode five register fields
      |       +-- otherwise: decode Rd/Rs1/Rs2/func
      |
      +--  0: decode I-type or vector-memory overlay by opcode
      |
      +-- -1: decode B-type
```

## Detailed Description

### Fixed-Width TritWord27

In one embodiment, all executable instructions are stored as twenty-seven-trit
words. The instruction memory is word-addressed. Fetch therefore retrieves one
`TritWord27` per instruction. The instruction word may be stored in a binary
container using two-bit-per-trit lane packing while preserving ternary field
semantics.

The fixed-width instruction word allows fetch, decode, branch target
calculation, assembler label resolution, and instruction memory indexing to
operate in word units.

### Single-Trit Format Discriminator

In one embodiment, trit 26 is the format discriminator. Because the
discriminator is balanced ternary, it naturally selects among three formats.
The processor or virtual machine decodes a positive discriminator as R-type, a
zero discriminator as I-type, and a negative discriminator as B-type.

This differs from a binary discriminator bit, which selects only two formats
unless additional bits or prefixes are consumed. The ternary discriminator
therefore reduces format-selection pressure while preserving a single trit of
format overhead.

### Opcode and Register Fields

In one embodiment, the opcode field is four trits wide. A four-trit balanced
ternary field encodes enough operation classes to reserve base arithmetic,
control flow, conversion, scalar lane operations, vector numeric operations,
accumulator operations, artificial-intelligence operations, and future
gather/scatter or permutation operations.

In one embodiment, register fields are three trits wide. A three-trit field
encodes the scalar register file or vector register file identifiers used by
the virtual machine or processor implementation. In software embodiments, the
field value is decoded with a constant offset so that balanced field values map
to unsigned register indices.

### Function Fields and Width Suffixes

In one embodiment, typed instruction widths are not represented by separate
opcodes. Instead, an assembler suffix, such as `.t10`, `.t20`, `.t40`, `.t50`,
`.l10`, `.l20`, `.l40`, or `.l50`, lowers into a function field. Bare scalar
mnemonics may default to a compatibility width, such as T50, while explicit
suffixes select narrower or lane-oriented widths.

This preserves opcode space because `add.t10`, `add.t20`, `add.t40`, and
`add.t50` share one opcode and differ by function field. Similarly, vector or
lane variants can share operation families while preserving explicit width
semantics.

### R5 Overlay for Five-Register Instructions

In one embodiment, a particular opcode causes the R-type decoder to reinterpret
the normal R-type field payload as an R5 overlay. The R5 overlay contains five
three-trit register fields:

- destination register;
- condition or predicate register;
- negative-arm source register;
- zero-arm source register; and
- positive-arm source register.

The R5 overlay is useful for ternary select operations because a single
balanced ternary predicate chooses among three source registers. A conventional
three-register R-type layout cannot name all operands. The R5 overlay permits
the instruction to remain one word long by using reserved R-type trits rather
than an extension word.

### R5 Width Encoding for Vector Select

In one embodiment, vector select uses the same R5 register fields and stores a
width function value in part of the reserved R5 field. The width function value
selects the lane or numeric width for the vector operation. The remaining
reserved trits may be kept zero, used for future sub-operation variants, or
reserved for validation.

### Vector Memory Overlay

In one embodiment, vector load and vector store instructions use an I-type
overlay. The overlay stores:

- a vector register field;
- a scalar base register field;
- a width or function field; and
- a signed immediate displacement.

This allows a vector memory operation to encode both address calculation and
typed vector width in one word. The overlay is selected by opcode, not by a new
format discriminator.

### Malformed Instruction Handling

In one embodiment, decoding validates the format discriminator, opcode range,
register field ranges, reserved field constraints, and instruction-specific
field rules. A malformed instruction may be marked in a decoded instruction
structure and may cause an illegal-operation trap during execution.

For R5 and vector-memory overlays, reserved trits may be required to be zero or
may be interpreted only for selected opcodes. Unsupported opcodes using
reserved overlay space may trap or decode as malformed.

### Assembler Lowering

In one embodiment, a two-pass assembler parses mnemonics and suffixes, validates
register classes, resolves labels, and emits `TritWord27` instructions.
Scalar mnemonics use scalar register operands. Vector mnemonics use vector
register operands. Lane and numeric suffixes are validated against the opcode
family before being encoded into function fields.

In one embodiment, a bare arithmetic mnemonic defaults to a compatibility
numeric width, while explicit suffixes encode a different width. Invalid
suffixes on instructions without width semantics are rejected by the assembler.

### Disassembler Support

In one embodiment, a disassembler reverses the field decoding and prints
mnemonics, suffixes, scalar register operands, vector register operands,
immediates, and branch offsets. R5 overlays are printed in their five-register
shape, while vector-memory overlays are printed with a vector register, scalar
base register, suffix, and displacement.

## Example Encoding Operations

### R-Type Arithmetic

```asm
add.t20 r3, r1, r2
```

One embodiment encodes the instruction as:

```text
fmt    = +1
opcode = ADD
Rd     = r3
Rs1    = r1
Rs2    = r2
func   = T20
```

### R5 Ternary Select

```asm
tsel r6, r3, r1, r2, r4
```

One embodiment encodes the instruction as:

```text
fmt    = +1
opcode = TSEL
Rd     = r6
rCond  = r3
rNeg   = r1
rZero  = r2
rPos   = r4
```

### R5 Vector Select

```asm
vsel.t20 v6, v3, v1, v2, v4
```

One embodiment encodes the instruction as:

```text
fmt    = +1
opcode = VSEL
vD     = v6
vCond  = v3
vNeg   = v1
vZero  = v2
vPos   = v4
func   = T20
```

### Vector Memory Overlay

```asm
vload.t20 v1, [r2 + 8]
```

One embodiment encodes the instruction as:

```text
fmt    = 0
opcode = VLOAD
vReg   = v1
rBase  = r2
func   = T20
imm13  = 8
```

## Implementation Evidence

The following implementation artifacts support the disclosed embodiments:

- `ternary_isa.h`
  - Defines `TritWord27`.
  - Defines R-type, I-type, B-type, R5, and vector-memory field positions.
  - Defines opcode values and width function constants.
  - Implements `InstructionWord::decode`.
  - Implements `InstructionWord::encodeR`, `encodeI`, `encodeB`, `encodeR5`,
    and vector-memory encoding.
  - Implements disassembly helpers.
- `ternary_asm.h`
  - Parses scalar and vector mnemonics.
  - Parses `.tN` and `.lN` suffixes.
  - Encodes R5 `TSEL`, R5 `VSEL`, and vector-memory instructions.
  - Rejects invalid suffixes and wrong register classes.
  - Performs two-pass label resolution for branch offsets.
- `ternary_vm.h`
  - Executes decoded opcodes using the decoded instruction fields.
  - Uses R5 fields for scalar and vector select execution.
  - Uses vector-memory overlay fields for vector load and store execution.
- `test_multiwidth_vm.cpp`
  - Tests R5 encode/decode roundtrips.
  - Tests assembler acceptance of R5 and vector-memory forms.
  - Tests vector operation roundtrips.
  - Tests disassembler output for R5 forms.
- `README.md`
  - Documents Phase 4 ISA extensions and vector execution behavior.

## Advantages

Embodiments of the disclosed architecture may provide one or more of the
following technical advantages:

- Selects among three base instruction formats using one ternary format trit.
- Preserves fixed-width instruction fetch.
- Avoids consuming opcode values for every numeric or lane width.
- Encodes five-register select operations in one instruction word.
- Avoids extension-word fetch for ternary select instructions.
- Encodes vector select width in reserved R5 space.
- Encodes vector memory width and displacement in one I-type overlay.
- Allows compact assembler suffixes while preserving explicit width semantics.
- Supports scalar, vector, lane, accumulator, and future extension operations
  without changing instruction word length.
- Maps naturally to VM decode tables, FPGA decoders, and ASIC instruction
  decoders.

## Example Claim Set

The following claims are draft examples for disclosure support. Final claim
language should be prepared by a registered patent practitioner.

### Claim 1: Independent Method Claim

1. A method for decoding a fixed-width balanced ternary instruction word in a
   processor or virtual processor, the method comprising:
   fetching a twenty-seven-trit instruction word;
   extracting a single balanced ternary format discriminator trit from the
   instruction word;
   selecting, according to whether the format discriminator trit is negative,
   zero, or positive, one of a branch format, an immediate format, or a
   register format;
   extracting an opcode field from the instruction word;
   when the selected format is the register format and the opcode indicates a
   five-register overlay, extracting five register fields from the instruction
   word; and
   issuing an operation using the extracted five register fields without
   fetching a second instruction word.

### Claim 2: System Claim

2. A computing system comprising:
   an instruction memory storing fixed-width twenty-seven-trit instruction
   words;
   a decoder configured to read a single balanced ternary format discriminator
   trit from each instruction word;
   a register-format decoder configured to decode either a three-register
   register instruction or an opcode-selected five-register overlay from the
   same register-format instruction word;
   an immediate-format decoder configured to decode either a scalar immediate
   instruction or an opcode-selected vector-memory overlay from the same
   immediate-format instruction word; and
   an execution unit configured to execute scalar or vector operations using
   width information encoded in a function field rather than in separate
   per-width opcodes.

### Claim 3: Computer-Readable Medium Claim

3. A non-transitory computer-readable medium storing instructions that, when
   executed by one or more processors, cause the one or more processors to:
   assemble a source program into fixed-width twenty-seven-trit instruction
   words;
   encode a ternary format discriminator trit for each instruction word;
   encode type or lane width suffixes into function fields;
   encode a five-register instruction using an opcode-selected R5 overlay; and
   reject a width suffix or register class that is invalid for an instruction
   opcode.

### Dependent Claim Concepts

4. The method of claim 1, wherein a positive discriminator trit selects the
   register format, a zero discriminator trit selects the immediate format, and
   a negative discriminator trit selects the branch format.

5. The method of claim 1, wherein the register format comprises a format field,
   an opcode field, a destination register field, two source register fields, a
   function field, and reserved trits.

6. The method of claim 1, wherein the five-register overlay comprises a
   destination register field, a condition register field, a negative-source
   register field, a zero-source register field, and a positive-source register
   field.

7. The method of claim 6, wherein the five-register overlay encodes a ternary
   select instruction.

8. The method of claim 6, wherein the five-register overlay encodes a vector
   select instruction.

9. The method of claim 8, wherein a function field in reserved overlay trits
   encodes a vector lane or numeric width.

10. The system of claim 2, wherein the immediate-format decoder decodes a
    vector memory overlay comprising a vector register field, a scalar base
    register field, a width function field, and a signed displacement.

11. The system of claim 2, wherein scalar arithmetic instructions having
    different numeric widths share one opcode and differ by the function field.

12. The system of claim 2, wherein lane operations having different lane widths
    share one opcode and differ by the function field.

13. The system of claim 2, wherein the decoder marks an instruction malformed
    when reserved overlay trits are nonzero for an opcode that does not define
    the reserved overlay trits.

14. The medium of claim 3, wherein a bare arithmetic mnemonic defaults to a
    compatibility numeric width and an explicit suffix overrides the default by
    setting the function field.

15. The medium of claim 3, wherein an assembler parses scalar registers and
    vector registers as separate register classes and rejects use of a scalar
    register where a vector register is required.

16. The method of claim 1, wherein field extraction is performed by
    two-bit-per-trit slicing of a packed instruction word.

17. The method of claim 1, wherein branch offsets are word-addressed offsets
    resolved by a two-pass assembler.

18. The system of claim 2, wherein the instruction word is mapped to an FPGA or
    ASIC decoder using the same field boundaries as a virtual-machine decoder.

19. The system of claim 2, further comprising a disassembler configured to
    print R5 instructions in a five-register operand form.

20. The method of claim 1, wherein the five-register overlay enables a
    three-source predicated select operation to be represented in a single
    instruction word.

## Abstract

A fixed-width balanced ternary instruction encoding uses a twenty-seven-trit
instruction word with a single balanced ternary format discriminator trit. The
format discriminator selects among register, immediate, and branch formats. A
register-format opcode may select an R5 overlay that decodes five register
fields from the same instruction word, enabling ternary select and vector
select operations without fetching a second instruction word. Width suffixes
for scalar, lane, and vector operations are encoded in function fields rather
than separate per-width opcodes. Vector memory instructions may use an
immediate-format overlay containing a vector register field, scalar base
register field, width function field, and signed displacement. The same field
boundaries support assembler emission, virtual-machine decode, disassembly,
and hardware decoder embodiments.

## Source-Code Exhibit Checklist

Attach or preserve copies of the following files with the provisional filing
materials:

- `ternary_isa.h`
- `ternary_asm.h`
- `ternary_vm.h`
- `ternary_vm_state.h`
- `test_multiwidth_vm.cpp`
- `README.md`

Recommended source excerpts to highlight:

- `TritWord27`.
- R-type, I-type, B-type field constants.
- R5 field constants.
- Vector-memory overlay field constants.
- `InstructionWord::decode`.
- `InstructionWord::encodeR5`.
- Vector-memory encoding.
- Opcode definitions for `TSEL`, `VSEL`, `VLOAD`, `VSTORE`, and `VLEN`.
- Function constants for `.tN` and `.lN` suffixes.
- Assembler parsing for suffixes and register classes.
- Tests for R5 encode/decode and vector-memory roundtrips.

## Filing Notes

- The filing should avoid claiming an instruction set abstractly. The stronger
  framing is a concrete fixed-width ternary instruction encoding and decode
  mechanism.
- The R5 overlay should be described as opcode-selected reuse of existing
  register-format payload space, not as a fourth base instruction format.
- The width suffix mechanism should be tied to opcode-space preservation.
- The 27-trit word should be tied to word-addressed instruction memory and
  field-slice decoding.
- The disclosure should mention FPGA and ASIC embodiments because the same
  field boundaries map directly to hardware decoder wires.
