# System and Method for Lane-Local Ternary Fault Masks with Continued Vector Execution

## Filing-Support Draft

This document is a technical disclosure draft for a provisional patent
application. It is not legal advice and is not a substitute for review by a
registered patent practitioner. The purpose of this draft is to disclose the
invention with enough implementation detail, variations, and source-code support
to preserve later claim options.

## Title

System and Method for Lane-Local Ternary Fault Masks with Continued Vector
Execution

## Field

The disclosure relates to processor instruction sets, virtual machines, vector
execution units, GPU and SIMD execution, fault handling, typed registers,
balanced ternary predicates, and lane-local exception metadata.

## Background

Scalar processors commonly stop execution when an instruction encounters a
fault such as division by zero, invalid operand encoding, illegal operation, or
memory access failure. Vector processors introduce a more subtle problem:
different vector lanes can encounter different fault conditions during the same
instruction. If a single lane fault traps the whole processor, non-faulting
lanes lose useful work. If the processor records only a single global fault
code, the program cannot determine which lanes faulted and which lanes
completed.

Binary architectures often represent exception state with Boolean masks,
condition flags, or global status registers. Balanced ternary systems can
represent fault state more compactly and naturally by separating the question
"did a fault occur" from the question "which class of fault occurred." A
single ternary fault-class value may encode three fault classes, while a
separate validity value explicitly distinguishes no-fault state from a valid
fault classification.

There is therefore a need for a vector fault mechanism that records, for each
lane, whether a fault occurred and what class of fault occurred, while allowing
non-faulting lanes of the same vector instruction to complete execution and
write their results.

## Summary

The disclosed architecture provides a lane-local vector fault model using
separate fault-valid and fault-class state. Each vector lane has a
`fault_valid` entry and a `fault_class` entry. The fault class is meaningful
only when the corresponding fault-valid entry indicates that a fault occurred.

In one embodiment, vector instructions write a deterministic typed-zero result
to a destination lane when that lane faults. The instruction continues
executing other lanes. Non-faulting lanes write their normal results and leave
their fault-valid entries clear. A processor-wide trap is not required merely
because one lane faulted.

In one embodiment, the scalar trap register uses the same two-field model:

```text
trap_record.trit[0] = fault_valid
trap_record.trit[1] = fault_class
```

The no-fault state is explicit: `fault_valid` is zero. When `fault_valid` is
set, `fault_class` identifies the fault class, such as division by zero, memory
fault, or illegal operation.

## Technical Improvement and Prior-Art Distinction

The invention improves vector execution by isolating per-lane data faults while
preserving useful results from non-faulting lanes. The improvement is not merely
recording an exception flag. The specific mechanism is separated
fault-valid/fault-class state per lane, deterministic typed-zero writeback for
faulted lanes, and continued execution of valid lanes for structurally valid
vector instructions.

This distinguishes the mechanism from global exception flags, binary vector
masks, GPU per-thread error reporting, and ordinary exception handling. Those
mechanisms may record whether work should be masked or whether an exception
occurred, but they do not combine explicit per-lane fault occurrence,
ternary-coded fault class, typed destination zeroing, and continued vector
completion under the same instruction semantics.

## Definitions

- **Fault-valid value:** a value indicating whether a fault record or lane fault
  entry is active.
- **Fault-class value:** a ternary value identifying a class of fault.
- **Lane-local fault:** a fault that applies to one vector lane without
  necessarily trapping or invalidating all lanes of the instruction.
- **Typed-zero result:** a zero value written in the destination lane's expected
  type or mode.
- **Continued vector execution:** execution in which non-faulting lanes complete
  and write results despite one or more faulting lanes.
- **Trap register:** a scalar architectural or virtual register storing scalar
  fault state.

## Brief Description of the Drawings

### Figure 1: Scalar Fault Record

```text
T5 fault record:

trit[0] = fault_valid
trit[1] = fault_class
trit[2..] reserved or zero
```

The fault class is decoded only when `fault_valid` indicates a fault.

### Figure 2: Vector Fault Masks

```text
lane:          0     1     2     3     ...
fault_valid:  0     1     0     1
fault_class:  -     DIV   -     MEM
```

Only lanes with `fault_valid` set have meaningful fault-class entries.

### Figure 3: Continued Execution

```text
lane 0 valid      -> normal result
lane 1 wrong tag  -> typed zero + ILLEGAL_OP fault
lane 2 div zero   -> typed zero + DIV_ZERO fault
lane 3 valid      -> normal result
```

The vector instruction completes without flushing results from lanes 0 and 3.

### Figure 4: Typed-Zero Writeback

```text
faulting lane in vadd.t20 -> destination lane receives T20 zero
faulting lane in vcmp.t20 -> destination lane receives L1 zero predicate
faulting lane in vload.t5 -> destination lane receives T5 zero
```

### Figure 5: Ternary Fault Classes

```text
fault_class = -1 -> division by zero
fault_class =  0 -> memory fault
fault_class = +1 -> illegal operation
```

Other embodiments may assign different ternary fault classes or use additional
trits for additional fault detail.

## Detailed Description

### Scalar Two-Field Fault Record

In one embodiment, a scalar machine state includes a dedicated trap register.
The trap register stores a fault record having at least two ternary fields:

```text
fault_valid
fault_class
```

The `fault_valid` field indicates whether the record contains an active fault.
The `fault_class` field identifies the fault type when `fault_valid` is set.
This avoids overloading a single ternary value to mean both "no fault" and a
particular fault class.

Example scalar encoding:

```text
fault_valid = 0  -> no active fault
fault_valid = +1 -> fault_class is meaningful

fault_class = -1 -> division by zero
fault_class =  0 -> memory fault
fault_class = +1 -> illegal operation
```

The scalar processor may still enter a trapped status for scalar faults. The
two-field trap record nevertheless makes no-fault state explicit and provides a
model that extends directly to vector lanes.

### Vector Lane-Local Fault State

In one embodiment, a vector machine state contains two per-lane arrays:

```text
fault_valid[lane]
fault_class[lane]
```

Before a vector operation begins, the vector fault state may be cleared. During
the operation, each lane is validated independently. A faulting lane sets its
fault-valid entry and writes its fault-class entry. A non-faulting lane leaves
its fault-valid entry clear.

The vector fault state may be stored as arrays, bit masks, trit vectors, lane
registers, predicate registers, architectural status registers, or memory-mapped
fault buffers. In a balanced ternary embodiment, both fault-valid and
fault-class may be represented as T1 lane values.

### Typed-Zero Fault Result

In one embodiment, a vector lane that encounters a fault writes a deterministic
typed-zero value to the destination lane. The type or mode of the zero is the
expected destination mode for the operation.

Examples:

```text
vdiv.t20 fault lane  -> T20 zero
vadd.t5 wrong tag    -> T5 zero
vcmp.t40 fault lane  -> L1 zero predicate
vsel.t20 fault lane  -> T20 zero
```

This deterministic value prevents uninitialized or stale destination data from
being mistaken for a valid result. Because the fault-valid mask records the
fault, later code can distinguish a true numeric zero from a fault-produced
zero.

### Continued Vector Execution

In one embodiment, a vector instruction does not enter scalar trapped status
merely because a lane faults. The instruction continues executing remaining
lanes, and the machine may proceed to the next instruction after completing the
vector operation. A program can inspect the vector fault masks after the
instruction and decide whether to repair, retry, ignore, compact, branch, or
select around faulted lanes.

This is distinct from structural instruction faults. If the instruction itself
is malformed, uses a non-existent vector register, or violates an instruction
encoding rule, the processor may still raise a scalar trap. Lane-local faulting
applies when the instruction is structurally valid but individual lanes have
invalid data, divide-by-zero operands, out-of-range memory addresses, wrong
tags, or similar per-lane conditions.

### Fault Handling with Ternary Select

In one embodiment, the fault masks are compatible with ternary selection
instructions. A program can use a fault-valid mask to select replacement values
for faulted lanes and preserve normal values for valid lanes. A fault-class mask
can route fault recovery code among division-by-zero, memory-fault, and
illegal-operation handlers.

Because the fault class is ternary, the class value may directly drive
three-way routing or a vector select operation.

### Vector Arithmetic Example

For vector division:

```asm
vdiv.t20 v2, v0, v1
```

Each lane checks whether the divisor lane is zero. If the divisor is nonzero
and the operands are valid T20 values, the destination lane receives the
division result. If the divisor is zero, the destination lane receives T20 zero,
`fault_valid[lane]` is set, and `fault_class[lane]` is set to division by zero.
Other lanes continue.

### Vector Memory Example

For vector load:

```asm
vload.t20 v0, rBase, offset
```

Each lane computes or derives a memory address. Lanes whose addresses are valid
receive loaded values. Lanes whose addresses are out of range receive typed zero
and set memory-fault class. Valid lanes are not discarded because another lane
accessed invalid memory.

### Vector Tag-Validation Example

For vector add:

```asm
vadd.t20 v2, v0, v1
```

Each lane validates that both source lanes are numeric T20-compatible values.
If one lane contains a lane-family value or invalid payload, that lane writes
T20 zero and records an illegal-operation fault. Other lanes complete normally.

### Hardware Embodiments

In an FPGA or ASIC embodiment, each vector lane may have local valid/class
storage associated with the lane datapath. Faulting lanes assert a lane-local
valid signal and write a small fault-class code. Non-faulting lanes write
normal results. The vector unit does not need to flush all lanes for a local
fault.

In a GPU embodiment, each work item or lane may write to fault-valid and
fault-class buffers. The kernel can complete for all lanes and allow a later
kernel or host pass to process the fault metadata.

## Example Programs

### Vector Divide with Lane-Local Fault

```asm
vdiv.t20 v2, v0, v1
halt
```

If only lane 2 has a zero divisor, lane 2 receives T20 zero and records
division-by-zero. Other lanes receive valid quotient results.

### Vector Load with Memory Faults

```asm
vload.t20 v0, rBase, 0
halt
```

Lanes whose addresses are within data memory load normally. Lanes whose
addresses are out of range receive typed zero and record memory fault.

### Vector Select with Invalid Predicate Lane

```asm
vsel.t20 v7, v6, v4, v0, v5
halt
```

If one predicate lane in `v6` is not a valid T1 predicate, the corresponding
destination lane receives T20 zero and records illegal operation. Other lanes
select normally.

## Implementation Evidence

The following implementation artifacts support the disclosed embodiments:

- `ternary_vm_state.h`
  - Defines scalar trap records using `fault_valid` and `fault_class`.
  - Defines `encodeNoTrap`, `encodeTrap`, `trapValid`, and `decodeTrap`.
  - Defines `VectorFaultState` with `fault_valid` and `fault_class` arrays.
  - Stores vector fault state in `VMState`.
- `ternary_vm.h`
  - Clears vector fault state at the start of vector operations.
  - Implements `writeVectorFaultZero`.
  - Records divide-by-zero, memory, and illegal-operation faults per lane.
  - Allows valid lanes to write normal results while faulting lanes receive
    typed zero.
  - Uses scalar traps for structural instruction faults.
- `test_multiwidth_vm.cpp`
  - Tests that vector divide with one zero divisor still halts normally.
  - Tests that valid vector lanes survive wrong-tag neighbor lanes.
  - Tests that memory-fault lanes receive typed zero while valid memory lanes
    load normally.
  - Tests lane-local `fault_valid` and `fault_class` values.
  - Tests scalar trap-record valid/class behavior.
- `README.md`
  - Documents the scalar two-field fault model.
  - Documents vector per-lane fault-valid and fault-class masks.

## Advantages

Embodiments of the disclosed architecture may provide one or more of the
following technical advantages:

- Prevents a single lane fault from discarding useful work from other lanes.
- Makes no-fault state explicit rather than implied by a separate processor
  status value.
- Separates fault occurrence from fault classification.
- Allows ternary fault classes to drive three-way routing or selection.
- Provides deterministic typed-zero results for faulted lanes.
- Supports vector arithmetic, vector memory, vector select, gather, scatter, GPU
  kernels, and SIMD execution.
- Distinguishes lane-local data faults from scalar structural instruction
  faults.
- Provides a fault model suitable for FPGA, ASIC, GPU, and virtual-machine
  embodiments.

## Example Claim Set

The following claims are draft examples for disclosure support. Final claim
language should be prepared by a registered patent practitioner.

### Claim 1: Independent Method Claim

1. A method for executing a vector instruction in a processor or virtual
   processor, the method comprising:
   clearing or initializing a vector fault state comprising, for each vector
   lane, a fault-valid entry and a fault-class entry;
   executing the vector instruction independently for a plurality of lanes;
   detecting a fault condition in a first lane of the plurality of lanes;
   writing a deterministic typed-zero value to a destination lane corresponding
   to the first lane;
   setting the fault-valid entry for the first lane;
   writing a fault class to the fault-class entry for the first lane; and
   completing execution for a second lane of the plurality of lanes without
   trapping the processor solely because of the fault condition in the first
   lane.

### Claim 2: System Claim

2. A computing system comprising:
   a vector register file;
   a vector execution unit configured to execute vector operations lane by
   lane;
   a vector fault-valid store comprising one entry per vector lane;
   a vector fault-class store comprising one entry per vector lane;
   typed-zero generation logic configured to write a zero value in a
   destination mode for a faulting lane; and
   control logic configured to permit non-faulting lanes to write normal
   results while the fault-valid and fault-class stores record lane-local fault
   metadata for faulting lanes.

### Claim 3: Computer-Readable Medium Claim

3. A non-transitory computer-readable medium storing instructions that, when
   executed by one or more processors, cause the one or more processors to:
   maintain per-lane fault-valid metadata and per-lane fault-class metadata for
   vector operations;
   process lanes of a vector instruction independently;
   record a fault-valid value and a fault-class value for each faulting lane;
   write a typed-zero result for each faulting lane;
   write normal results for non-faulting lanes; and
   continue execution after the vector instruction when the instruction is
   structurally valid.

### Dependent Claim Concepts

4. The method of claim 1, wherein the fault-class entry is meaningful only when
   the corresponding fault-valid entry indicates that a fault occurred.

5. The method of claim 1, wherein the fault class is encoded as a balanced
   ternary value.

6. The method of claim 5, wherein the balanced ternary fault class identifies
   one of division by zero, memory fault, or illegal operation.

7. The method of claim 1, wherein the deterministic typed-zero value has the
   same numeric or lane mode as the expected destination lane.

8. The method of claim 1, wherein the vector instruction is a vector arithmetic
   instruction and the fault condition is division by zero.

9. The method of claim 1, wherein the vector instruction is a vector arithmetic
   instruction and the fault condition is an invalid operand tag.

10. The method of claim 1, wherein the vector instruction is a vector memory
    instruction and the fault condition is an out-of-range memory address.

11. The method of claim 1, wherein the vector instruction is a vector select
    instruction and the fault condition is an invalid ternary predicate lane.

12. The method of claim 1, wherein structural instruction faults cause a scalar
    trap while lane-local data faults are recorded in the vector fault state.

13. The method of claim 1, further comprising storing scalar fault state as a
    two-field record comprising scalar fault-valid and scalar fault-class
    fields.

14. The method of claim 13, wherein the scalar fault-valid field explicitly
    distinguishes a no-fault state from a valid fault-class value.

15. The method of claim 1, wherein the vector fault-valid entries are T1
    ternary predicate values.

16. The method of claim 1, wherein the vector fault-class entries are T1
    ternary values.

17. The method of claim 1, further comprising using a ternary select instruction
    to route, repair, or replace values according to at least one of the vector
    fault-valid entries or vector fault-class entries.

18. The system of claim 2, wherein the computing system is a graphics processor
    kernel implementation that writes fault-valid and fault-class buffers.

19. The system of claim 2, wherein the computing system is implemented in a
    field-programmable gate array or application-specific integrated circuit
    with lane-local fault registers.

20. The method of claim 1, wherein the vector length is implementation-defined
    and the vector fault state is resized or initialized according to the vector
    length.

## Abstract

A processor, virtual machine, or accelerator executes vector instructions with
lane-local fault handling. For each vector lane, the machine maintains a
fault-valid entry and a fault-class entry. A faulting lane writes a
deterministic typed-zero result, sets its fault-valid entry, and records its
fault class. Non-faulting lanes of the same vector instruction write normal
results and complete execution without a processor-wide trap caused solely by
the faulting lane. A scalar trap record may use the same separated
fault-valid/fault-class model, making no-fault state explicit. The mechanism
supports vector arithmetic, vector memory, vector select, GPU kernels, SIMD
execution, FPGA implementations, and ASIC implementations.

## Source-Code Exhibit Checklist

Attach or preserve copies of the following files with the provisional filing
materials:

- `ternary_vm_state.h`
- `ternary_vm.h`
- `test_multiwidth_vm.cpp`
- `README.md`

Recommended source excerpts to highlight:

- Scalar trap-record helpers: `makeFaultRecord`, `encodeNoTrap`,
  `encodeTrap`, `trapValid`, and `decodeTrap`.
- `VectorFaultState` with `fault_valid` and `fault_class`.
- `VMState::vector_faults`.
- `prepareVectorOp()`.
- `writeVectorFaultZero()`.
- Vector arithmetic, vector compare, vector select, vector load/store, gather,
  and scatter fault handling.
- Tests for `VDIV`, wrong-tag `VADD`, `VLOAD`, gather/scatter, and scalar
  trap-record behavior.

## Filing Notes

- The strongest claim framing is not "vector exceptions" generally. It is the
  separated per-lane fault-valid and fault-class state, typed-zero writeback,
  and continued execution of non-faulting lanes.
- The scalar two-field trap record should be disclosed because it shows the
  same fault-valid/fault-class model at the scalar level and avoids ambiguity
  around the no-fault state.
- The filing should disclose VM, CPU SIMD, GPU, FPGA, and ASIC embodiments even
  if later claims focus on only a subset.
- Later follow-on filings may claim compiler or IR transformations that lower
  fault recovery to `tsel` or `vsel` using the fault-valid and fault-class
  masks.
