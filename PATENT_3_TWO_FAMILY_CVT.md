# System and Method for Separating Ternary Numeric and Lane Encodings with Explicit Conversion

## Filing-Support Draft

This document is a technical disclosure draft for a provisional patent
application. It is not legal advice and is not a substitute for review by a
registered patent practitioner. The purpose of this draft is to disclose the
invention with enough implementation detail, variations, and source-code support
to preserve later claim options.

## Title

System and Method for Separating Ternary Numeric and Lane Encodings with
Explicit Conversion

## Field

The disclosure relates to processor instruction sets, virtual machines,
typed register architectures, balanced ternary arithmetic, SIMD and GPU
transport formats, compiler lowering, and hardware accelerators using explicit
conversion boundaries between arithmetic representations and lane or wire
representations.

## Background

Balanced ternary values may be represented in more than one binary backing
format. One representation stores the value in a base-3 positional form suitable
for arithmetic. Another representation stores each trit as a fixed-width bit
pair suitable for lane-wise logic, SIMD transport, GPU kernels, FPGA wiring, or
instruction field extraction.

These two representations have different meanings. A base-3 positional backing
integer has arithmetic meaning: carries, normalization, mantissas, exponents,
and positional powers of three may be derived from the representation. A
two-bit-per-trit lane representation has wire or transport meaning: each trit
can be directly addressed as a pair of bits, but ordinary binary addition or
multiplication on the backing integer does not correspond to balanced ternary
arithmetic.

If a processor, virtual machine, compiler, or accelerator implicitly converts
between these representations, programs may accidentally perform arithmetic on
transport values, invoke unnecessary conversion passes, lose precision, or hide
data movement costs from optimization and profiling tools. There is therefore a
need for an architecture that separates ternary numeric encodings from ternary
lane encodings and permits crossing between the two families only through an
explicit conversion operation.

## Summary

The disclosed architecture provides two separate families of ternary
representations and an explicit conversion boundary between them.

The first family comprises numeric formats. Numeric values are stored in a
base-3 positional or numeric format and carry arithmetic meaning. Examples
include `T1`, `T5`, `T10`, `T20`, `T40`, and `T50`.

The second family comprises lane formats. Lane values are stored in a
two-bit-per-trit format and carry wire, SIMD, GPU, or transport meaning.
Examples include `L1`, `L5`, `L10`, `L20`, `L40`, and `L50`.

Crossing between the families occurs only through an explicit conversion
instruction. In one embodiment, the instruction is spelled with a source and
destination suffix:

```asm
cvt.t20.l20 rD, rS
cvt.l20.t20 rD, rS
cvt.t10.t20 rD, rS
```

The first suffix identifies the source type. The second suffix identifies the
destination type. Numeric-to-lane conversions pack numeric trits into a
two-bit-per-trit lane format. Lane-to-numeric conversions decode trit pairs and
reconstruct the corresponding numeric representation. Numeric-to-numeric
conversions perform width conversion, promotion, demotion, rounding, trapping,
or invalid-state handling according to the numeric formats involved.

The architecture rejects implicit numeric/lane crossings. Numeric arithmetic
instructions accept numeric modes. Lane/carryless logic instructions accept
lane modes. A memory copy, load, store, or tag-preserving select may preserve a
tagged value without conversion, but arithmetic and lane operations do not
reinterpret a backing integer as belonging to the other family.

## Definitions

- **Numeric family:** ternary formats whose payloads carry arithmetic meaning,
  including positional integer, floating point, mantissa/exponent, or other
  numeric representations.
- **Lane family:** ternary formats whose payloads store trits as lane-addressable
  encodings, such as two-bit-per-trit storage, for transport, SIMD, GPU, FPGA,
  wire, or instruction-field use.
- **Base-3 positional encoding:** a representation in which trit position `i`
  contributes according to a power of three.
- **Two-bit-per-trit encoding:** a representation in which each trit occupies a
  pair of bits, such as `00` for negative, `01` for zero, `10` for positive,
  and `11` for invalid or spare state.
- **Explicit conversion boundary:** an instruction or operation that names both
  the source and destination family, or otherwise explicitly names the desired
  destination, before a value crosses families.
- **Tagged value:** a register or memory value comprising a mode or family tag
  and a payload.
- **Matching-width boundary:** a numeric/lane conversion in which the source and
  destination contain the same number of trits, such as `T20` to `L20`.

## Brief Description of the Drawings

### Figure 1: Two Encoding Families

```text
Numeric family                         Lane family
--------------                         -----------
T1, T5, T10, T20, T40, T50             L1, L5, L10, L20, L40, L50
base-3 positional / numeric            2-bit-per-trit transport
arithmetic meaning                     wire/SIMD/GPU meaning
```

### Figure 2: Explicit Conversion Boundary

```text
T20 numeric value  -- cvt.t20.l20 -->  L20 lane value
L20 lane value     -- cvt.l20.t20 -->  T20 numeric value
T10 numeric value  -- cvt.t10.t20 -->  T20 numeric value
```

No hidden conversion path crosses the family boundary.

### Figure 3: Tagged Register Value

```text
+-------------+-----------------------------+
| mode / tag   | payload bits                |
+-------------+-----------------------------+
| T20          | base-3 positional numeric   |
| L20          | two-bit-per-trit lane       |
+-------------+-----------------------------+
```

The same payload container size may hold different meanings depending on the
mode tag.

### Figure 4: Instruction Family Separation

```text
Numeric arithmetic:
    add.t20, mul.t20, vadd.t20
    -> numeric ALU path

Lane/carryless logic:
    tladd.l20, tlneg.l20, tland.l20
    -> lane/wire logic path
```

### Figure 5: GPU/SIMD Boundary

```text
host numeric value -> explicit cvt/toLane -> raw lane payload -> GPU/SIMD kernel
GPU/SIMD result    -> explicit fromLane/cvt -> host numeric value
```

The transport representation is visible and does not silently replace numeric
arithmetic representation.

## Detailed Description

### Numeric Encoding Family

In one embodiment, the numeric family includes ternary integer and floating
point formats. Numeric values are stored in representations that support
arithmetic operations. Such representations may use base-3 positional storage,
mantissa/exponent fields, normalization, rounding, invalid states, overflow
states, or special values.

Example numeric modes include:

```text
T1   single-trit numeric predicate or integer
T5   five-trit numeric integer
T10  ten-trit ternary floating point
T20  twenty-trit ternary floating point
T40  forty-trit ternary floating point
T50  fifty-trit ternary floating point
```

Numeric arithmetic instructions operate on these modes. Examples include:

```asm
add.t20 r3, r1, r2
mul.t40 r3, r1, r2
vadd.t20 v3, v1, v2
```

The numeric ALU path may use native balanced ternary arithmetic, exponent
alignment, mantissa normalization, integer overflow detection, or other numeric
format rules.

### Lane Encoding Family

In one embodiment, the lane family includes fixed-width two-bit-per-trit
transport containers. Each trit is stored in a pair of bits. One encoding may
map negative, zero, and positive trits to three bit pairs and reserve the
remaining bit pair for invalid or spare state.

Example lane modes include:

```text
L1   one trit lane
L5   five trit lane
L10  ten trit lane
L20  twenty trit lane
L40  forty trit lane
L50  fifty trit lane
```

Lane operations operate on trit pairs or decoded lane trits. Examples include:

```asm
tladd.l20 r3, r1, r2
tlneg.l20 r4, r3
tland.l20 r5, r1, r2
```

The lane backing integer is not treated as a numeric ternary value. Binary
integer addition on the lane backing integer is not balanced ternary addition.
Lane operations are performed by trit-pair extraction, tritwise logic,
carryless trit operations, or explicit full-adder chains depending on the
operation.

### Tagged Values

In one embodiment, each register or memory word is represented as a tagged
value:

```text
{ mode, payload }
```

The `mode` identifies whether the payload is numeric or lane encoded and also
identifies the trit width. The same underlying binary payload size can carry
different meanings depending on the mode. For example, an `L20` payload and a
`T20` payload may both fit inside a binary container, but their payload bits are
not interchangeable.

Load, store, copy, and tag-preserving select operations may preserve the tag and
payload exactly. Arithmetic and lane operations validate the tag before
execution.

### Explicit Conversion Instruction

In one embodiment, an explicit conversion instruction converts values between
numeric widths or across numeric/lane families:

```asm
cvt.src.dst rD, rS
```

Examples:

```asm
cvt.t10.t20 rD, rS
cvt.t20.t10 rD, rS
cvt.t20.l20 rD, rS
cvt.l20.t20 rD, rS
```

The instruction names both source and destination when a family crossing is
performed. Numeric-to-lane conversions require the trit width to match the lane
width. Lane-to-numeric conversions likewise require a matching width before
optional numeric promotion or demotion in a separate conversion. Invalid
encodings may trap or produce an invalid destination value according to the
architecture.

In another embodiment, a destination-only conversion suffix is permitted for
numeric-to-numeric compatibility, while lane-family conversions require both
source and destination suffixes.

### Conversion Algorithm

In a numeric-to-lane conversion, the processor:

1. Validates that the source is a numeric value.
2. Converts or normalizes the source to the matching numeric width of the lane
   destination.
3. Extracts the source trits.
4. Encodes each trit into the lane bit-pair format.
5. Writes a tagged lane value to the destination.

In a lane-to-numeric conversion, the processor:

1. Validates that the source is a lane value.
2. Validates lane padding and rejects invalid trit-pair states.
3. Decodes each trit pair into a ternary trit.
4. Packs the decoded trits into the matching numeric format.
5. Optionally converts the matching numeric format to the requested destination
   numeric width.
6. Writes a tagged numeric value to the destination.

### Structural Rejection of Implicit Crossings

In one embodiment, the assembler rejects lane suffixes on numeric arithmetic
mnemonics and rejects numeric suffixes on lane logic mnemonics. For example:

```asm
add.l20    ; rejected
tladd.t20  ; rejected
```

The assembler also rejects mismatched numeric/lane conversions such as:

```asm
cvt.t20.l10
```

unless an embodiment explicitly defines a two-step conversion through a matching
width.

In one embodiment, the virtual machine or processor validates operand tags at
execution time. A numeric instruction receiving a lane-tagged operand traps or
sets a fault. A lane instruction receiving a numeric-tagged operand traps or
sets a fault. Vector instructions may use lane-local fault masks rather than
trapping the entire processor.

### Compiler and IR Embodiments

In one embodiment, a compiler intermediate representation carries typed values
corresponding to numeric modes and lane modes. The lowering pass emits explicit
conversion instructions whenever a value crosses the numeric/lane boundary. The
compiler does not insert hidden numeric/lane crossings during arithmetic
lowering.

This property makes conversion costs visible in assembly output, benchmark
profiles, and optimization passes. Conversion operations can therefore be
fused, moved, eliminated, or scheduled explicitly.

### GPU, SIMD, FPGA, and ASIC Embodiments

In a GPU or SIMD embodiment, lane values are used for dense transport and
tritwise logic kernels. Numeric values remain in numeric formats for arithmetic
unless explicitly converted. Kernel launch interfaces may accept raw lane
payloads, while host-side arithmetic uses numeric formats.

In an FPGA or ASIC embodiment, lane formats map directly to wire pairs, while
numeric formats route through arithmetic units. The explicit conversion
boundary corresponds to a physical decoder or encoder between the wire-oriented
lane datapath and the numeric ALU datapath.

## Example Programs

### Numeric-to-Lane and Back

```asm
mov.t20 r1, 7
cvt.t20.l20 r2, r1
cvt.l20.t20 r3, r2
halt
```

The first conversion packs the numeric T20 value into an L20 lane value. The
second conversion decodes the lane value back into a numeric T20 value.

### Rejected Family Mix

```asm
add.l20 r3, r1, r2     ; rejected: numeric add cannot use lane suffix
tladd.t20 r3, r1, r2   ; rejected: lane add cannot use numeric suffix
```

### Visible Conversion in a GPU Transport Path

```asm
cvt.t20.l20 r2, r1
; lane payload is now suitable for lane transport or lane logic
cvt.l20.t20 r3, r2
; numeric payload is restored for numeric arithmetic
```

## Implementation Evidence

The following implementation artifacts support the disclosed embodiments:

- `ternary_lanes.h`
  - Defines `TritLane1`, `TritLane5`, `TritLane10`, `TritLane20`,
    `TritLane40`, and `TritLane50`.
  - Stores lane payloads as two-bit-per-trit containers.
  - Keeps lane backing storage private.
  - Provides validity checks for invalid trit-pair states and padding.
- `ternary_vm_state.h`
  - Defines numeric modes and lane modes.
  - Defines tagged `TernaryValue` payloads.
  - Defines `isNumericMode`, `isLaneMode`, `matchingNumericMode`, and
    `matchingLaneMode`.
  - Implements `convertValue` for numeric-to-lane, lane-to-numeric, and
    numeric-to-numeric conversions.
- `ternary_asm.h`
  - Parses `cvt.src.dst`.
  - Rejects lane suffixes on numeric arithmetic mnemonics.
  - Rejects numeric suffixes on lane/carryless mnemonics.
  - Rejects mismatched numeric/lane conversion widths.
- `ternary_vm.h`
  - Executes `CVT`.
  - Validates source family when a source suffix is present.
  - Rejects invalid family crossings.
  - Rejects numeric operations on lane-tagged operands.
- `ternary_backend.h` and `ternary_kernel.h`
  - Provide device-safe raw lane helpers for GPU/SIMD-oriented lane payloads.
- `test_multiwidth_vm.cpp`
  - Tests `cvt.src.dst` assembly and VM execution.
  - Tests numeric-to-lane and lane-to-numeric round trips.
  - Tests rejection of `add.l20`, `tladd.t20`, and mismatched `cvt.t20.l10`.
  - Tests numeric operation traps when given lane input.
  - Tests invalid lane payload behavior.

## Advantages

Embodiments of the disclosed architecture may provide one or more of the
following technical advantages:

- Prevents accidental binary arithmetic on lane backing integers.
- Prevents hidden numeric/lane promotion or demotion in execution paths.
- Makes transport-format conversion visible to assemblers, compilers,
  profilers, and benchmarks.
- Separates numeric ALU datapaths from lane/wire logic datapaths.
- Allows dense two-bit-per-trit storage for SIMD/GPU/FPGA transport without
  changing numeric arithmetic semantics.
- Preserves tags through load, store, copy, and select operations.
- Enables compiler optimization of explicit conversion instructions.
- Provides a clean hardware boundary between wire-oriented lane encoders and
  arithmetic-oriented numeric units.

## Example Claim Set

The following claims are draft examples for disclosure support. Final claim
language should be prepared by a registered patent practitioner.

### Claim 1: Independent Method Claim

1. A method for executing ternary instructions in a processor or virtual
   processor, the method comprising:
   storing ternary values in a first family of tagged formats in which payloads
   have numeric arithmetic meaning;
   storing ternary values in a second family of tagged formats in which payloads
   have lane transport meaning and trits are encoded as bit pairs;
   rejecting execution of a numeric arithmetic instruction on a value tagged as
   belonging to the second family;
   rejecting execution of a lane logic instruction on a value tagged as
   belonging to the first family;
   decoding an explicit conversion instruction identifying a source format and
   a destination format; and
   converting a value between the first family and the second family only in
   response to the explicit conversion instruction.

### Claim 2: System Claim

2. A computing system comprising:
   a register file storing tagged ternary values;
   a numeric execution path configured to operate on numeric-family ternary
   values;
   a lane execution path configured to operate on lane-family ternary values
   whose trits are encoded as bit pairs;
   a decoder configured to reject mismatched numeric and lane instruction
   suffixes; and
   a conversion unit configured to convert between matching numeric-family and
   lane-family formats only when an instruction explicitly identifies a source
   format and a destination format.

### Claim 3: Computer-Readable Medium Claim

3. A non-transitory computer-readable medium storing instructions that, when
   executed by one or more processors, cause the one or more processors to:
   maintain a first set of ternary types for numeric arithmetic and a second set
   of ternary types for lane transport;
   preserve type tags for load, store, copy, and select operations;
   validate instruction operands against numeric or lane type tags;
   trap, fault, or reject operations that mix numeric and lane families without
   an explicit conversion; and
   perform an explicit conversion between a numeric representation and a
   two-bit-per-trit lane representation when a conversion instruction names the
   source and destination representations.

### Dependent Claim Concepts

4. The method of claim 1, wherein the first family comprises `T1`, `T5`, `T10`,
   `T20`, `T40`, and `T50`.

5. The method of claim 1, wherein the second family comprises `L1`, `L5`,
   `L10`, `L20`, `L40`, and `L50`.

6. The method of claim 1, wherein each lane-family trit is encoded in two bits
   and one of four possible bit-pair states is reserved as an invalid state.

7. The method of claim 1, wherein a lane-family object does not expose binary
   arithmetic operations on its backing integer.

8. The method of claim 1, wherein converting from a numeric-family format to a
   lane-family format requires the source and destination formats to contain the
   same number of trits.

9. The method of claim 1, wherein converting from a lane-family format to a
   numeric-family format comprises validating lane padding, rejecting invalid
   trit-pair states, decoding trit pairs, and packing decoded trits into a
   numeric representation.

10. The method of claim 1, wherein the explicit conversion instruction is
    spelled with source and destination suffixes.

11. The method of claim 10, wherein the explicit conversion instruction has the
    form `cvt.src.dst`.

12. The method of claim 1, wherein a numeric-to-numeric conversion is permitted
    by the conversion instruction and a numeric-to-lane conversion requires both
    source and destination suffixes.

13. The system of claim 2, wherein a virtual machine state stores each data word
    as a mode tag and a payload large enough for any supported numeric or lane
    format.

14. The system of claim 2, wherein load, store, copy, and ternary select
    instructions preserve the mode tag and payload without conversion.

15. The method of claim 1, wherein an assembler rejects a lane suffix on a
    numeric arithmetic mnemonic.

16. The method of claim 1, wherein an assembler rejects a numeric suffix on a
    lane logic mnemonic.

17. The method of claim 1, wherein vector numeric instructions operate on
    numeric-family modes and scalar lane instructions operate on lane-family
    modes.

18. The method of claim 1, wherein a compiler intermediate representation emits
    the explicit conversion instruction whenever a value crosses between the
    first family and the second family.

19. The system of claim 2, wherein a GPU or SIMD kernel receives lane-family
    payloads for tritwise operations and numeric-family values are converted to
    lane-family payloads before kernel dispatch.

20. The system of claim 2, wherein an FPGA or ASIC embodiment implements a
    physical encoder or decoder between a lane wire datapath and a numeric ALU
    datapath.

## Abstract

A processor, virtual machine, compiler, or accelerator maintains two distinct
families of ternary representations. Numeric-family values carry arithmetic
meaning, such as base-3 positional or floating point meaning. Lane-family values
carry transport or wire meaning, with trits encoded as bit pairs. Instructions
validate operand tags so numeric arithmetic is not executed on lane-family
payloads and lane logic is not executed on numeric-family payloads. Conversion
between the families occurs through an explicit conversion instruction naming a
source format and a destination format, such as `cvt.t20.l20` or
`cvt.l20.t20`. The architecture thereby prevents hidden family crossings,
preserves tags through data movement, and exposes conversion costs to
assemblers, compilers, profilers, GPU kernels, and hardware datapaths.

## Source-Code Exhibit Checklist

Attach or preserve copies of the following files with the provisional filing
materials:

- `ternary_lanes.h`
- `ternary_vm_state.h`
- `ternary_asm.h`
- `ternary_vm.h`
- `ternary_backend.h`
- `ternary_kernel.h`
- `test_multiwidth_vm.cpp`
- `README.md`

Recommended source excerpts to highlight:

- Lane class definitions and private backing storage.
- `TernaryMode` numeric and lane modes.
- `isNumericMode()` and `isLaneMode()`.
- `matchingNumericMode()` and `matchingLaneMode()`.
- `TernaryValue` tagged payload structure.
- `convertValue()` numeric/lane conversion logic.
- Assembler validation for `cvt.src.dst`.
- Tests rejecting `add.l20`, `tladd.t20`, and `cvt.t20.l10`.
- Tests proving numeric-to-lane and lane-to-numeric round trips.

## Filing Notes

- The filing should avoid claiming "using two encodings" abstractly. The
  stronger claim is the structural family separation plus explicit conversion
  instruction plus tag validation in the execution path.
- The lane family should be described as transport/wire/SIMD/GPU format, not as
  a replacement numeric representation.
- The numeric family should be described as the arithmetic representation, even
  when the underlying storage is a binary container.
- Later filings may claim compiler optimizations that move, fuse, or eliminate
  explicit `cvt.src.dst` instructions, and GPU kernels that consume lane-family
  payloads after an explicit conversion boundary.
