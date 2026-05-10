# Ternary VM Native Roadmap

This document tracks the implementation phases for the ternary numeric stack,
VM, lane/SIMD layer, and future ISA extensions. The core rule is that numeric
formats and lane/wire formats are separate families with explicit conversion
boundaries.

## Phase 1: Native Arithmetic Migration

Status: implemented.

Goal: replace FPU bridge arithmetic in VM execution paths with native ternary
operations.

Major changes:

- Added `ternary_native_ops.h` as the shared native arithmetic engine.
- Generalized numeric formats: `T1`, `T5`, `T10`, `T20`, `Triple/T40`, `LongTriple/T50`.
- Added native add/sub/mul/div/sqrt/compare/conversion paths.
- Added ISA/ASM width selectors and suffixes such as `.t5`, `.t20`, `.t50`.
- Reworked VM register and memory values into tagged native values.
- Added exhaustive and workload-style tests: Fibonacci, recursion, DFT/FFT, Taylor/Maclaurin, and edge cases.
- Fixed the exponent-alignment bug affecting fractional addition and Newton-Raphson sqrt iteration.

## Phase 2: Device-Portable Representation Layer

Status: implemented.

Goal: prepare the codebase for CPU SIMD, GPU kernels, SYCL/CUDA allocation, and
eventual hardware mappings without changing numeric semantics.

### Iteration 2.1: `UInt128`

- Added portable `UInt128` and `UInt256`.
- Replaced `LongTriple::data` backing storage with `UInt128`.
- Updated positional pack/unpack/power-table logic.
- Updated VM tagged payloads to use `UInt128`.

### Iteration 2.2: Flat VM Memory

- Replaced vector-owned VM memory with pointer-backed flat memory.
- Added `VMStateAllocator` so host, CUDA, or SYCL allocation can back the same VM state model.

### Iteration 2.3: Lane Types

- Added `TritLane1`, `TritLane5`, `TritLane10`, `TritLane20`, `TritLane40`, and `TritLane50`.
- Kept lane types separate from numeric `T1/T5/T10/T20/Triple/LongTriple`.
- Added explicit `toLane(...)` and `fromLane(...)` conversion boundaries.
- Added tritwise lane ops and tests.

Important distinction:

- Numeric formats are base-3 positional and carry arithmetic meaning.
- Lane formats are 2-bit-per-trit wire/SIMD/GPU transport formats.
- Binary arithmetic on lane backing integers is never ternary arithmetic.

## Phase 3: GPU/SIMD Execution Backend

Status: implemented. Plain C++ and CPU SIMD paths are validated; SYCL launch wrappers have been tuned on the local CPU OpenCL/default SYCL path; AMD GPU SYCL validation still requires the Codeplay AMD plugin or equivalent device backend.

Goal: accelerate lane operations and prepare the backend shape for GPU/SYCL
kernels.

Implemented so far:

- Added `ternary_simd.h`.
- Added `BatchBackend::Scalar`, `BatchBackend::Auto`, and `BatchBackend::Avx2`.
- Added scalar batch fallback for `neg`, `add`, `sub`, `compare`, `min`, and `max`.
- Added scalar batch support for `TritLane1/5/10/20/40/50`.
- Added AVX2 runtime dispatch for `TritLane20` `neg`, `add`, `sub`, and `compare`.
- Added `ternary_backend.h` for small device-safe helpers that avoid STL, exceptions, strings, and virtual dispatch.
- Added raw 64-bit and two-limb 128-bit lane ALU helpers for kernel-callable code paths.
- Added `ternary_kernel.h` batch wrappers for raw lane payloads across `TritLane1/5/10/20/40/50` semantics.
- Added optional CUDA managed-memory and SYCL shared-USM `VMStateAllocator` implementations behind opt-in build flags.
- Added optional CUDA and SYCL launch wrappers in `ternary_gpu_kernels.h` behind opt-in build flags.
- Replaced compiler `__int128` scratch arithmetic in `ternary_native_ops.h` with portable `Int128`/`UInt128` arithmetic.
- Extended benchmarks with scalar-vs-auto rows across batch operations and batch sizes.
- Added `kernel-raw` benchmark rows for the raw kernel-callable batch API.
- Added CUDA/SYCL validation harnesses and geometry tuning output in `tuning_results.md`.

External validation follow-ups:

- Validate SYCL on AMD GPU from a supported Linux/ROCm environment after installing the Codeplay AMD plugin or another compatible backend.
- Validate CUDA wrappers on a CUDA-capable GPU/toolchain.
- Continue splitting any newly discovered host-only utilities if future kernels need them.
- Tune GPU launch geometry and memory movement after real GPU measurements.

Tuning note:

- Current SYCL geometry tuning used the local `AMD Ryzen 9 3900X 12-Core Processor` via the available CPU OpenCL/default SYCL path.
- It does not represent AMD GPU SYCL behavior. On this Windows host, `sycl-ls` currently exposes only the CPU OpenCL device; Codeplay's AMD GPU plugin documentation targets Linux with ROCm/HIP.

Benchmark review:

- Keep the existing `TritLane20` AVX2 paths; they are clearly useful for `neg`, `add`, `sub`, and `compare`.
- Keep `TritLane1` scalar for now. Current scalar batch ops are already sub-nanosecond to low-nanosecond per lane, so an AVX2 path is unlikely to justify the extra code.
- Defer `TritLane5/10` AVX2 until after GPU/SYCL wrappers. They are fast enough in scalar form that backend plumbing is higher leverage.
- Keep `TritLane40/50` scalar in Phase 3. Their current cost is dominated by wider `UInt128` lane access and per-trit loops; no simple 128-bit-limb AVX2 strategy has been proven yet.

## Phase 4: Ternary-Aware ISA and Vector Extension

Status: Phase 4A base scalar foundations implemented; vector extension planned.

Goal: expose advantages that are specific to ternary computation instead of
copying a binary SIMD ISA shape.

### ISA Design Rules

- Keep width selection orthogonal: prefer `vadd.t20`, `vadd.t40`, `vdot.t1` over width-specific opcode names like `V32_ADD`.
- Keep numeric vector ops separate from lane/carryless logic ops:
  - `vadd.t20` is numeric and routes through carry, normalization, exponent, and rounding behavior.
  - `tladd.t20` is lane/carryless logic and operates directly on trit pairs.
- Preserve `T1` as a first-class predicate type, not just an integer that happens to be `-1/0/+1`.
- Avoid implicit crossing between numeric formats and lane formats.
- Keep all numeric/lane family crossings explicit and visible in the instruction stream.
- Spell explicit conversions as `cvt.src.dst`, where the first suffix is the source type and the second suffix is the destination type.
- Reserve opcode space for base ternary control, scalar lane/carryless ops, vector numeric ops, accumulator/AI ops, conversion, and device-query families.
- Define invalid/trap behavior consistently across VM, SIMD, GPU, FPGA, and ASIC paths before gather/scatter is added.
- Keep architectural vector length abstract. Do not bake 256-bit or 512-bit widths into the ISA.

### Phase 4 Prerequisites

These should be completed before any vector opcode work begins.

1. Fix the scalar fault model by separating `fault_valid` from `fault_class`. Implemented.
   - Current scalar code relies on `VMStatus` to decide whether `r27` is meaningful.
   - Vector execution needs partial-lane faults, so "did a fault occur" and "which class of fault" must be separate fields.
   - Scalar layout: `r27.trit[0] = fault_valid`, `r27.trit[1] = fault_class`.
   - `fault_valid == 0` means no fault. `fault_valid == +1` means `fault_class` is meaningful.
2. Add the extended R5 decode path needed by `TSEL`. Implemented.
   - This is not a new format discriminant.
   - The opcode identifies the extended layout.
   - `InstructionWord::decode()` sets decoded R5 fields when `opcode == TSEL`.

### Phase 4A Implemented

- Added scalar `TSEL` using the R5 layout.
- Added scalar `BRZ` and `BRP` alongside existing `BRN`.
- Added scalar `SWAP`.
- Added numeric `cvt.src.dst` assembler spelling while preserving destination-only `cvt.dst` compatibility.
- Made scalar `r27` a two-trit `fault_valid` / `fault_class` record.
- Added ISA, assembler, VM execution, and trap-record tests.

### Base ISA Additions

These should be implemented before the vector extension because they are useful
for scalar VM code and clarify the architecture.

- `tsel rD, rCond, rNeg, rZero, rPos`: ternary select. If `rCond` is `-1`, copy `rNeg`; if `0`, copy `rZero`; if `+1`, copy `rPos`.
- `brn`, `brz`, `brp`: branch on negative, zero, or positive `T1` predicate.
- `swap rA, rB`: explicit scalar register swap.
- `cvt.*.*`: explicit typed conversion. Examples:
  - `cvt.t10.t20 rD, rS`: promote `T10` to `T20`.
  - `cvt.t20.t10 rD, rS`: demote `T20` to `T10` with rounding/trap behavior defined by the format.
  - `cvt.t50.l50 rD, rS`: numeric to lane encoding.
  - `cvt.l50.t50 rD, rS`: lane encoding to numeric.

`TSEL` is the priority. It is the core ternary control-flow primitive and
should live in the base ISA, not the vector extension. It removes common
`TCMP + TINV + BRN` routing sequences, maps naturally to a 3:1 mux in FPGA/ASIC
logic, and becomes `vsel.*` for branch-divergence-free vector control later.

`TSEL` is tag-preserving. It selects and copies one entire register value. It
does not perform arithmetic, coerce types, round, normalize, or inspect the
selected payload. That is why base `tsel` needs no width suffix.

Encoding note: current R-type instructions expose `rd`, `rs1`, `rs2`, and
`func`, but `tsel` needs five register fields. The extended R5 layout fits in
the existing R-type word:

`[fmt:1 | opcode:4 | rd:3 | rCond:3 | rNeg:3 | rZero:3 | rPos:3 | reserved:7]`

It should use the existing reserved pad trits rather than consuming a second
instruction word.

### T1 Predicate Semantics

- `TCMP` produces `T1`.
- `BRN/BRZ/BRP` consume `T1`.
- `TSEL` consumes `T1`.
- Vector compare produces a vector of `T1` predicates.
- A vector of `T1` is the native predicate mask for SIMD/GPU/FPGA backends.

Important algebra distinction:

- `T1` multiplication is sign product: `-1 * -1 = +1`.
- Ternary AND is `TMIN` / `tland`.
- These are different truth tables and different circuits.
- For `vmac.t1` and `vdot.t1`, `T1` weights avoid the multiplier anyway:
  nonzero signs combine by sign XOR, and zero on either input produces zero.
  Each weight means conditional subtract, skip, or conditional add.

### Scalar Lane / Carryless Opcodes

Carryless trit algebra:

- `tladd.*`: per-trit carryless add.
- `tlsub.*`: per-trit carryless subtract.
- `tlneg.*`: per-trit negation.
- `tland.*`: ternary AND, equivalent to tritwise minimum.
- `tlor.*`: ternary OR, equivalent to tritwise maximum.

These are distinct from numeric `add/sub/neg`. They operate on lane/wire
encodings and must not be implemented by binary arithmetic on the backing
integer. They are defined over lane types from `ternary_lanes.h`.

### Vector Numeric Opcodes

- `vadd.*`, `vsub.*`, `vneg.*`, `vmul.*`, `vdiv.*`: numeric vector arithmetic.
- `vcmp.*`: vector compare, producing a `T1` predicate vector.
- `vsel.*`: vector ternary select, using a `T1` predicate vector.
- `vload.*`, `vstore.*`: contiguous vector memory movement.
- `vbcast.*`: broadcast scalar value into every vector lane.
- `vlen rD`: query implementation vector length.

Vector numeric ops work on numeric formats. They are separate from `tl*` lane
ops because they use different hardware paths and have different semantics.

### Accumulator and AI Opcodes

- `aclr.*`: clear accumulator.
- `aload.*`: load accumulator from scalar value.
- `aadd.*`, `asub.*`, `amul.*`: accumulate numeric work.
- `astore.*`: write accumulator to scalar register.
- `vmac.t1`: multiply-accumulate packed ternary weights into an accumulator.
- `vdot.t1`: dot product over packed `-1/0/+1` lanes.
- `vact.t1`: ternary activation, mapping sign to `-1/0/+1`.

Accumulator precision decision: accumulators should maintain `T50` precision
unless a narrower suffix explicitly says otherwise. This mirrors binary
low-precision multiply with wider accumulation, but ternary `T1` multiply is
cheaper: it is conditional add/subtract/skip, not a true multiply.

### Vector Plumbing and Memory

- `vpack.*`, `vunpack.*`: explicit precision conversion.
- `vpermute`: rearrange vector lanes.
- `vblend.*`: blend lanes under a predicate mask.
- `vswap`: explicit vector register swap.
- `vgather.*`, `vscatter.*`: add last, after vector fault semantics are stable.

### Fault Model

Scalar traps currently use a ternary-native trap class in one trit:

- `-1`: division by zero.
- `0`: memory fault.
- `+1`: illegal operation.

The scalar model must be corrected before vector faults are implemented:

- `fault_valid` says whether a fault occurred.
- `fault_class` says which ternary fault class occurred.
- `fault_class` is meaningful only when `fault_valid == +1`.
- This makes the no-fault state explicit instead of relying only on `VMStatus`.

Vector faults need an explicit model before gather/scatter:

- Use a per-lane `T1` fault-valid vector.
- Use a per-lane `T1` fault-class vector.
- `fault_class[lane]` is meaningful only when `fault_valid[lane] == +1`.
- Make fault selection compatible with `TSEL`/`vsel.*` so fault handling can
  route on `-1/0/+1` without binary flag emulation.

### Vector Register Model

- Introduce vector registers separately from scalar `R0..R26`.
- Keep architectural vector length abstract rather than hard-coding 256 or 512 bits into the ISA.
- Add `vlen` so AVX2, AVX-512, GPU kernels, FPGA, and ASIC backends can choose different physical widths.

### Phase 4 Implementation Order

1. Correct scalar fault encoding with separate `fault_valid` and `fault_class`. Done.
2. Add the R5 decoder path for `TSEL`. Done.
3. Add base ternary control instructions: `tsel`, `brz`, `brp`, `swap`, and numeric `cvt.src.dst`. Done.
4. Add scalar lane/carryless opcodes: `tladd`, `tlsub`, `tlneg`, `tland`, and `tlor`.
5. Add vector numeric opcodes after the SYCL/GPU backend boundary is stable.
6. Add accumulator and AI ops: `vdot.t1`, `vmac.t1`, and `vact.t1`.
7. Add gather/scatter and complex vector plumbing only after the vector fault model is tested.

## Phase 5: Hardware Backends

Status: future.

Goal: move the same semantics onto concrete GPU, FPGA, and ASIC targets.

Major goals:

- SYCL/CUDA kernels over lane types.
- Device allocator implementations.
- FPGA-friendly lane ALU modules.
- ASIC-oriented decode/execute mapping.
- Conformance tests shared across CPU VM, SIMD, GPU, FPGA simulation, and ASIC models.
