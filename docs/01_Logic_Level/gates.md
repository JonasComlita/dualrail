# Logic Gates and Trit Backend

Source of truth: `ternary_backend.h` (namespace `sandbox::backend`)

---

## Overview

`ternary_backend.h` is the **lowest layer** of the stack — pure, device-safe trit helpers with no STL, no exceptions, no strings, and no virtual dispatch. It can be compiled for CPU, CUDA (`__host__ __device__`), SYCL, or FPGA simulation.

---

## Trit Pair Encoding (2-bit per trit)

All lane operations use 2-bit-per-trit encoding:

| 2-bit raw value | Balanced trit |
|-----------------|--------------|
| `0b00` (0)      | −1 (T_NEG)   |
| `0b01` (1)      |  0 (T_ZER)   |
| `0b10` (2)      | +1 (T_POS)   |
| `0b11` (3)      | **INVALID**  |

```cpp
// Encode balanced trit {-1, 0, +1} → 2-bit raw {0, 1, 2}
uint8_t encodeTritPair(int8_t trit);   // trit + 1

// Decode 2-bit raw → balanced trit
int8_t decodeTritPair(uint8_t raw);    // raw - 1

// Check if a 2-bit value is valid (not 0b11)
bool validTritPair(uint8_t raw);       // raw < 3

// Negate: flip sign by swapping 0 ↔ 2, leaving 1 unchanged
uint8_t negateTritPair(uint8_t raw);   // 2U - raw
```

---

## Ternary Full-Adder

The balanced ternary full-adder rule: if `sum > 1`, emit `sum−3, carry=+1`; if `sum < −1`, emit `sum+3, carry=−1`:

```cpp
int8_t normalizeTritSum(int& sum);     // clamps sum ∈ {-1,0,+1}, sets carry
```

Used by `addSubLane64` and `addSubLane128`.

---

## 64-bit Lane Operations

Operate on up to 32 trits packed in a `uint64_t` (2 bits per trit).

```cpp
// Get/set trit pair at position pos
uint8_t getPair64(uint64_t raw, int pos);
uint64_t setPair64(uint64_t raw, int pos, uint8_t pair);

// Validate: all trits valid, unused bits zero
bool validLane64(uint64_t raw, int trits);

// Create invalid sentinel (all positions = 0b11)
uint64_t invalidLane64(int trits);

// Negate all trits
uint64_t negLane64(uint64_t raw, int trits);

// Add two lanes (with balanced ternary carry propagation)
uint64_t addLane64(uint64_t a, uint64_t b, int trits);

// Subtract: a - b
uint64_t subLane64(uint64_t a, uint64_t b, int trits);

// Compare MST-first: returns -1, 0, or +1
int8_t compareLane64(uint64_t a, uint64_t b, int trits);

// Min/Max
uint64_t minLane64(uint64_t a, uint64_t b, int trits);
uint64_t maxLane64(uint64_t a, uint64_t b, int trits);
```

---

## 128-bit Lane Operations

Same API for `RawUInt128` (used for T40/T50 lanes):

```cpp
struct RawUInt128 { uint64_t lo; uint64_t hi; };

uint8_t getPair128(RawUInt128 raw, int pos);
RawUInt128 setPair128(RawUInt128 raw, int pos, uint8_t pair);
bool validLane128(RawUInt128 raw, int trits);
RawUInt128 invalidLane128(int trits);
RawUInt128 negLane128(RawUInt128 raw, int trits);
RawUInt128 addLane128(RawUInt128 a, RawUInt128 b, int trits);
RawUInt128 subLane128(RawUInt128 a, RawUInt128 b, int trits);
int8_t compareLane128(RawUInt128 a, RawUInt128 b, int trits);
RawUInt128 minLane128(RawUInt128 a, RawUInt128 b, int trits);
RawUInt128 maxLane128(RawUInt128 a, RawUInt128 b, int trits);
```

---

## Compilation Modes

| Macro | Effect |
|-------|--------|
| `__CUDACC__` or `__HIPCC__` | `TERNARY_HOST_DEVICE` = `__host__ __device__` |
| (none) | `TERNARY_HOST_DEVICE` = empty (CPU only) |
| `__GNUC__` or `__clang__` | `TERNARY_FORCE_INLINE` = `__attribute__((always_inline))` |

---

## Long Double Requirement

`ternary_math.h` has a compile-time assert that `long double > double`. This means:
- **MinGW-w64 GCC or Clang on x86**: 80-bit extended precision → OK
- **MSVC**: 64-bit (same as double) → build with `TERNARY_IGNORE_LONG_DOUBLE_ASSERT` or use GCC/Clang

This matters for transcendental operations (`exp`, `ln`, `sin`, `cos`) that use `long double` internally for precision.
