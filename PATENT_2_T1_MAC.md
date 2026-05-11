# System and Method for Multiplier-Free Ternary Weight Multiply-Accumulate with Wider Accumulation

## Filing-Support Draft

This document is a technical disclosure draft for a provisional patent
application. It is not legal advice and is not a substitute for review by a
registered patent practitioner. The purpose of this draft is to disclose the
invention with enough implementation detail, variations, and source-code support
to preserve later claim options.

## Title

System and Method for Multiplier-Free Ternary Weight Multiply-Accumulate with
Wider Accumulation

## Field

The disclosure relates to processor instruction sets, virtual machines,
accelerators, neural-network inference engines, vector execution units,
graphics processors, field-programmable gate arrays, application-specific
integrated circuits, and arithmetic units for balanced ternary values.

## Background

Neural-network inference and signal-processing workloads commonly use
dot-product and multiply-accumulate operations. Binary processors and
accelerators typically perform such operations by multiplying an input operand
by a weight operand and adding the product to an accumulator. Even when weights
are quantized to very low precision, an instruction set that treats the operand
as an ordinary numeric value may still route execution through a multiplier,
through multiply-like integer logic, or through a software-emulated multiply
path.

Balanced ternary weights have three possible values: negative one, zero, and
positive one. A multiplication by such a weight does not require a general
multiplier. A positive weight means add the input, a negative weight means
subtract the input, and a zero weight means skip the input. When both operands
are single trits, the product is also a single trit: zero if either operand is
zero, positive if both nonzero operands have the same sign, and negative if the
nonzero operands have opposite signs.

There is therefore a need for an instruction-set and execution-unit mechanism
that exposes single-trit ternary weights as a first-class operand type and
executes dot-product or multiply-accumulate operations by conditional add,
conditional subtract, or skip paths, while accumulating into a wider precision
format.

## Summary

The disclosed architecture provides a multiplier-free ternary weight
multiply-accumulate instruction family. A vector dot instruction, referred to
as `VDOT.t1`, computes a dot product over single-trit operands and writes a
wider scalar result. A vector multiply-accumulate instruction, referred to as
`VMAC.t1`, computes a single-trit-weighted contribution and accumulates the
result into a wider accumulator. In one embodiment, the wider accumulator uses a
T50 or LongTriple precision format.

In one embodiment, the instruction set includes:

```asm
vdot.t1 rD, vA, vB
vmac.t1 vA, vB
aclr.t50
astore.t50 rD
```

The `.t1` suffix identifies the operands as single-trit ternary values. The
execution path checks each lane for a valid T1 value. A lane with a zero operand
contributes zero. A lane with two nonzero operands contributes positive one
when the signs match and negative one when the signs differ. The contributions
are added to a wider accumulator without invoking a general multiplier.

In another embodiment, one vector contains T1 weights and the other vector
contains wider numeric activations. Each T1 weight controls whether the
corresponding activation is added, subtracted, or skipped. A positive weight
routes the activation to an add path, a negative weight routes the activation
to a subtract path, and a zero weight suppresses the contribution.

## Definitions

- **T1 value:** a typed single-trit value having one of `-1`, `0`, or `+1`.
- **T1 weight:** a T1 value used as a neural-network or dot-product weight.
- **Multiplier-free execution:** execution in which a general-purpose
  multiplier circuit, binary multiply instruction, or floating point multiply
  unit is not invoked to apply the T1 weight.
- **Conditional add/subtract/skip:** a datapath in which a T1 weight selects an
  addition path, a subtraction path, or no contribution.
- **Wider accumulator:** an accumulator having greater precision or range than
  the T1 operands, such as a T50 or LongTriple accumulator.
- **Sign product:** the product rule for two T1 operands: zero if either input
  is zero, positive if nonzero signs match, and negative if nonzero signs
  differ.

## Brief Description of the Drawings

### Figure 1: T1 Weight Datapath

```text
               T1 weight
             {-1, 0, +1}
                  |
                  v
        +-------------------+
input --| add/sub/skip mux  |---- contribution ----+
        +-------------------+                      |
                                                   v
                                             wider accumulator
```

A positive weight selects add, a negative weight selects subtract, and a zero
weight selects no contribution.

### Figure 2: T1 x T1 Dot-Product Lane Rule

```text
a == 0 or b == 0       -> contribution =  0
a and b have same sign -> contribution = +1
a and b differ in sign -> contribution = -1
```

The nonzero sign combination may be implemented using sign equality or sign
exclusive-or logic rather than a multiplier.

### Figure 3: VDOT.t1 and VMAC.t1 Accumulation

```text
VDOT.t1:
    vector lanes -> T1 sign products -> T50 sum -> scalar destination

VMAC.t1:
    vector lanes -> T1 sign products -> T50 sum -> accumulator += sum
```

### Figure 4: Wider Accumulator Flow

```text
aclr.t50      accumulator = 0 in T50 precision
vmac.t1      accumulator = accumulator + dot(T1 lanes)
vmac.t1      accumulator = accumulator + dot(T1 lanes)
astore.t20   convert/store accumulator result to selected destination width
```

### Figure 5: Hardware Embodiment

```text
T1 decode -> zero detector -> sign combine -> add/sub enable -> accumulator adder
```

The multiplier block is bypassed or absent for the T1 MAC path.

## Detailed Description

### T1 Operand Semantics

The architecture treats T1 as a first-class operand width rather than as an
ordinary integer stored in a larger format. A T1 value is constrained to one
balanced ternary trit. The value may be represented numerically, lane-encoded,
or as part of a typed register value, depending on the embodiment.

For two T1 operands `a` and `b`, the product is:

```text
if a == 0 or b == 0: product = 0
else if sign(a) == sign(b): product = +1
else: product = -1
```

The operation is algebraic multiplication over the three single-trit values. It
is distinct from ternary minimum, ternary maximum, ternary AND, or other
lattice operations.

### VDOT.t1 Instruction

In one embodiment, a vector dot-product instruction has the assembly form:

```asm
vdot.t1 rD, vA, vB
```

The instruction reads corresponding lanes from vector registers `vA` and `vB`.
Each lane must contain a valid T1 value. The execution unit computes the T1
sign product for each lane and accumulates the lane contributions into a wider
sum. The wider sum is written to scalar register `rD` as a tagged value, such
as a T50 value.

The instruction may perform the following steps:

1. Fetch and decode a vector dot instruction having a T1 width selector.
2. Validate the vector source registers and operand width.
3. Initialize a wider sum.
4. For each lane, read the T1 operands.
5. Produce a contribution of `-1`, `0`, or `+1` using zero detection and sign
   comparison.
6. Add nonzero contributions to the wider sum.
7. Write the wider sum to the scalar destination register.

The instruction does not require a general binary or floating point multiplier
for the T1 product.

### VMAC.t1 Instruction

In one embodiment, a vector multiply-accumulate instruction has the assembly
form:

```asm
vmac.t1 vA, vB
```

The instruction computes a T1 dot-product contribution from the source vector
registers and adds the contribution to a persistent wider accumulator. The
accumulator may be cleared with `aclr`, loaded from a scalar source with
`aload`, updated with accumulator arithmetic instructions, and stored with
`astore`.

Example:

```asm
aclr.t50
vmac.t1 v0, v1
vmac.t1 v2, v3
astore.t50 r5
```

In this example, both `vmac.t1` instructions contribute to the same T50
accumulator. A narrower result may be produced later by storing or converting
the accumulator to a selected format.

### T1 Weight Applied to Wider Activations

In another embodiment, a first vector contains T1 weights and a second vector
contains activations represented in a wider numeric format, such as T10, T20,
T40, or T50. For each lane:

```text
weight = +1 -> accumulator += activation
weight =  0 -> accumulator unchanged
weight = -1 -> accumulator -= activation
```

This embodiment applies ternary neural-network weights without multiplying the
activation by the weight. The T1 weight selects an add, subtract, or skip path.
The accumulator remains wider than the weight representation, preserving range
for dot products, matrix multiplication, convolution, attention projections, or
other neural-network layers.

### Accumulator Precision

In one embodiment, the accumulator stores a T50 or LongTriple value. This is a
wider format than the T1 operands. The architecture therefore separates operand
precision from accumulation precision. This is useful for workloads in which
many low-precision products are summed into a higher-precision result.

In other embodiments, the accumulator may be T20, T40, fixed-point, integer, or
another format selected by an instruction suffix or execution mode. The
preferred embodiment uses T50 accumulation for precision reference behavior.

### Fault Handling

In one embodiment, each vector lane is validated before contributing to the
dot-product or multiply-accumulate operation. If either operand for a lane is
not a valid T1 value, the lane contribution is suppressed and a lane-local
fault indicator is set. Non-faulting lanes continue contributing to the result.

In another embodiment, any invalid T1 source operand causes a processor-wide
trap. In a vector embodiment, lane-local fault-valid and fault-class masks are
preferred so that the vector operation can complete for valid lanes.

### Hardware Embodiments

In an FPGA or ASIC embodiment, the T1 MAC datapath includes:

- a T1 decoder;
- a zero detector;
- sign-combine logic for nonzero inputs;
- add-enable and subtract-enable signals; and
- a wider accumulator adder.

The datapath does not require a general multiplier for the T1 weight path.
Where the processor also contains a multiplier for other instructions, the T1
MAC instruction bypasses that multiplier. In a specialized accelerator, the T1
MAC array may omit multiplier cells entirely and replicate conditional
add/subtract/skip cells across lanes.

### GPU and SIMD Embodiments

In a GPU embodiment, a kernel may map each lane or work item to a T1
contribution. The kernel may use packed T1 lane encodings and convert each
weight into an add, subtract, or skip operation. Warp or wavefront lanes whose
weights are zero need not perform a multiply. In a SIMD embodiment, packed
predicate masks may identify positive, negative, and zero weight lanes, and the
execution path may add positive lanes, subtract negative lanes, and ignore zero
lanes.

### Neural-Network Embodiments

The disclosed instruction family is suitable for neural-network inference with
ternary weights. A matrix-vector multiply, matrix-matrix multiply, convolution,
or attention projection may store weights as T1 values. The computation applies
each weight through conditional addition, conditional subtraction, or skipping,
and accumulates into a wider result. The architecture therefore exposes the
weight type to the instruction set instead of treating quantized weights as
ordinary binary integers.

## Example Programs

### Dot Product of T1 Vectors

```asm
vdot.t1 r1, v0, v1
halt
```

The result in `r1` is a wider scalar dot product over T1 lanes.

### Accumulating Multiple T1 Dot Products

```asm
aclr.t50
vmac.t1 v0, v1
vmac.t1 v2, v3
astore.t50 r5
halt
```

The accumulator receives the sum of multiple T1 dot products and stores the
final result in T50 format.

### Activation to T1 Predicate

```asm
vact.t1 v2, v3
```

In one embodiment, a vector activation instruction maps numeric signs to T1
lanes: negative to `-1`, zero to `0`, and positive to `+1`. The resulting T1
lanes may be used by later T1 dot-product or MAC instructions.

## Implementation Evidence

The following implementation artifacts support the disclosed embodiments:

- `ternary_isa.h`
  - Defines accumulator and AI opcodes including `VDOT`, `VMAC`, and `VACT`.
  - Defines width suffix handling that restricts `VDOT` and `VMAC` to `.t1`.
- `ternary_asm.h`
  - Parses `vdot.t1`, `vmac.t1`, `vact.t1`, `aclr`, `aload`, `aadd`, `asub`,
    `amul`, and `astore`.
  - Rejects non-T1 suffixes for T1-specific vector AI operations.
- `ternary_vm.h`
  - Implements T1 sign-product logic.
  - Computes vector T1 dot products.
  - Implements `VDOT.t1` by writing a wider scalar dot result.
  - Implements `VMAC.t1` by adding the dot result into a T50 accumulator.
  - Implements `VACT.t1` by mapping numeric sign to T1 predicate lanes.
- `ternary_vm_state.h`
  - Defines tagged values, vector registers, vector fault state, and the
    accumulator field.
- `ternary_native_ops.h`
  - Defines T1 numeric operations and conversions used by the VM.
- `test_multiwidth_vm.cpp`
  - Tests assembler acceptance and rejection for T1 AI opcodes.
  - Tests that `VDOT.t1` writes a T50 dot product to a scalar register.
  - Tests that `VMAC.t1` accumulates a T1 dot product into the accumulator.
  - Tests that `VACT.t1` writes sign predicates.

## Advantages

Embodiments of the disclosed architecture may provide one or more of the
following technical advantages:

- Avoids invoking a general multiplier for T1 weight application.
- Converts ternary weights directly into add, subtract, or skip control.
- Supports wider accumulation of low-precision ternary contributions.
- Reduces arithmetic hardware for T1 neural-network inference paths.
- Supports packed T1 vector operands and SIMD/GPU execution.
- Provides an explicit ISA-level distinction between T1 multiply and ternary
  lattice operations such as minimum or maximum.
- Allows zero weights to suppress datapath activity for the corresponding lane.
- Provides a direct path from T1 activation predicates to later dot-product or
  MAC operations.

## Example Claim Set

The following claims are draft examples for disclosure support. Final claim
language should be prepared by a registered patent practitioner.

### Claim 1: Independent Method Claim

1. A method for executing a multiplier-free ternary multiply-accumulate
   operation in a processor or virtual processor, the method comprising:
   fetching an instruction identifying a single-trit ternary
   multiply-accumulate operation;
   decoding a first vector operand comprising single-trit ternary weights, each
   single-trit ternary weight having one of a negative value, a zero value, or a
   positive value;
   for each of a plurality of lanes, selecting an arithmetic action according
   to the corresponding single-trit ternary weight, wherein the arithmetic
   action is a subtract action for the negative value, a skip action for the
   zero value, and an add action for the positive value; and
   updating a wider accumulator according to the selected arithmetic actions
   without invoking a general multiplier for the single-trit ternary weights.

### Claim 2: System Claim

2. A computing system comprising:
   a decoder configured to decode an instruction identifying a T1 vector
   multiply-accumulate operation;
   a vector register file storing single-trit ternary operands;
   a T1 decode path configured to classify each single-trit ternary operand as
   negative, zero, or positive;
   a conditional add/subtract/skip execution path configured to generate lane
   contributions without using a general multiplier; and
   an accumulator configured to store a result in a precision wider than the
   single-trit ternary operands.

### Claim 3: Computer-Readable Medium Claim

3. A non-transitory computer-readable medium storing instructions that, when
   executed by one or more processors, cause the one or more processors to:
   decode a T1 dot-product or multiply-accumulate instruction;
   read a plurality of single-trit ternary operands from one or more vector
   registers;
   determine, for each lane, whether a contribution is negative, zero, or
   positive using zero detection and sign-combine logic;
   add nonzero contributions to a wider accumulator; and
   produce a tagged wider-precision result without performing a general
   multiply operation for the single-trit ternary operands.

### Dependent Claim Concepts

4. The method of claim 1, wherein the wider accumulator stores a T50 balanced
   ternary floating point value.

5. The method of claim 1, wherein the instruction is encoded with a `.t1` width
   suffix that restricts the multiply-accumulate operation to single-trit
   ternary operands.

6. The method of claim 1, wherein each lane contribution is produced by a sign
   product rule in which a zero input produces zero, matching nonzero signs
   produce positive one, and differing nonzero signs produce negative one.

7. The method of claim 1, wherein the processor includes a multiplier for other
   instructions and bypasses the multiplier for the single-trit ternary
   multiply-accumulate operation.

8. The method of claim 1, wherein the processor is a virtual machine executing
   a fetch-decode-execute loop.

9. The method of claim 1, wherein the first vector operand comprises T1 weights
   and a second vector operand comprises wider numeric activations, and wherein
   the corresponding T1 weight selects whether the activation is added,
   subtracted, or skipped.

10. The method of claim 1, wherein the operation is a vector dot-product
    instruction that writes a wider scalar result to a destination register.

11. The method of claim 1, wherein the operation is a vector
    multiply-accumulate instruction that adds a vector dot-product contribution
    to a persistent accumulator.

12. The method of claim 1, further comprising clearing the wider accumulator
    before a plurality of multiply-accumulate instructions and storing the
    wider accumulator to a scalar register after the plurality of
    multiply-accumulate instructions.

13. The method of claim 1, further comprising validating each lane as a valid
    T1 value before contribution to the wider accumulator.

14. The method of claim 13, further comprising setting a lane-local fault-valid
    value and a lane-local fault-class value for an invalid T1 lane.

15. The method of claim 1, wherein the single-trit ternary operands are stored
    in a two-bit-per-trit lane encoding.

16. The method of claim 1, wherein a zero T1 weight suppresses switching or
    arithmetic activity for a corresponding lane contribution.

17. The method of claim 1, wherein an activation instruction maps signs of
    numeric vector lanes to T1 predicate lanes for use by a later T1
    dot-product or multiply-accumulate instruction.

18. The system of claim 2, wherein the conditional add/subtract/skip execution
    path is implemented in a field-programmable gate array or
    application-specific integrated circuit without multiplier cells in a T1
    MAC lane.

19. The system of claim 2, wherein the conditional add/subtract/skip execution
    path is implemented by a graphics processor kernel using T1 predicate masks
    to select add, subtract, or no-op contributions.

20. The method of claim 1, wherein the operation is used to compute at least
    part of a neural-network inference layer selected from a matrix multiply,
    convolution, attention projection, or feed-forward projection.

## Abstract

A processor, virtual machine, or accelerator executes a multiplier-free
single-trit ternary multiply-accumulate instruction. T1 operands represent
values of negative one, zero, or positive one. For each vector lane, the T1
operand selects an arithmetic action: subtract, skip, or add. Contributions are
accumulated into a wider accumulator, such as a T50 balanced ternary value,
without invoking a general multiplier for the T1 weight path. A dot-product
form writes a wider scalar result, and a multiply-accumulate form adds the
result to a persistent accumulator. Invalid T1 lanes may set lane-local fault
metadata while non-faulting lanes continue execution.

## Source-Code Exhibit Checklist

Attach or preserve copies of the following files with the provisional filing
materials:

- `ternary_isa.h`
- `ternary_asm.h`
- `ternary_vm.h`
- `ternary_vm_state.h`
- `ternary_native_ops.h`
- `test_multiwidth_vm.cpp`
- `README.md`

Recommended source excerpts to highlight:

- `Opcode::VDOT`, `Opcode::VMAC`, and `Opcode::VACT`.
- Assembler suffix validation for `.t1` AI operations.
- `t1Product()` sign-product logic.
- `vectorDotT1()` accumulation path.
- VM execution cases for `VDOT` and `VMAC`.
- Accumulator state in `VMState`.
- Tests proving `VDOT.t1`, `VMAC.t1`, and `VACT.t1` behavior.

## Filing Notes

- The filing should distinguish T1 algebraic multiplication from ternary
  minimum, maximum, AND, OR, or other lattice operations.
- The strongest claim framing is not "quantized neural network weights" alone.
  It is the ISA-visible T1 operand type causing execution to route through
  conditional add, conditional subtract, or skip paths with wider accumulation.
- The provisional should disclose VM, CPU, SIMD, GPU, FPGA, and ASIC
  embodiments even if later claims focus on only a subset.
- Later follow-on filings may claim fused GPU kernels, compiler IR lowering to
  `vdot.t1` and `vmac.t1`, transformer-specific T1 attention or projection
  kernels, and hardware array layouts for replicated T1 MAC cells.
