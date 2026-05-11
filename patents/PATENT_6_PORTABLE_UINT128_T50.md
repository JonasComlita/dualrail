# System and Method for Portable 50-Trit Balanced Ternary Arithmetic Across Heterogeneous Backends

## Filing-Support Draft

This document is a technical disclosure draft for a provisional patent
application. It is not legal advice and is not a substitute for review by a
registered patent practitioner. The purpose of this draft is to disclose the
invention with enough implementation detail, variations, and source-code support
to preserve later claim options.

## Title

System and Method for Portable 50-Trit Balanced Ternary Arithmetic Across
Heterogeneous Backends

## Field

The disclosure relates to balanced ternary numeric representations, virtual
machines, portable wide-integer arithmetic, GPU and SYCL device compatibility,
field-programmable gate array models, application-specific integrated circuits,
and stable application binary interfaces for ternary floating point and
positional trit storage.

## Background

Balanced ternary numeric formats may require binary backing storage wider than
the number of information bits represented by the ternary value. A fifty-trit
ternary floating point value, for example, may be stored in a 128-bit binary
container to provide enough room for positional encoding, reserved sentinel
states, and arithmetic intermediates.

Many host compilers provide a non-standard `__int128` extension. However, that
extension is not a portable C++ type and may not be available or may not behave
uniformly across MSVC, CUDA device code, SYCL device code, FPGA synthesis tools,
and other accelerator toolchains. If a ternary virtual machine stores its core
numeric values directly in compiler-specific extended integer types, the
machine-state layout, file snapshots, GPU transport, and hardware mapping can
become platform-dependent.

There is therefore a need for portable 50-trit balanced ternary arithmetic that
can compile across CPU, CUDA device, SYCL device, and hardware-oriented
toolchains without making compiler-specific extended integer types part of the
machine-state ABI. There is also a need for fused small-divisor operations
because base-3 positional ternary pack and unpack operations repeatedly divide
by, multiply by, or take remainders modulo three.

## Summary

The disclosed architecture provides a portable arithmetic substrate for a
50-trit balanced ternary numeric format, referred to as `LongTriple` or `T50`.
The format stores packed base-3 positional trits, supports overflow and
underflow sentinel states, and is usable across host CPU code, CUDA/SYCL device
code, virtual-machine memory, and hardware-oriented models without changing the
public payload shape.

In the preferred embodiment, the T50 payload uses a stable 128-bit unsigned
storage primitive comprising two 64-bit limbs:

```cpp
struct UInt128 {
    uint64_t lo;
    uint64_t hi;
};
```

The public representation remains the two-limb layout on all platforms. Host
compiler extensions such as `unsigned __int128`, CPU carry/borrow intrinsics,
or other machine-specific operations may be used only inside operators or helper
functions. Such fast paths do not change the externally visible storage layout,
serialization shape, VM state shape, GPU transport shape, or hardware mapping.

In one embodiment, a signed companion type represents signed values as a sign
and an unsigned magnitude:

```cpp
struct Int128 {
    UInt128 magnitude;
    int8_t sign;
};
```

In one embodiment, the stable unsigned storage primitive is used as the backing
data field for a fifty-trit balanced ternary floating point type, referred to
as `LongTriple` or `T50`. The type stores its packed trits in base-3 positional
form using the two-limb `UInt128` storage. Pack and unpack operations operate by
repeated multiplication by powers of three, fused division/remainder by three,
and explicit sentinel handling for overflow and underflow states.

## Technical Improvement and Prior-Art Distinction

The invention improves heterogeneous execution by providing portable 50-trit
balanced ternary arithmetic without requiring the public VM state or device
payload representation to depend on compiler-specific 128-bit integer types.
The improvement is not the two-limb integer representation alone. The specific
mechanism is the combination of a 50-trit positional ternary format, stable
machine-state payload shape, cross-target portable arithmetic, fused base-3
pack/unpack operations, and optional internal fast paths that do not change the
ABI.

This distinguishes the mechanism from ordinary multi-precision integer
libraries, `__int128` compiler extensions, and generic big-integer arithmetic.
Those systems provide wide arithmetic, but they do not define a portable
backing substrate for a fixed 50-trit balanced ternary floating point format
that preserves VM snapshots, device transport, and hardware mapping while
supporting native ternary pack/unpack and arithmetic.

## Definitions

- **Two-limb 128-bit storage:** a storage object comprising a low 64-bit limb
  and a high 64-bit limb.
- **Stable ABI:** an externally visible layout that does not change between
  host, GPU, SYCL, FPGA-model, or other compilation targets.
- **Internal fast path:** an implementation detail inside an operator or helper
  that may use compiler extensions or CPU intrinsics while preserving the
  public storage layout.
- **Base-3 positional storage:** an encoding in which packed trits contribute
  according to powers of three.
- **Fused small division:** an operation that computes quotient and remainder
  by a small integer divisor in one pass.
- **T50 / LongTriple:** a fifty-trit ternary floating point format having a
  packed positional payload stored in a 128-bit container.

## Brief Description of the Drawings

### Figure 1: Preferred Two-Limb T50 Payload Layout

```text
+----------------------+----------------------+
| hi: 64 bits          | lo: 64 bits          |
+----------------------+----------------------+

public layout is identical on CPU, CUDA, SYCL, VM snapshots, and hardware maps
```

### Figure 2: Internal Fast Path Without ABI Change

```text
UInt128 {lo, hi}
      |
      +-- host GCC/Clang: temporary unsigned __int128 inside operator
      |
      +-- MSVC: carry/borrow intrinsics inside operator
      |
      +-- device/fallback: explicit limb arithmetic
      |
      v
UInt128 {lo, hi}
```

The input and output layout remain two limbs.

### Figure 3: T50 Positional Packing

```text
50 balanced trits -> encode each trit -> sum encoded_trit[i] * 3^i -> UInt128
```

### Figure 4: T50 Positional Unpacking

```text
UInt128 temp
for each trit:
    temp, raw = divMod3(temp)
    trit = raw - 1
```

The quotient and remainder are produced together.

### Figure 5: Signed Arithmetic Layer

```text
Int128:
    sign = -1, 0, or +1
    magnitude = UInt128

add/sub compare signs and magnitudes
mul/div combine signs and unsigned magnitudes
```

## Detailed Description

### Preferred Two-Limb Storage Embodiment

In one embodiment, a 128-bit unsigned storage primitive is defined as two
64-bit limbs:

```cpp
struct UInt128 {
    uint64_t lo;
    uint64_t hi;
};
```

The storage order, limb widths, and public fields remain fixed across
compilation targets. A machine state, data memory word, instruction memory
word, file snapshot, device transport buffer, or hardware model can therefore
depend on the same layout.

The architecture does not redefine `UInt128` to be a compiler-native
`unsigned __int128` on platforms that support that type. Instead, the compiler
extension is used only as a temporary internal implementation path when
available and safe.

### Arithmetic Operators

In one embodiment, the storage primitive supports:

- equality and comparison;
- addition and subtraction with carry or borrow;
- left and right shifts;
- multiplication modulo 128 bits;
- division and modulo;
- multiplication by a small integer;
- division by a small integer;
- fused quotient/remainder by a small integer;
- bit inspection and bit setting; and
- decimal string conversion for diagnostics.

On compilers that support native 128-bit integers in host code, selected
operators may convert the two limbs to a native temporary value, execute the
operation, and convert back to the stable two-limb representation. On MSVC x64,
selected operators may use carry/borrow intrinsics for limb addition or
subtraction. On device or fallback paths, operators use explicit limb
arithmetic.

### Fused Small-Divisor Operations

In one embodiment, the storage primitive provides a fused small-divisor
operation:

```cpp
UInt128 divModSmall(uint32_t divisor, uint32_t& remainder);
```

The operation computes both quotient and remainder in one pass. Specialized
helpers, such as division/remainder by three, use the same mechanism:

```cpp
UInt128 divMod3(uint32_t& remainder);
```

This is useful for base-3 positional ternary encodings because unpacking a
packed ternary value repeatedly divides by three and reads the remainder to
recover trits. Fusing quotient and remainder avoids performing separate full
division and modulo scans.

### UInt256 Product Support

In one embodiment, a four-limb `UInt256` structure is provided for full-width
products:

```cpp
struct UInt256 {
    uint64_t limb[4];
};
```

This allows tests, diagnostics, or selected algorithms to compute the full
product of two two-limb operands without changing the public `UInt128` storage
layout. The lower 128 bits can be used for wrapping arithmetic while the full
product can be inspected when necessary.

### Signed Int128 Layer

In one embodiment, signed arithmetic is represented by a sign and an unsigned
magnitude:

```cpp
struct Int128 {
    UInt128 magnitude;
    int8_t sign;
};
```

The sign is `-1`, `0`, or `+1`. Addition and subtraction compare magnitudes
when signs differ. Multiplication and division combine signs and operate on
unsigned magnitudes. This representation is suitable for balanced ternary
mantissa/exponent arithmetic and avoids reliance on a platform-specific signed
128-bit integer.

### LongTriple / T50 Storage

In one embodiment, a fifty-trit ternary floating point type uses `UInt128` as
its data field:

```cpp
struct LongTriple {
    UInt128 data;
};
```

The fifty trits may be split into mantissa and exponent trits. In one
embodiment, forty-one trits are used for mantissa and nine trits are used for
exponent. The packed representation uses base-3 positional encoding. Overflow
and underflow may be represented by reserved two-limb sentinel values.

Packing fifty balanced trits into storage may compute:

```text
result = sum((trit[i] + 1) * 3^i)
```

where each balanced trit belongs to `{-1, 0, +1}`. Unpacking repeatedly applies
fused division and remainder by three:

```text
raw = temp mod 3
temp = temp / 3
trit = raw - 1
```

### Power Table Construction

In one embodiment, a power-of-three table is constructed in `UInt128` storage.
The table stores `3^i` for trit positions used by the packed ternary format.
Power-table construction uses multiplication by the small integer three and
therefore remains portable across targets.

### VM State and Device Transport

In one embodiment, virtual-machine tagged values use the same two-limb
`UInt128` payload shape. The stable shape allows data memory, register files,
device transport buffers, snapshots, and hardware models to share a consistent
layout. Optional CUDA or SYCL allocators may allocate memory containing the same
payload structure without replacing the payload type.

### Internal Fast Paths

In one embodiment, internal host fast paths are conditionally compiled when a
compiler supports native 128-bit integers and the code is not being compiled
for CUDA or SYCL device execution. The native temporary value accelerates
selected operations while preserving the stable two-limb ABI. In another
embodiment, CPU carry/borrow intrinsics accelerate addition and subtraction.
In another embodiment, explicit limb fallback arithmetic is used for all
targets.

The public `UInt128` type remains the same in all cases.

## Example Operations

### Stable Addition

```cpp
UInt128 c = a + b;
```

The implementation may use a host-native temporary, a CPU carry intrinsic, or
manual limb carry. The result is still `UInt128 {lo, hi}`.

### T50 Unpack

```cpp
UInt128 temp = value.data;
for (int i = 0; i < 50; ++i) {
    uint32_t raw = 0;
    temp = temp.divMod3(raw);
    trit[i] = raw - 1;
}
```

### T50 Pack

```cpp
UInt128 result = 0;
for (int i = 0; i < 50; ++i) {
    result += pow3[i] * static_cast<uint32_t>(trit[i] + 1);
}
```

### Signed Magnitude Arithmetic

```cpp
Int128 result = Int128::fromMagnitude(sign, magnitude);
```

Signed arithmetic combines sign handling with unsigned two-limb magnitude
operations.

## Implementation Evidence

The following implementation artifacts support the disclosed embodiments:

- `ternary_uint128.h`
  - Defines stable `UInt128 { lo, hi }` storage.
  - Defines optional host-native internal fast paths.
  - Defines MSVC carry/borrow intrinsic paths.
  - Defines fallback limb arithmetic.
  - Defines `divModSmall`, `divMod3`, `UInt256`, and `Int128`.
- `ternary_math.h`
  - Defines `LongTriple` with `UInt128 data`.
  - Defines T50 overflow and underflow sentinel values using `UInt128`.
  - Builds a `UInt128` power-of-three table.
  - Packs and unpacks fifty trits through `UInt128` arithmetic.
- `ternary_native_ops.h`
  - Uses portable `Int128` and `UInt128` arithmetic for native ternary
    operations and conversions.
- `ternary_vm_state.h`
  - Stores tagged VM payloads in `UInt128`.
  - Uses the same payload shape for numeric values, lane values, register file,
    and memory words.
- `ternary_device_allocators.h`
  - Allocates VM memory without changing the `UInt128` payload shape.
- `test_multiwidth_vm.cpp`
  - Tests `UInt128` arithmetic against a host-native oracle when available.
  - Tests add, subtract, multiply, divide, modulo, shifts, small division, and
    `UInt256` full product behavior.
- `optimization_baseline.md`
  - Records before/after measurements for `UInt128` hot-path work, including
    `divModSmall`, pack, unpack, and VM dispatch effects.
- `README.md`
  - Documents replacement of `LongTriple::data` backing storage with `UInt128`.
  - Documents use of portable `Int128`/`UInt128` arithmetic.

## Advantages

Embodiments of the disclosed architecture may provide one or more of the
following technical advantages:

- Keeps VM state, memory snapshots, GPU transport, and hardware maps stable
  across platforms.
- Avoids making compiler-specific `__int128` part of the public ABI.
- Allows host-native or intrinsic acceleration without changing storage layout.
- Provides device-compatible fallback arithmetic for targets without compiler
  128-bit extensions.
- Accelerates base-3 positional unpacking through fused quotient/remainder by
  three.
- Supports fifty-trit ternary floating point storage using portable two-limb
  arithmetic.
- Provides signed wide arithmetic through sign/magnitude representation rather
  than platform-specific signed 128-bit types.
- Supports full product diagnostics or algorithms through a separate four-limb
  product structure.

## Example Claim Set

The following claims are draft examples for disclosure support. Final claim
language should be prepared by a registered patent practitioner.

### Claim 1: Independent Method Claim

1. A method for executing portable fifty-trit balanced ternary arithmetic in a
   processor or virtual processor, the method comprising:
   storing a fifty-trit balanced ternary numeric value as a packed base-3
   positional payload;
   preserving a fixed public payload layout for the packed base-3 positional
   payload across host, device, virtual-machine, snapshot, and hardware-model
   execution targets;
   compiling arithmetic operations for the fifty-trit balanced ternary numeric
   value without requiring a compiler-specific 128-bit integer type in the
   fixed public payload layout;
   packing trits into the fixed public payload layout using powers of three;
   unpacking trits from the fixed public payload layout using fused quotient
   and remainder operations by three; and
   permitting target-specific internal fast paths only when the fixed public
   payload layout is preserved.

### Claim 2: System Claim

2. A computing system comprising:
   a virtual-machine state including tagged values having a fixed public
   payload layout;
   a ternary floating point type storing fifty packed trits in the fixed public
   payload layout;
   a portable arithmetic library configured to perform addition, subtraction,
   comparison, shift, multiplication, division, and small-divisor
   quotient/remainder operations on the fixed public payload layout; and
   conditional internal fast paths configured to accelerate selected operations
   without changing the fixed public payload layout.

### Claim 3: Computer-Readable Medium Claim

3. A non-transitory computer-readable medium storing instructions that, when
   compiled for one or more execution targets, cause the one or more execution
   targets to:
   maintain a stable payload representation for fifty-trit balanced ternary
   values;
   use the stable payload representation as backing storage for a fifty-trit
   balanced ternary value;
   perform portable arithmetic when compiler-native 128-bit arithmetic is
   unavailable;
   use compiler-native or intrinsic arithmetic internally when available; and
   convert packed ternary trits to and from the stable payload representation
   without changing the external representation.

### Dependent Claim Concepts

4. The method of claim 1, wherein the fixed public payload layout comprises `lo` and
   `hi` fields, each field being 64 bits.

5. The method of claim 1, wherein the native temporary fast path uses a
   compiler-supported unsigned 128-bit integer only inside an operator or helper
   function.

6. The method of claim 1, wherein the intrinsic limb path uses a carry or borrow
   intrinsic for addition or subtraction.

7. The method of claim 1, wherein the fallback limb path computes carry or
   borrow by comparing low limbs.

8. The method of claim 1, further comprising computing a quotient and remainder
   by a small integer divisor in one pass.

9. The method of claim 8, wherein the small integer divisor is three.

10. The method of claim 1, wherein unpacking trits comprises repeatedly
    computing a quotient and remainder by three and mapping a remainder to a
    balanced ternary trit.

11. The method of claim 1, wherein packing trits comprises multiplying stored
    powers of three by encoded trit values and adding the products into the
    two-limb payload.

12. The method of claim 1, further comprising representing signed 128-bit
    intermediate values using a sign field and an unsigned two-limb magnitude.

13. The method of claim 12, wherein the sign field is a ternary sign value.

14. The system of claim 2, wherein the ternary floating point type uses
    forty-one mantissa trits and nine exponent trits.

15. The system of claim 2, wherein overflow and underflow are represented by
    reserved two-limb sentinel payloads.

16. The method of claim 1, further comprising generating a table of powers of
    three in the two-limb representation.

17. The method of claim 1, further comprising computing a four-limb full product
    of two two-limb operands.

18. The system of claim 2, wherein the same two-limb payload is stored in data
    memory, register files, and device transport buffers.

19. The system of claim 2, wherein a CUDA or SYCL allocation backend stores the
    same two-limb payload layout as a host allocation backend.

20. The method of claim 1, wherein the public storage layout remains unchanged
    between CPU, GPU, SYCL, virtual-machine snapshot, FPGA model, and
    application-specific integrated circuit embodiments.

## Abstract

A portable fifty-trit balanced ternary arithmetic substrate stores packed
base-3 positional values while preserving a fixed public payload layout across
host, GPU, SYCL, virtual-machine, snapshot, and hardware-oriented targets. In a
preferred embodiment, the payload is a stable two-limb 128-bit storage
primitive containing low and high 64-bit limbs. Arithmetic operators may use
internal compiler-native or intrinsic fast paths when available, or portable
limb fallback arithmetic otherwise, but all paths return the same public
payload layout. Packing uses powers of three represented in the payload format,
and unpacking uses fused quotient/remainder operations such as division by
three. A signed companion type represents sign and magnitude for portable wide
signed arithmetic.

## Source-Code Exhibit Checklist

Attach or preserve copies of the following files with the provisional filing
materials:

- `ternary_uint128.h`
- `ternary_math.h`
- `ternary_native_ops.h`
- `ternary_vm_state.h`
- `ternary_device_allocators.h`
- `test_multiwidth_vm.cpp`
- `optimization_baseline.md`
- `README.md`

Recommended source excerpts to highlight:

- `UInt128 { lo, hi }`.
- `fromNative()` and `toNative()` as internal fast-path helpers.
- Carry/borrow intrinsic branches.
- Fallback limb add/sub/shift/multiply/divide.
- `divModSmall()` and `divMod3()`.
- `UInt256` and `multiplyFull()`.
- `Int128` sign/magnitude representation.
- `LongTriple { UInt128 data }`.
- `LongTriple::initPowTable()`.
- `LongTriple::pack()` and `LongTriple::unpack()`.
- Tests comparing `UInt128` to a host-native oracle.

## Filing Notes

- This disclosure should not be filed as a standalone claim to a generic
  two-limb integer. The stronger framing is portable fifty-trit balanced
  ternary arithmetic across heterogeneous backends, with two-limb storage as
  the preferred embodiment.
- The two-limb `UInt128` material should be used as dependent claim support or
  implementation disclosure unless counsel chooses otherwise.
- The disclosure should emphasize why the public type is not redefined per
  platform: stable VM state, device transport, file snapshots, and hardware
  mapping.
- The fused small-divisor path should be disclosed because base-3 pack/unpack
  makes division and remainder by three a hot operation.
- Later follow-on filings may claim additional microarchitectural fast paths,
  SIMD batch wide-integer operations, or hardware implementations of the same
  two-limb ternary positional storage contract.
