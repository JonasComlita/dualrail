# T40 numeric representation boundaries

T40 remains the native scalar and accumulator standard: 33 balanced mantissa
trits and 7 exponent trits. ISA v2, executable/function ABI v3, syscall ABI v2,
tBoot v3, and tDisk v2 remain the only supported platform versions. Explicit
T50 operations remain their existing separate feature; ordinary reductions do
not promote to T50.

## Arithmetic and representation

Production arithmetic in `ternary_native_ops.h` operates on native radix-3
mantissas and exponents. Host `double`/`long double` conversion helpers are
approximate interoperability boundaries. A wider binary accumulator can improve
accuracy independently of radix, so results obtained with those helpers are not
evidence of a ternary precision advantage.

`T40Accumulator` retains a decoded 33-trit mantissa in signed 64-bit host storage
and its exponent between additions/subtractions. The portable integer scratch
operations are the same ones used by ordinary native T40 arithmetic. It does
not retain extra numeric precision. Each update still aligns, rounds, and
normalizes exactly as the existing packed T40 operation does. Positional packing
is lazy and cached; `ASTORE`, context serialization, and disk checkpoints request
the packed value. A read-out does not reset the accumulator. `ACLR` clears it.
The host helper's `flush()` explicitly reads and resets.

`ALOAD` and operation suffixes select the input view; the accumulator stays T40.
`ASTORE` converts from T40 to the requested output view. `TMAC` retains its
existing multiply-then-add rounding; it is not a fused single-rounding operation.
`VMAC.T1` accumulates its existing dot result in the same T40 state.

The kernel/native scheduler stores the accumulator in the existing tagged word
at slot 5 of the 279-word v3 vector context. Host core copies preserve decoded
state; process restoration and disk checkpoint restoration rebuild it from the
packed T40 word. No hidden numeric state, new context slot, or new wire format is
needed. The assembler exposes the existing `vctxstore rContext [, offset]` and
`vctxload rContext [, offset]` escape encodings for conformance of these boundaries.

## Compiler and language

`radix3(coefficient, exponent)` represents `coefficient * 3^exponent` exactly in
T40. Both arguments are integer constants; the coefficient fits the ISA's signed
16-trit immediate, and an out-of-range finite value is diagnosed. The compiler
uses a pure structural `Radix3Const` value and existing ISA shift operations in
bounded chunks. It never evaluates the constant through a host binary FPU.

```trit
const QUANTUM: t40 = radix3(1, -5);
const ALSO_EXACT: t40 = 1 / 243;
```

Fractional constant expressions retain native arithmetic expressions instead
of passing through the compiler's integer evaluator. Integer folding declines
non-integral divisions, including at SSA optimization levels. This also prevents
using such a fraction as an integer array extent. T40 division by a power of
three is exact when representable. Arbitrary fractions such as 3/2 still follow
ordinary T40 rounding; finite radix-3 representation is not universal exactness.
Constants retain the environment and numeric width of their declaration, so a
later shadowed name or a wider alias does not erase an earlier rounding boundary.
Validated radix-3 constants can use the compiler's existing loop-invariant code
motion without changing arithmetic ordering or precision.

Conversion removal and loop-invariant placement may preserve representations
only when observable typed operations and rounding are preserved. Automatically
delaying numeric normalization, changing association, or replacing a reduction
with a fused operation needs an explicit language policy. Ordinary arithmetic
continues to use strict T40 semantics. This is independent of whether compilation
runs AOT or in a JIT.

## Rounding and exceptional values

For a finite balanced-trit tail below a retained digit, its magnitude is less
than half the retained unit: `sum(3^-k, k=1..n) = (1-3^-n)/2`. Dropping that tail
therefore selects the nearest retained balanced representation. This does not
justify deleting rounding from host integer division: truncation toward zero
of an integer mantissa differs from truncating balanced digits. Native arithmetic
uses nearest division by three during alignment/normalization, and the decoded
accumulator preserves that behavior. Infinite tails/halfway cases, division,
and multiplication require their own specified rounding rules.

Overflow and underflow retain their existing distinct sentinels; malformed
payloads remain invalid. Checked host decode distinguishes them, while unchecked
host conversion may map underflow to zero or overflow to infinity. New status
flags/CSRs would be architectural state requiring specified trap, call, process,
checkpoint, and device behavior; this representation optimization adds none.

## Repeatable evidence

```powershell
cmake --build build_current_cleanup --target trit_current_conformance
./build_current_cleanup/trit_current_conformance.exe --numeric-only
cmake --build build_current_cleanup --target ci_production
```

The current conformance executable writes `numeric_workflow_artifact/` below the
configured build directory: source, linked assembly at all three optimization
levels, accumulator/context assembly, a live mid-reduction disk checkpoint, and
`report.json`. The guest subtracts an exact 1/243 impulse 243 times across function
calls, checks module/local fractions and large positive/negative radix exponents,
and checks independent packed results. The accumulator scenario saves/restores
the current context while running and resumes a disk checkpoint mid-reduction.
Native checks reconcile rounding and exceptional states with strict packed T40.
The report also contains actual/expected packed guest values, source hashes,
same-host packed/retained T40 timings, and binary64/extended/Kahan controls.

Performance measurements must compare cached and packed T40 on the same host,
with identical inputs, operation ordering, precision, and final output. Report
emulator representation cost separately from ISA instruction counts and from
hardware design merit. Binary precision controls should include binary64,
`long double` with its actual `numeric_limits::digits`, and compensated binary64;
small differences alone do not establish a radix advantage.
