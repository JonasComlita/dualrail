# Ternary Arithmetic

Source of truth: `ternary_native_ops.h`, `ternary_scalar.h`, `ternary_math.h`, and `tests/test_formats.cpp`.

---

## Numeric Families

The native arithmetic layer exposes two integer formats and four floating formats.

`UInt128` and its signed-magnitude `Int128` helper use checked public indices:
out-of-range bit positions are rejected without shifting or indexing storage.
Their division and modulo operations throw `std::domain_error` for a zero
divisor; zero is never returned as an ambiguous error value. VM and ternary
floating-point entry points continue to translate their own invalid divisions
into architectural traps or special values before reaching that host contract.

| Type | Role | Storage | Balanced range / layout |
|------|------|---------|-------------------------|
| `T1` | 1-trit integer | `uint8_t` | -1..+1, invalid sentinel `0xFF` |
| `T5` | 5-trit integer | `uint8_t` | -121..+121, invalid sentinel `0xFF` |
| `T10` | compact float | `uint16_t` | 6 mantissa trits, 4 exponent trits, 4 guard trits |
| `T20` | medium float | `uint32_t` | 14 mantissa trits, 6 exponent trits, 5 guard trits |
| `Triple` / `T40` | native VM word float | `uint64_t` | 33 mantissa trits, 7 exponent trits, 6 guard trits |
| `LongTriple` / `T50` | extended float | `UInt128` | 41 mantissa trits, 9 exponent trits, 14 guard trits |

`T10`, `T20`, `T40`, and `T50` are aliases for `TernaryScalar<N>`. The scalar storage is positional base-3: trit `i` is represented by `(data / 3^i) % 3 - 1`. Floating zero is represented by reserved raw storage value `0`; numeric `unpack()` and trit access expand it as neutral trits, while `unpackPositional()` remains available to raw storage consumers. Normalized non-zero values are packed from mantissa and exponent trits. Raw values in `[3^N, UNDERFLOW_DATA)` are invalid encodings rather than ordinary payloads.

The top storage values are reserved sentinels for floating formats:

| Sentinel | Meaning |
|----------|---------|
| `OVERFLOW_DATA` | Exponent overflow, invalid transcendental input, or special-input propagation |
| `UNDERFLOW_DATA` | Exponent below the representable range |

For integer formats, out-of-range construction or invalid input produces `INVALID_DATA`.

---

## Float Format Constants

`TernaryFloatFormat` defines the contract used by all native floating operations.

| Format | Total trits | Mantissa | Exponent | Exponent range | Guard | Product trits | Division scale |
|--------|-------------|----------|----------|----------------|-------|---------------|----------------|
| `T10` | 10 | 6 | 4 | -40..+40 | 4 | 16 | 5 |
| `T20` | 20 | 14 | 6 | -364..+364 | 5 | 33 | 13 |
| `T40` | 40 | 33 | 7 | -1093..+1093 | 6 | 72 | 32 |
| `T50` | 50 | 41 | 9 | -9841..+9841 | 14 | 96 | 40 |

The product width is `2 * mantissa_trits + guard_trits`. `tests/test_formats.cpp` checks these constants directly.

---

## Integer Operations

`T1` and `T5` use signed balanced integer conversion at operation boundaries:

- `intToSigned()` decodes each stored trit as `raw_digit - 1`.
- `intFromSigned()` re-encodes with balanced remainders and rejects values outside the format range.
- `add`, `subtract`, `multiply`, `negate`, and `abs` operate on signed host integers, then revalidate the result.
- `divide` uses integer division after a zero-divisor check. In C++ terms, this truncates toward zero.
- `sqrt` rejects negative input and uses integer square root on the magnitude.

Invalid input propagates to an invalid result.

---

## Rounding And Normalization

The arithmetic implementation is bridge-free: it does not decode through a binary floating-point value for core operations.

Important helpers:

- `balancedRem()` extracts a remainder in `{-1, 0, +1}`.
- `roundedDiv3()` divides an unsigned magnitude by 3 and increments the quotient when the remainder is 2.
- `roundedDivPow3Signed()` repeatedly applies rounded division by 3 when aligning exponents.
- `roundedDivide()` divides two unsigned magnitudes and increments the quotient when `2 * remainder >= denominator`.
- `packNormalized()` shifts magnitude down with rounded division when it exceeds the mantissa range, shifts it up when it is below the normalized minimum, then checks exponent limits.

The normal mantissa magnitude range is:

```text
mantissaMin = (3^(mantissa_trits - 1) - 1) / 2
mantissaMax = (3^mantissa_trits - 1) / 2
```

Zero magnitude remains the canonical raw zero value.

---

## Float Operations

### Add / Subtract

`floatAdd()` unpacks both operands, extracts integer mantissas and balanced exponents, normalizes both parts, aligns the operand with the smaller exponent by rounded division by powers of 3, then adds mantissas and repacks. `floatSubtract()` negates the second operand and calls `floatAdd()`.

Special input returns overflow. Zero is a fast path that returns the other operand.

### Multiply

`floatMultiply()` multiplies mantissas by trit convolution:

1. For every mantissa trit pair, accumulate `a[i] * b[j]` into product position `i + j`.
2. Propagate balanced carry with `balancedRem()`.
3. Derive the product sign and magnitude from the product trits.
4. Compute exponent as `ea + eb - (mantissa_trits - 1)`.
5. Pack through `packNormalized()`.

### Divide

`floatDivide()` rejects zero divisor as overflow, scales the numerator by `3^(mantissa_trits - 1)`, performs rounded magnitude division, uses exponent `ea - eb`, and repacks.

The VM adds a stronger architectural rule: scalar `DIV` and `TMOD` check for zero divisor before calling native arithmetic and raise `TRAP_DIV_ZERO`.

### Square Root

`floatSqrt()` rejects negative and special input as overflow. For valid positive input it:

1. Converts the mantissa to a magnitude.
2. Adjusts the scale exponent to be even.
3. Seeds from integer square root.
4. Runs up to 8 Newton iterations: `(x + t / x) / 2`.
5. Stops early when the packed value stops changing.

The VM traps `SQRT` of a negative value as `TRAP_ILLEGAL_OP`.

---

## Transcendentals

The native transcendental functions operate on `LongTriple`:

| Function | Implementation |
|----------|----------------|
| `exp()` | Handles negative input by reciprocal, scales large positive input down by powers of 3, evaluates 22 Taylor terms, then cubes back for each scale step |
| `ln()` | Rejects zero/negative input as overflow, normalizes into `[2/3, 3/2]`, evaluates an atanh-style series for 28 terms, and adds `k * ln(3)` |
| `ln3()` | Cached result from the same atanh-style series |
| `pi()` | Cached Machin-style arctangent computation |
| `sin()` / `cos()` | Reduce by `2*pi`, then evaluate 14 Taylor terms |

`ternary_math.h` exposes `Triple exp()` and `Triple ln()` by promoting to `LongTriple` and converting back to `T40`.

---

## VM Surface

The dispatcher in `ternary_vm.h` wraps this layer by width suffix:

- Numeric widths: `.t1`, `.t5`, `.t10`, `.t20`, `.t40`, `.t50`.
- Scalar arithmetic: `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `NEG`, `ABS`, `TCMP`, `TMIN`, `TMAX`, `TMOD`, `TLSHIFT`, `TRSHIFT`, `TCOUNT`, `TSCAN`, `TCLAMP`.
- Accumulator operations convert sources to `T40` internally: `ACLR`, `ALOAD`, `AADD`, `ASUB`, `AMUL`, `ASTORE`, `TMAC`.

If a native result is invalid, the VM writes no result and raises `TRAP_ILLEGAL_OP` through `writeChecked()`.

---

## Test Coverage

Relevant focused tests:

- `tests/test_uint128.cpp`: independent `__int128` comparisons where the compiler provides that oracle, including the portable Windows/MinGW implementation, boundary indices, and explicit divide-by-zero failures.
- `tests/test_formats.cpp`: format constants, exhaustive `T1`/`T5` integer round-trips and overflow, exact small float arithmetic, fractional alignment, square root tolerances.
- `tests/test_vm_widths.cpp`: width-suffixed VM arithmetic, scalar trap behavior, accumulator operations, trit count/scan, modulo and shift behavior.
- `TEST_MANIFEST.json` suite `core`: includes `test_native_ops`, `test_multiwidth_vm`, `test_ternary_lanes`, and numeric workload coverage.
