# Trit Encoding Schemes

There are **three distinct encoding schemes** in this codebase. Confusing them is a common bug.

---

## Scheme A — Positional Base-3 (`TernaryScalar<N>`)

**Used by:** All arithmetic, `ternary_scalar.h`, `ternary_math.h`, `ternary_native_ops.h`

Each N-trit value is stored as a single integer in `[0, 3^N)`.
The balanced trit `t` at position `i` contributes `(t + 1) × 3^i` to the integer.

| Stored digit | Balanced trit value |
|-------------|-------------------|
| `0`         | `−1` (T_NEG)      |
| `1`         | ` 0` (T_ZER)      |
| `2`         | `+1` (T_POS)      |

**Storage type by width:**

| Width | Trits | Type | Max value (`3^N`) |
|-------|-------|------|-------------------|
| T1    | 1     | `uint8_t`  | 3 |
| T5    | 5     | `uint8_t`  | 243 |
| T10   | 10    | `uint16_t` | 59,049 |
| T20   | 20    | `uint32_t` | ~3.49×10⁹ |
| T40   | 40    | `uint64_t` | ~1.22×10¹⁹ |
| T50   | 50    | `UInt128`  | ~7.18×10²³ |

**Key invariant:** `3^40 < 2^64`, so T40 (the native word size) fits exactly in one `uint64_t`. This is exploited for efficiency.

**Special sentinels** live at the top of the storage range (unreachable by `pack()`):
- `OVERFLOW_DATA` = `std::numeric_limits<Storage>::max()`
- `UNDERFLOW_DATA` = `std::numeric_limits<Storage>::max() - 1`

---

## Scheme B — 2-Bit-Per-Trit Lane Encoding (`TritLane<N>`)

**Used by:** SIMD/GPU transport, `ternary_lanes.h`, `ternary_backend.h`, `ternary_simd.h`, instruction words

Each trit occupies **2 bits**. Trit `i` is at bits `[2i+1 : 2i]`.

| Stored 2-bit value | Balanced trit |
|-------------------|--------------|
| `0b00`            | `−1` (T_NEG) |
| `0b01`            | ` 0` (T_ZER) |
| `0b10`            | `+1` (T_POS) |
| `0b11`            | **INVALID** → triggers `TRAP_ILLEGAL_OP` |

**Also used for instruction words (`TritWord27`):**
- 27 trits × 2 bits = 54 bits in a `uint64_t` (bits [63:54] always zero)
- Same encoding: `0b00`=−1, `0b01`=0, `0b10`=+1, `0b11`=INVALID

**Lane types:**

| Type       | Trits | Storage    |
|------------|-------|------------|
| `TritLane1`  | 1   | `uint8_t`  |
| `TritLane5`  | 5   | `uint16_t` |
| `TritLane10` | 10  | `uint32_t` |
| `TritLane20` | 20  | `uint64_t` |
| `TritLane40` | 40  | `UInt128`  |
| `TritLane50` | 50  | `UInt128`  |

**AVX2 acceleration** is available for `TritLane20` operations (neg, add, sub, compare).

---

## Scheme C — Dual-Rail Enum (`Trit` enum)

**Used by:** HAL abstraction layer (`kernel/hal.trit`, `ternary_scalar.h` `getTrit()`)

| `Trit` enum value | Meaning |
|------------------|---------|
| `Trit::Neutral`  = `0x0` | Balanced 0 |
| `Trit::Positive` = `0x1` | Balanced +1 |
| `Trit::Negative` = `0x2` | Balanced −1 |
| `Trit::Invalid`  = `0x3` | Error state |

This encoding is used as an **interface type** in hardware abstraction. Do NOT mix it with `int8_t` trit values or positional base-3 digits.

---

## Conversions Between Schemes

### Positional → Lane
Use `toLane()` / `laneFromPositional<N, Storage>(scalar)`:
```cpp
TritLane20 lane = toLane(myT20value);
```

### Lane → Positional
Use `fromLane()` / `laneToPositional<N, Storage>(lane)`:
```cpp
T20 scalar = fromLane(myTritLane20);
```

### Positional → signed integer
Use `native_ops::toLongLong(value)`:
```cpp
long long v = native_ops::toLongLong(myTriple);
```

### Signed integer → Positional
Use `native_ops::fromInt(value)`:
```cpp
LongTriple t = native_ops::fromInt(42LL);
```

---

## What NOT To Do

- ❌ Do NOT do binary integer arithmetic on `TritLane<N>` backing integers — they have no positional meaning.
- ❌ Do NOT confuse positional digit `0` (balanced −1) with trit value `0` (balanced 0).
- ❌ Do NOT store `int8_t` trit values {−1, 0, +1} where a positional digit {0, 1, 2} is expected, or vice versa.
- ❌ Do NOT use `getTritRaw()` for arithmetic — it returns positional digits (0/1/2), not balanced values (−1/0/+1).
