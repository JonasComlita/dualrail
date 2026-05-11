# System and Method for Ternary Predicate Selection in Scalar and Vector Processors

## Filing-Support Draft

This document is a technical disclosure draft for a provisional patent
application. It is not legal advice and is not a substitute for review by a
registered patent practitioner. The purpose of this draft is to disclose the
invention with enough implementation detail, variations, and source-code support
to preserve later claim options.

## Title

System and Method for Ternary Predicate Selection in Scalar and Vector
Processors

## Field

The disclosure relates to processor instruction sets, virtual machines,
accelerators, vector execution units, graphics processors, field-programmable
gate arrays, application-specific integrated circuits, and typed register-file
architectures using balanced ternary predicate values.

## Background

Binary processors commonly implement conditional control flow using Boolean
predicates, condition-code flags, predicated moves, or branch instructions. A
binary predicate typically distinguishes only two cases. When a program must
route among three outcomes, such as less-than, equal-to, and greater-than, a
binary execution pipeline commonly performs multiple comparisons, multiple
conditional moves, or one or more conditional branches. Such sequences may
increase instruction count, consume branch prediction resources, and introduce
pipeline stalls or vector-lane divergence.

Balanced ternary computation represents a predicate as one of three values:
negative one, zero, and positive one. A comparison operation can therefore
naturally produce a complete three-way routing predicate in a single result.
However, without an instruction that directly consumes that ternary predicate,
the program must still lower the three-way route into multiple binary-style
operations.

There is therefore a need for a processor instruction and corresponding vector
operation that directly consumes a ternary predicate and selects from three
source operands in a single decoded instruction, while preserving operand type
tags and payloads without arithmetic conversion.

## Summary

The disclosed architecture provides a ternary select instruction family for
scalar and vector processors. A scalar instruction, referred to as `TSEL`, reads
a ternary predicate from a condition register and selects one of three source
registers. A vector instruction, referred to as `VSEL`, applies the same
ternary selection independently across vector lanes using a vector of ternary
predicate values.

In one embodiment, the scalar instruction has the assembly form:

```asm
tsel rD, rCond, rNeg, rZero, rPos
```

The instruction reads a selected trit of `rCond`, such as trit zero. If that
trit is negative, the processor copies the full value from `rNeg` to `rD`. If
that trit is zero, the processor copies the full value from `rZero` to `rD`. If
that trit is positive, the processor copies the full value from `rPos` to
`rD`. The copy is tag-preserving: the selected register value, including its
type tag and payload bits, is written without normalization, rounding,
coercion, reinterpretation, or arithmetic conversion.

In another embodiment, the vector instruction has the assembly form:

```asm
vsel.tN vD, vCond, vNeg, vZero, vPos
```

The instruction treats `vCond` as a vector of ternary predicate values. For
each vector lane, the corresponding condition lane selects the negative, zero,
or positive source lane from `vNeg`, `vZero`, or `vPos`. Invalid predicate lanes
may write a deterministic typed-zero result for that lane and set lane-local
fault metadata, while non-faulting lanes continue to complete.

In one embodiment, both scalar and vector select instructions are encoded in a
fixed-width 27-trit instruction word using an extended five-register R-type
layout. The layout reuses existing reserved trits in a register-register
instruction format and therefore avoids requiring a second instruction word to
name the fifth register operand.

## Technical Improvement and Prior-Art Distinction

The invention produces a measurable reduction in instructions and
pipeline-disrupting control events required to perform a three-way conditional
operation. The improvement is not merely that a ternary value exists. The
specific mechanism is a single decoded instruction that consumes a first-class
balanced ternary predicate value and selects among three register operands
without consulting a branch predictor, reading a binary flags register,
performing a secondary comparison, or coercing the selected value.

This distinguishes the mechanism from binary conditional moves, ARM-style
predication, x86 `CMOV`, AVX-512 mask registers, and conventional vector blend
instructions. Those mechanisms select between two alternatives or require
multiple Boolean masks to encode a three-way route. This also distinguishes the
mechanism from historical ternary arithmetic generally: the disclosed operation
ties a balanced ternary predicate, a five-register instruction layout,
tag-preserving register movement, and scalar/vector execution semantics into
one concrete processor operation.

## Definitions

- **Balanced ternary trit:** a digit having one of three values: `-1`, `0`, or
  `+1`.
- **T1 predicate:** a single-trit predicate value encoded as a typed ternary
  value.
- **Tagged value:** a register or memory value comprising at least a type or
  mode tag and a payload.
- **Tag-preserving selection:** copying a selected tagged value as a whole,
  without changing its tag, payload, numeric width, lane width, exponent,
  mantissa, or spare-state representation.
- **R5 layout:** an instruction field layout containing five register fields:
  destination, condition, negative source, zero source, and positive source.
- **Vector lane:** one independently selectable element within a vector
  register.

## Brief Description of the Drawings

### Figure 1: Scalar TSEL Data Path

```text
                  +----------------+
rCond.trit[0] --->| ternary decode |---- neg/zero/pos select
                  +----------------+
                          |
                          v
             +-------------------------+
rNeg  -----> |                         |
rZero -----> |        3:1 mux          | -----> rD
rPos  -----> |  selected tagged value  |
             +-------------------------+
```

The selected source register is copied to the destination as a complete tagged
value.

### Figure 2: Extended R5 Instruction Layout

```text
[ fmt:1 | opcode:4 | rd:3 | rCond:3 | rNeg:3 | rZero:3 | rPos:3 | reserved:7 ]
```

In one embodiment, `fmt` identifies a register-register instruction, `opcode`
identifies `TSEL` or `VSEL`, and the five 3-trit fields identify the destination
and four source registers.

### Figure 3: Vector VSEL Lane Operation

```text
lane i:

vCond[i] = -1  ->  vD[i] = vNeg[i]
vCond[i] =  0  ->  vD[i] = vZero[i]
vCond[i] = +1  ->  vD[i] = vPos[i]
```

Each lane is selected independently. A fault in one lane does not require a
trap or pipeline flush for all lanes.

### Figure 4: Prior Multi-Step Routing Compared With Ternary Select

```text
Prior binary-style route:
    compare
    branch or conditional move for first case
    branch or conditional move for second case
    fallthrough or third move

Disclosed ternary route:
    ternary compare produces {-1, 0, +1}
    TSEL or VSEL consumes the predicate directly
```

### Figure 5: Vector Fault Metadata

```text
lane fault state:
    fault_valid[i] = 0  -> no lane fault
    fault_valid[i] = 1  -> fault_class[i] identifies lane fault
```

The fault-class value is meaningful only when the corresponding valid bit or
trit indicates a lane-local fault.

## Detailed Description

### Scalar TSEL Instruction

In one embodiment, a scalar execution unit implements a ternary select
instruction having five register operands:

```asm
tsel rD, rCond, rNeg, rZero, rPos
```

The instruction performs the following steps:

1. Fetch a fixed-width instruction word.
2. Decode the instruction opcode as a ternary select operation.
3. Decode five register fields from the instruction word.
4. Read a ternary predicate from the condition register.
5. Select the negative source, zero source, or positive source according to the
   ternary predicate.
6. Write the selected complete register value to the destination register.

In one implementation, the predicate is read from trit zero of the condition
register. Other embodiments may read a different predicate trit or a configured
predicate field.

Example execution semantics:

```cpp
cond = trit0(rCond);
if (cond < 0) {
    rD = rNeg;
} else if (cond > 0) {
    rD = rPos;
} else {
    rD = rZero;
}
```

The above pseudocode describes the behavior and does not require implementation
using branches. In hardware embodiments, the behavior may be implemented as a
ternary-controlled three-input multiplexer on the register-file output path. In
software virtual-machine embodiments, the behavior may be implemented by
selecting an indexed source register and copying the selected tagged value to
the destination.

### Tag-Preserving Writeback

The scalar instruction performs data movement rather than arithmetic. The
selected value is copied as a complete tagged value. If the selected source is a
numeric value, the numeric value is not rounded, normalized, widened, narrowed,
or re-encoded. If the selected source is a lane-encoded value, the lane payload
is not interpreted as a numeric value. If the selected source contains an
invalid or spare-state payload, that payload may be copied according to the
architectural rules of the selected embodiment.

This tag-preserving behavior allows `TSEL` to select among values having any
supported numeric or lane mode without requiring a width suffix on the scalar
instruction. The instruction does not perform a conversion operation. Explicit
conversion, where needed, is performed by a separate conversion instruction.

### Ternary Predicate Production

In one embodiment, a ternary compare instruction produces a T1 predicate:

```asm
tcmp rCond, rA, rB
tsel rD, rCond, rLess, rEqual, rGreater
```

The compare instruction computes a sign-like result representing less-than,
equal-to, or greater-than. The ternary select instruction consumes the result
directly. This avoids lowering three-way routing through multiple binary
predicates or multiple branch instructions.

### Extended R5 Encoding

In one embodiment, the instruction set uses a 27-trit instruction word. A
register-register instruction normally contains a destination register, two
source registers, a function field, and reserved or pad trits. The ternary
select instruction reuses reserved trits to encode an extended five-register
layout:

```text
[fmt:1 | opcode:4 | rd:3 | rCond:3 | rNeg:3 | rZero:3 | rPos:3 | reserved:7]
```

The same fixed-width instruction word therefore names all five register
operands needed by `TSEL`. The instruction does not require an instruction-pair
encoding, a register-indirect operand list, or an implicit temporary register.

In one embodiment, `fmt` identifies the register-register instruction class and
the opcode itself signals that the decoder should interpret the remaining
fields using the R5 layout rather than the ordinary register-register layout.
This allows the processor to retain the existing instruction format
discriminant while adding a five-register instruction.

### Vector VSEL Instruction

In one embodiment, a vector instruction implements lane-wise ternary selection:

```asm
vsel.tN vD, vCond, vNeg, vZero, vPos
```

The `.tN` suffix identifies the numeric width or element mode selected by the
vector operation, such as T1, T5, T10, T20, T40, or T50. The vector condition
register contains T1 predicate lanes. For each lane:

1. The corresponding predicate lane is read.
2. The negative, zero, or positive source lane is selected.
3. The selected lane value is copied to the destination lane.

The vector form may use the same R5 register fields as the scalar form. In one
embodiment, part of the R5 reserved field stores a function or width selector
for the vector operation.

### Lane-Local Fault Handling

In one embodiment, a vector select instruction validates that the predicate lane
contains a valid T1 predicate. If a predicate lane is invalid, the destination
lane receives a deterministic typed-zero value and lane-local fault metadata is
written. The vector instruction continues executing other lanes.

Fault metadata may include:

```text
fault_valid[lane]
fault_class[lane]
```

The `fault_valid` entry indicates whether the lane encountered a fault. The
`fault_class` entry identifies the class of fault, such as invalid operation,
memory fault, or division by zero, and is meaningful when `fault_valid` is set.

This per-lane fault handling permits vector execution to continue despite a
fault in a subset of lanes. It also allows later code to use ternary selection
or vector selection to route faulted and non-faulted lanes.

### Hardware Embodiments

In an FPGA or ASIC embodiment, the scalar instruction can be implemented as a
three-input multiplexer controlled by the encoded ternary predicate. The vector
instruction can be implemented by replicating the scalar select cell across
lanes or by implementing a packed lane-selection datapath.

Because the predicate has three possible values, the selection maps directly to
three source arms. The processor does not need to synthesize the three-way
route from two Boolean predicates, two conditional moves, or multiple branch
instructions.

### GPU and SIMD Embodiments

In a GPU or SIMD embodiment, `VSEL` provides branch-divergence-free ternary lane
routing. A predicate vector drives per-lane source selection without requiring
lanes to diverge into separate branch paths. The source vectors may be numeric
vectors, lane-encoded vectors, or tagged vector values depending on the
implementation.

In one embodiment, a GPU kernel performs ternary vector selection by applying
the lane rule independently to each work item or packed lane. In another
embodiment, a SIMD implementation computes masks for negative, zero, and
positive predicate lanes and blends the three source vectors under those masks.

## Example Programs

### Scalar Three-Way Selection

```asm
tcmp r4, r1, r2
tsel r5, r4, r10, r11, r12
```

If `r1 < r2`, `r5` receives the complete tagged value in `r10`. If `r1 == r2`,
`r5` receives the complete tagged value in `r11`. If `r1 > r2`, `r5` receives
the complete tagged value in `r12`.

### Vector Three-Way Selection

```asm
vcmp.t20 v3, v1, v2
vsel.t20 v4, v3, v10, v11, v12
```

Each lane of `v3` selects the corresponding lane from `v10`, `v11`, or `v12`.

## Implementation Evidence

The following implementation artifacts support the disclosed embodiments:

- `ternary_isa.h`
  - Defines the extended R5 layout.
  - Defines `Opcode::TSEL` and `Opcode::VSEL`.
  - Decodes R5 fields when the opcode identifies `TSEL` or `VSEL`.
  - Encodes five-register instructions with `InstructionWord::encodeR5`.
- `ternary_asm.h`
  - Parses `tsel rD, rCond, rNeg, rZero, rPos`.
  - Parses `vsel.tN vD, vCond, vNeg, vZero, vPos`.
  - Validates scalar and vector register classes.
  - Emits R5 instruction words.
- `ternary_vm.h`
  - Executes scalar `TSEL` by reading trit zero of the condition register,
    selecting one of three source registers, and writing the selected tagged
    value unchanged to the destination.
  - Executes vector `VSEL` by applying ternary selection to each vector lane.
  - Sets vector lane-local faults for invalid predicate lanes.
- `ternary_vm_state.h`
  - Defines tagged values, numeric modes, lane modes, vector register state,
    and vector fault state.
- `test_multiwidth_vm.cpp`
  - Tests R5 decode/encode roundtrip for `TSEL`.
  - Tests assembler support for `TSEL`.
  - Tests tag-preserving negative, zero, and positive scalar arms.
  - Tests `VSEL.t20` lane-wise three-arm selection.
  - Tests vector fault behavior for invalid predicate lanes.

## Advantages

Embodiments of the disclosed architecture may provide one or more of the
following technical advantages:

- Reduced instruction count for three-way routing.
- Reduced reliance on branch prediction for less-than/equal/greater-than
  decision paths.
- Reduced vector branch divergence by converting three-way lane control into a
  single vector select operation.
- Tag-preserving selection without implicit conversion.
- Fixed-width encoding of a five-register instruction without requiring a
  second instruction word.
- Direct mapping to a scalar 3:1 multiplexer or replicated vector 3:1
  multiplexer in FPGA or ASIC hardware.
- Uniform scalar and vector predicate semantics using T1 predicate values.

## Example Claim Set

The following claims are draft examples for disclosure support. Final claim
language should be prepared by a registered patent practitioner.

### Claim 1: Independent Method Claim

1. A method for executing a ternary predicate selection instruction in a
   processor or virtual processor, the method comprising:
   fetching a fixed-width instruction word from an instruction memory;
   decoding, from the fixed-width instruction word, an opcode identifying a
   ternary select instruction;
   decoding, from the fixed-width instruction word, a destination register
   field, a condition register field, a negative-source register field, a
   zero-source register field, and a positive-source register field;
   reading a ternary predicate value from a register identified by the condition
   register field, the ternary predicate value having one of a negative value, a
   zero value, or a positive value;
   selecting a register identified by the negative-source register field when
   the ternary predicate value is negative, selecting a register identified by
   the zero-source register field when the ternary predicate value is zero, and
   selecting a register identified by the positive-source register field when
   the ternary predicate value is positive; and
   writing a complete tagged value from the selected register to a register
   identified by the destination register field without performing arithmetic
   conversion of the complete tagged value.

### Claim 2: System Claim

2. A computing system comprising:
   an instruction memory storing fixed-width instruction words;
   a decoder configured to decode an opcode and five register fields from a
   single fixed-width instruction word when the opcode identifies a ternary
   select operation;
   a register file storing tagged register values;
   a ternary predicate read path configured to read a predicate value having one
   of three states from a condition register;
   a three-source selection unit configured to select one of a negative source
   register, a zero source register, or a positive source register according to
   the predicate value; and
   a writeback path configured to copy a selected tagged register value to a
   destination register without changing a type tag or payload of the selected
   tagged register value.

### Claim 3: Computer-Readable Medium Claim

3. A non-transitory computer-readable medium storing instructions that, when
   executed by one or more processors, cause the one or more processors to:
   decode a ternary select instruction from a fixed-width instruction word;
   decode five register operands from the fixed-width instruction word;
   read a ternary predicate operand having one of negative, zero, or positive
   state;
   select one of three source operands according to the ternary predicate
   operand; and
   write the selected source operand to a destination operand while preserving a
   type tag and payload of the selected source operand.

### Dependent Claim Concepts

4. The method of claim 1, wherein the fixed-width instruction word comprises 27
   balanced ternary trits.

5. The method of claim 1, wherein the five register fields are encoded in an
   extended register-register layout comprising:

   ```text
   [fmt:1 | opcode:4 | rd:3 | rCond:3 | rNeg:3 | rZero:3 | rPos:3 | reserved:7]
   ```

6. The method of claim 1, wherein the opcode causes the decoder to interpret
   reserved pad trits of a register-register instruction as at least one
   additional source-register field.

7. The method of claim 1, wherein the ternary predicate value is produced by a
   ternary compare instruction.

8. The method of claim 1, wherein writing the complete tagged value comprises
   copying a mode tag and payload without normalization, rounding, widening,
   narrowing, or numeric reinterpretation.

9. The method of claim 1, wherein the processor is implemented as a virtual
   machine executing a fetch-decode-execute loop.

10. The system of claim 2, wherein the three-source selection unit is a
    three-input multiplexer controlled by the ternary predicate value.

11. The system of claim 2, wherein the ternary select operation is implemented
    in a field-programmable gate array or application-specific integrated
    circuit by a ternary-controlled multiplexer circuit.

12. The method of claim 1, further comprising executing a vector ternary select
    instruction wherein each lane of a predicate vector independently selects a
    corresponding lane from one of three source vector registers.

13. The method of claim 12, wherein the vector ternary select instruction
    encodes a width selector in a reserved field of the extended
    register-register layout.

14. The method of claim 12, wherein an invalid predicate lane writes a
    deterministic typed-zero result to a corresponding destination lane.

15. The method of claim 14, further comprising setting a lane-local fault-valid
    value and a lane-local fault-class value for the invalid predicate lane.

16. The method of claim 12, wherein non-faulting vector lanes complete
    execution without a processor-wide trap caused by a faulting vector lane.

17. The method of claim 12, wherein the vector ternary select instruction
    reduces branch divergence in a graphics processing unit by replacing a
    three-way branch sequence with lane-wise predicated selection.

18. The method of claim 1, wherein the ternary predicate value is read from a
    least-significant trit of the condition register.

19. The method of claim 1, wherein the selected source operand is a lane-encoded
    value and is copied without treating a backing binary integer as a numeric
    value.

20. The method of claim 1, wherein the selected source operand is a numeric
    balanced ternary floating point value and is copied without invoking a
    floating point arithmetic unit.

## Abstract

A processor, virtual machine, or accelerator executes a ternary predicate
selection instruction. A fixed-width instruction word encodes an opcode and five
register operands: destination, condition, negative source, zero source, and
positive source. The condition operand supplies a ternary predicate having one
of negative, zero, or positive state. The processor selects one of the three
source operands according to the predicate and writes the selected complete
tagged value to the destination without arithmetic conversion. A vector form
performs the same selection independently for vector lanes using a vector of
ternary predicates and may record lane-local faults for invalid predicate lanes
while allowing non-faulting lanes to complete.

## Source-Code Exhibit Checklist

Attach or preserve copies of the following files with the provisional filing
materials:

- `ternary_isa.h`
- `ternary_vm.h`
- `ternary_asm.h`
- `ternary_vm_state.h`
- `test_multiwidth_vm.cpp`
- `README.md`

Recommended source excerpts to highlight:

- R5 layout constants.
- `InstructionWord::decode()` R5 path.
- `InstructionWord::encodeR5()`.
- `Opcode::TSEL` and `Opcode::VSEL`.
- Assembler parsing for `tsel` and `vsel`.
- VM execution cases for `Opcode::TSEL` and `Opcode::VSEL`.
- Vector selection helper and vector fault-state definitions.
- Tests proving scalar tag preservation and vector lane selection.

## Filing Notes

- This disclosure should be filed before public release of benchmark results,
  pitch materials, white papers, or external demonstrations that describe the
  same mechanism.
- The provisional should disclose scalar, vector, VM, CPU, GPU, FPGA, and ASIC
  embodiments even if later claims focus on only a subset.
- Claims should avoid abstract conditional-expression language and emphasize
  the concrete decoder, register-file, instruction-word, mux, tag-preserving
  writeback, and vector-lane behavior.
- Later continuation or follow-on filings may claim benchmark-specific
  optimizations, compiler IR lowering to `TSEL`/`VSEL`, or transformer routing
  kernels using ternary predicate selection.
