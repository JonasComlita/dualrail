# Ternary VM Native Roadmap

This document tracks the implementation phases for the ternary numeric stack,
VM, lane/SIMD layer, and future ISA extensions. The core rule is that numeric
formats and lane/wire formats are separate families with explicit conversion
boundaries.

Optimization before/after measurements are tracked in
`optimization_baseline.md`. Add a new dated entry there before accepting any
hot-path optimization.

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

Status: Phase 4 reference VM implementation complete. Scalar lane ops, vector
numeric ops, accumulator/T1 AI ops, vector plumbing, and gather/scatter now have
ISA, assembler, VM execution, and tests. Hardware acceleration remains Phase 6.

Goal: expose advantages that are specific to ternary computation instead of
copying a binary SIMD ISA shape.

### ISA Design Rules

- Keep width selection orthogonal: prefer `vadd.t20`, `vadd.t40`, `vdot.t1` over width-specific opcode names like `V32_ADD`.
- Keep numeric vector ops separate from lane/carryless logic ops:
  - `vadd.t20` is numeric and routes through carry, normalization, exponent, and rounding behavior.
  - `tladd.l20` is lane/carryless logic and operates directly on trit pairs.
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

### Phase 4 Implemented

- Added scalar `TSEL` using the R5 layout.
- Added scalar `BRZ` and `BRP` alongside existing `BRN`.
- Added scalar `SWAP`.
- Added numeric `cvt.src.dst` assembler spelling while preserving destination-only `cvt.dst` compatibility.
- Made scalar `r27` a two-trit `fault_valid` / `fault_class` record.
- Added `.lN` lane value tags and explicit numeric/lane conversion boundaries.
- Added scalar lane/carryless opcodes: `tladd`, `tlsub`, `tlneg`, `tland`, and `tlor`.
- Added vector registers, abstract `vlen`, vector fault masks, and core vector numeric ops.
- Added accumulator ops and T1 AI ops: `vdot.t1`, `vmac.t1`, and `vact.t1`.
- Added vector conversion, permutation, blend, swap, gather, and scatter reference VM paths.
- Added ISA, assembler, VM execution, vector-fault, and trap-record tests.

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

- `vpack.src.dst`, `vunpack.src.dst`: explicit precision conversion.
- `vpermute`: rearrange vector lanes.
- `vblend.*`: blend lanes under a predicate mask.
- `vswap`: explicit vector register swap.
- `vgather.*`, `vscatter.*`: indexed memory movement with lane-local faults.

### Fault Model

Scalar traps currently use a ternary-native trap class in one trit:

- `-1`: division by zero.
- `0`: memory fault.
- `+1`: illegal operation.

The scalar model has been corrected before vector faults are implemented:

- `fault_valid` says whether a fault occurred.
- `fault_class` says which ternary fault class occurred.
- `fault_class` is meaningful only when `fault_valid == +1`.
- This makes the no-fault state explicit instead of relying only on `VMStatus`.

Vector faults use an explicit lane-local model:

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
2. Add the R5 decoder path for `TSEL`, `VSEL`, and `VBLEND`. Done.
3. Add base ternary control instructions: `tsel`, `brz`, `brp`, `swap`, and numeric `cvt.src.dst`. Done.
4. Add scalar lane/carryless opcodes: `tladd`, `tlsub`, `tlneg`, `tland`, and `tlor`. Done.
5. Add vector numeric opcodes after the SYCL/GPU backend boundary is stable. Done in the scalar reference VM.
6. Add accumulator and AI ops: `vdot.t1`, `vmac.t1`, and `vact.t1`. Done.
7. Add gather/scatter and complex vector plumbing after the vector fault model is tested. Done in the scalar reference VM.

## Phase 5: GPU Launch, IR, Benchmarks, and Transformer Runtime

Status: in progress.

Goal: finish the near-complete GPU lane launch validation, then add the minimal
compiler/IR layer and benchmark suite needed before building a maintainable
tiny NanoGPT-class transformer forward pass. The pivot is intentional: serious
VM programs should be generated from a typed IR rather than maintained as large
hand-written assembly programs.

Important host limitation:

- This Windows machine cannot currently validate AMD GPU SYCL through Codeplay
  oneAPI because the Codeplay AMD plugin targets Linux with ROCm/HIP.
- Current local SYCL results are CPU OpenCL/default SYCL results only.
- AMD GPU validation should be run later from a supported Linux/ROCm
  environment, or through another confirmed AMD GPU SYCL backend.

### Phase 5A: GPU Kernel Launch Validation

Status: local launch-boundary validation implemented; external AMD GPU
validation remains deferred to Linux/ROCm.

Goal: prove that the already-written device-side lane ALU can execute through
real kernel launch wrappers. This is not new arithmetic design; it is launch,
device-memory, and conformance work.

What is already close:

- `ternary_backend.h` contains device-safe raw lane helpers.
- Raw lane add/sub/neg/compare/min/max helpers are already written without STL,
  exceptions, strings, or virtual dispatch.
- `ternary_kernel.h` exposes batch wrappers for raw lane payloads.
- Optional CUDA/SYCL allocators and launch wrappers already exist behind
  compile-time flags.
- CPU SIMD and CPU OpenCL/default SYCL paths have already been benchmarked.
- CUDA/SYCL harnesses now run conformance plus timing over every raw lane width
  and operation, with deterministic invalid/spare payload cases.
- CUDA and SYCL CMake targets are opt-in, so normal repo builds do not require
  GPU or oneAPI tooling.

Scope:

- Add or finish minimal `__global__` / `parallel_for` launch wrappers where the
  existing wrappers are incomplete.
- Validate `batchTritwiseAddRaw64` and related raw lane operations on an actual
  device buffer.
- Keep tests shared with scalar/reference CPU paths.
- Keep numeric formats and lane formats separate. This phase validates lane ALU
  execution, not full numeric `T10/T20/T40/T50` arithmetic kernels.
- Record every device run in `tuning_results.md`.

Validation:

- CPU scalar reference and device result buffers must match bit-for-bit.
- Cover `TritLane1/5/10/20/40/50` raw operations where the backend supports the
  payload width.
- Include invalid/spare lane payload behavior.
- Record device name, backend, compiler, local size, payload count, operation,
  runtime, checksum, and whether the run was CPU fallback or real GPU.

Non-goals:

- Do not require AMD GPU SYCL validation on this Windows host.
- Do not add transformer-specific opcodes here.
- Do not add FPGA/ASIC modules here.

### Phase 5B: Minimal Ternary IR and Lowering

Status: first implementation complete.

Goal: add a small purpose-built IR that can generate ternary VM programs from a
typed operation graph. This is not a full LLVM-scale compiler. It is the minimum
layer needed to make transformers, benchmarks, and later cryptographic kernels
maintainable.

IR requirements:

- Represent typed scalar, vector, and lane values: `T1/T5/T10/T20/T40/T50` and
  `L1/L5/L10/L20/L40/L50`.
- Represent operations as a small graph or linear SSA-like form with typed
  edges.
- Lower directly to existing ISA/assembler text first, not binary instruction
  words directly.
- Add a simple register allocator for `r0-r26` and `v0-v7`.
- Add accumulator-aware lowering for `aclr`, `aload`, `aadd`, `asub`, `amul`,
  `astore`, `vdot.t1`, and `vmac.t1`.
- Add explicit lowering for `cvt.src.dst`; never insert hidden numeric/lane
  crossings.
- Emit labels, branches, calls, and loops through the existing assembler.

Initial IR operations:

- Constants, copy, load, store, vector load/store, and conversion.
- Numeric arithmetic, compare, `tsel`, branches, and loop labels.
- Vector arithmetic, `vsel`, `vbcast`, `vpack`, `vunpack`, gather/scatter.
- Accumulator and T1 AI dot/MAC operations.

Implemented first tranche:

- Added `ternary_ir.h` under `namespace sandbox::ir`.
- Added typed scalar/vector values for numeric and lane widths.
- Added deterministic register allocation over `r1-r24` and `v0-v7`, with
  diagnostics on exhaustion and no implicit spilling.
- Lowering emits assembly text first and then calls the existing assembler.
- Typed arithmetic always emits explicit suffixes; `cvt` emits `.src.dst`;
  exact no-op conversions are removed.
- Added `test_ternary_ir.cpp` for assembly text checks, VM execution,
  diagnostics, branch/load/store/vector/accumulator/T1 coverage, and register
  exhaustion.

Validation:

- Golden tests compare generated assembly against expected instruction
  families, not exact register names where allocation is intentionally flexible.
- Run generated programs through the existing VM and compare outputs to direct
  host references.
- Include register pressure tests that force spills or reject programs with a
  clear diagnostic.
- Add a static no-bridge check for generated runtime programs.

Non-goals:

- Do not implement a general programming language in this phase.
- Do not add optimizer passes beyond tiny local cleanups such as adjacent
  no-op `cvt` removal.
- Do not add new ISA opcodes to make lowering easier.

### Phase 5C: Architectural Benchmark Suite

Status: first deterministic benchmark tranche implemented.

Goal: produce repeatable numbers for the architecture's core claims before the
transformer and security work. Each benchmark should report VM step count,
runtime, checksum, memory footprint, and the exact program source or generated
IR.

Benchmarks:

- T1 dot product through `vmac.t1` / `vdot.t1` versus T50 multiply-add.
- Ternary heap versus binary heap emulated on the VM.
- Three-way routing with `tcmp`/`tsel` versus binary-style two-branch routing.
- Two-tailed outlier detection using ternary predicates.
- Existing Fibonacci, recursion, DFT/FFT, and Taylor/Maclaurin workloads with
  step-count and timing rows.
- Tiny generated matmul kernels in T10, T20, and T50.

Implemented first tranche:

- Added `benchmark_architecture.cpp`.
- Results are written to `architecture_benchmark_results.md`.
- Current rows cover T1-vs-T50 dot, ternary-vs-binary-style routing,
  two-tailed outlier routing, small heap-shape routing, and generated small
  matmul at T10/T20/T50 widths.
- Rows report generated program size, VM steps, checksum, baseline, and
  relative step-count delta where a paired baseline exists.

Validation:

- Every benchmark must have a deterministic checksum.
- Record results in `optimization_baseline.md` or a dedicated benchmark results
  document before and after any optimization.
- Keep binary comparisons honest: compare VM instruction counts separately from
  host CPU wall-clock time.

### Phase 5D: Tiny Transformer VM Runtime

Status: initial generated-VM runtime scaffold implemented after Phase 5B and
the first Phase 5C benchmark tranche.

Goal: build the first useful application-level benchmark on top of the
completed scalar/vector ISA: a tiny NanoGPT-class transformer forward pass
running through generated VM programs. This phase should prove that the
architecture can express modern neural workloads without hidden FPU bridges
while giving us a stable benchmark before lower-level hardware optimization
continues.

Scope:

- Generate transformer kernels through the IR/lowering layer, not by writing a
  large hand-maintained assembly program.
- Use existing ISA features first: `add`, `mul`, `div`, `sqrt`, `tsel`, `cvt`,
  `vload`, `vstore`, `vdot.t1`, `vmac.t1`, `vact.t1`, and the accumulator
  family.
- Keep numeric and lane families explicit. Quantized storage crosses through
  `cvt.*.*` or runtime conversion helpers only at named boundaries.
- Prefer VM-callable math routines before adding new opcodes. Add opcodes only
  after profiling shows a routine is both common and expensive.
- Record before/after measurements in `optimization_baseline.md` before any
  runtime or hot-path optimization is accepted.

Initial runtime pieces:

- Added a VM-callable `exp` routine.
  - Native C++ `ops::exp(LongTriple)` already exists and is tested.
  - The generated runtime path implements this through existing arithmetic
    instructions and keeps native exponential calls in tests only.
- Added `softmax` over a vector.
  - Use max subtraction for stability.
  - Use `exp`, sum reduction, and reciprocal/division.
- Added `tanh` and `gelu` scalar kernels.
  - `vact.t1` already covers ternary sign activation for T1 paths.
- Added matrix multiply kernels.
  - Start with scalar nested loops for correctness.
  - Add accumulator-backed versions.
  - Add T1 dot/MAC paths using `vdot.t1` and `vmac.t1`.
- Added layer normalization.
  - Mean, variance, reciprocal square root, scale, and bias are expressible
    with existing VM arithmetic.
- Added a tiny tensor/memory layout.
  - Define row-major tensor storage in DMEM.
  - Define packed lane storage for quantized weights where useful.
  - Keep tokenizer and embedding tables as explicit software data structures.
- Added a tiny model fixture.
  - Prefer a character-level model with small vocabulary and dimensions.
  - Keep weights deterministic and small enough for fast regression tests.

Validation:

- Compare every runtime routine against a host reference using fixed inputs.
- Add VM program tests for `exp`, `softmax`, matmul, layer norm, and activation.
- Add an end-to-end tiny transformer forward-pass test.
- Track instruction counts, VM dispatch throughput, memory footprint, and
  numerical error.
- Add static no-bridge checks for the runtime path.

Non-goals:

- Do not train a model in this phase.
- Do not target GPT-2-small scale as the first milestone.
- Do not add CUDA/SYCL/FPGA-specific execution here.
- Do not make lane/numeric conversion implicit to simplify model code.
- Do not implement security primitives as part of the transformer milestone.

## Phase 6: Core Tooling and OS-Capable Machine Roadmap

Status: in progress (incorporating implementation plan targets).

Goal: resolve core architectural correctness bugs, finalize the ternary ISA expansion, add VM tooling needed for kernel-scale debugging and static data, then formalize the OS machine contract before expanding into a full source language frontend.

### Scope and Implementation Tracks

#### Track 6.0: Correctness and Architectural Foundations (Completed)
- **Bug Fixes**: Corrected `MOVH` register masking, eliminated silent denormal precision loss in `fromLane` for `T10/T20`, aligned AVX2 SIMD validity checking with scalar paths, added `TernaryValue` equality operators for optimization passes, and ensured copy constructor exception safety in `TernaryMemory`.
- **Architectural Unification**: Established canonical in-memory representations unifying raw lanes, typed `TritLane<N>`, `Triple`/`LongTriple`, and `TernaryValue`.
- **Native Word & Width Semantics**: Unified machine word configuration to default to `T40` (80 bits packed in `uint64_t`), transitioning `T50` into an explicit extended precision target. Configured default vector lengths to ternary-native powers of 3 (`DEFAULT_VECTOR_LENGTH = 27`), and standardized `STORE` source register addressing conventions.

#### Track 6.1: ISA Expansion (Verified)
- **Indirect Control Flow**: Implement register-indirect branches (`CALLR`, `JMPR`) to support function pointers, dynamic dispatch, and vtables.
- **Advanced Arithmetic & Windowing**: Register `R4`-type instruction layouts supporting windowed comparisons (`TWCMP`) and range clamping (`TCLAMP`). Implement remainder extraction (`TMOD`), structural exponent scaling/shifting (`TLSHIFT`, `TRSHIFT`), and scalar multiply-accumulate (`TMAC`).
- **Ternary-Native Analysis**: Register non-zero trit counting (`TCOUNT`) and most-significant non-zero trit scanning (`TSCAN`) primitives.
- **Infrastructure & Reductions**: Implement sandbox system service invocation gates (`SYSCALL`), architectural synchronization fences (`FENCE`), and horizontal vector reductions (`VSUM`, `VHMIN`, `VHMAX`).
- **Assembler & Execution Integration**: Update assembly builder mapping, decoding logic, and execution dispatch loop handlers for all 15 new opcodes.

Current integration note:
- Opcodes 59-73 now have ISA decode/disassembly, assembler mnemonics, VM execution, and IR builder helpers.
- `CALLR` and `JMPR` consume absolute instruction-memory PC targets from numeric scalar registers; `CALLR` writes `LR = PC + 1`.
- `SYSCALL` uses a sandbox service-id immediate: `1` appends `r1` as decimal text, `2` appends newline, and `3` clears the syscall buffer.
- Focused Phase 6.1 verification passes in `test_multiwidth_vm` and `test_ternary_ir`.
- Regression checks also pass in `test_native_ops`, `test_numeric_workloads`, and `test_ternary_lanes`.

#### Track 6.2: VM Tooling and Infrastructure Bridge (Implemented)
- **VM Observability**: Added lightweight debugging and tracing execution hooks (`VMHooks` exposing `onStep`, `onTrap`, and `onHalt`) to support compiler and kernel output profiling without core patching.
- **Data Sections**: Added assembler `.text`, `.data`, `.word`, text labels, data labels, and mixed text/data/text section support for static storage resolution.
- **Step Limits**: Formalized `run()` limits so one architecturally executed instruction is one step; timeout leaves `VMStatus::RUNNING` and allows execution to continue later.
- **Math Caching & Fallbacks**: Added lazy cached statics for expensive series constants (`cachedLn3()`, `cachedPi()`) and schoolbook limb multiplication for `UInt128 * UInt128` and `multiplyFull(UInt128, UInt128)`.

#### Track 6.3: Core VM OS Substrate (Implemented)
- **CSR and Return-from-Trap ISA**: Added `CSRR`, `CSRW`, and `ERET` as opcodes 74-76 with assembler, disassembler, decode, VM dispatch, and IR builder support.
- **Privilege and Routed Traps**: Added kernel `-1`, supervisor/reserved `0`, and user `+1` privilege state; routed traps save `EPC`, `CAUSE`, and packed `STATUS`, disable interrupts, enter kernel mode, jump to `TVEC`, and resume through `ERET`.
- **Syscalls, Timer IRQs, and Protection**: In routed mode, user `SYSCALL imm` becomes an ECALL-style trap with `SYSCALL_ID`; deterministic timer IRQs route through the same trap path; user-mode fetch/load/store use v1 IMEM/DMEM base-limit protection while kernel bypasses those bounds.
- **Compatibility**: Legacy halt-on-trap behavior and sandbox syscall buffer services remain unchanged until trap routing is enabled by kernel setup.

#### Track 6.4: Remaining OS Architecture Contract (Next)
- **ABI**: Formalize the calling convention around `r1-r12` callee-saved, `r13-r24` caller-saved/argument/return/scratch, `r25` link register, `r26` stack pointer, and `r27` trap/status.
- **Memory, Atomics, and Boot**: Document single-core sequential consistency, future `FENCE`/atomic semantics, reset vector, initial mode, initial stack, trit order, data layout, and instruction alignment.
- **Next Kernel Substrate**: Reserve full page tables, atomics, `WFI`, sticky FP flags, context switching, and the tiny two-task kernel for follow-on OS substrate sprints.

#### Track 6.5: Advanced IR Expansion
- **Structural Node AST**: Replace string-based code emission with structured instruction node types (`IrInstr`) containing explicit source/destination operand payloads.
- **Type Auto-Widening Lattice**: Establish automatic numeric widening rules (`T1 < T5 < T10 < T20 < T40 < T50`) with implicit conversion node insertion.
- **Control Flow Graphs (CFG)**: Build explicit `BasicBlock` topologies supporting structured high-level closure builders (`ifTernary`, `whileLoop`, `forRange`).
- **Analysis & Optimization Pipeline**: Implement pre-lowering verification passes, explicit liveness analysis supporting automatic register release, and localized SSA optimization passes (Copy Propagation → Constant Folding → CSE → DCE → Strength Reduction).
- **Module Abstraction**: Implement `Function` calling conventions and `Module` containers for multi-function compilation.

#### Track 6.6: High-Level Source Language Frontend
- **Language Design**: Design a premium ML-style ternary-native language mapping types directly to precision layers (`t1..t50`, `l1..l50`, `vec<t20>`), exposing first-class three-way conditional blocks (`match sign(x)`), replacing booleans with ternary conditions, and embedding dedicated carryless logic operators (`|+|`, `|-|`, `/\`, `\/`, `~`).
- **Frontend Stages**: Implement an end-to-end driver orchestrating tokenization (Lexer), recursive descent parsing (Parser → AST), type checking/widening resolution, lowering to structural IR blocks, optimization passes, and backend assembly compilation.

### Critical Gaps to Bridge for `xv6` OS Execution
The VM now has the first OS substrate layer: privilege state, CSR control registers, routed trap/interrupt entry, `ERET`, syscall traps, deterministic timer IRQs, and v1 user base-limit protection. The remaining `xv6` path is now less about "can the VM trap?" and more about ABI, memory management, scheduler context, and toolchain conventions.

#### A. Privilege Rings (Kernel vs. User Mode)
Implemented in substrate v1:
* **Ternary Protection Mode**: `-1` kernel, `0` supervisor/reserved, `+1` user.
* **Restricted Opcodes**: CSR writes and `ERET` are kernel-only and route protection faults from user mode.
* **V1 Protection**: User fetch/load/store obey IMEM/DMEM base-limit CSRs; kernel bypasses those v1 bounds.

#### B. Memory Management Unit (MMU) & Page Tables
Still future work. The current substrate is base-limit protection, not virtual memory.
* **Virtual Address Translation**: Add a page-table base register (`PTBR`) or equivalent CSR family later. The VM execution loop will need to translate user virtual addresses to physical DMEM/IMEM pages before true process isolation.

#### C. Preemptive Timer Interrupts (`IRQ`)
A deterministic timer IRQ exists now and can route to `TVEC` when interrupts are enabled.
* **Scheduler Context**: The next step is saving/restoring register files and per-task CSRs around that timer trap, then using the timer handler to switch between two minimal tasks.

By augmenting `VMState` with **Privilege Modes, Virtual Translation, and Hardware Interrupts**, your machine will transition from executing isolated functional algorithms to booting a fully interactive virtualized operating system.

### Critical Path

The current OS-oriented sequence prioritizes the machine contract before the full source language:
```
Phase 0/1 foundations -> Phase 6.1 ISA expansion -> Phase 6.2 VM tooling bridge ->
Phase 6.3 core VM OS substrate -> ABI/memory-model/boot contract docs ->
minimal IR/C-like kernel authoring -> tiny kernel milestone -> source language expansion
```
Full language and optimization work should grow from the ABI, trap/interrupt, memory-model, and boot contracts rather than preceding them.

Validation:

- End-to-end lowering verification passing multi-function AST input through lexing, parsing, lowering, and VM reference checks.
- Static liveness test verifications guaranteeing register reuse without leaks or manual release invocations.

Non-goals:

- Do not implement arbitrary memory-hard garbage collectors in the initial source language runtime.
- Do not add target-specific backends directly inside the source language lowering phase.

## Phase 7: Security and Post-Quantum Crypto Primitives

Status: future.

Goal: apply the measured IR, benchmark, and hardware work to security workloads
where ternary arithmetic is structurally useful. Password hashing is not the
target; password hashes are deliberately slow and memory-hard, so being faster
is not a useful security claim.

Major goals:

- Implement lattice/PQC inner products with T1/T5 coefficients.
- Implement polynomial multiplication for Kyber/Dilithium-like workloads.
- Implement and benchmark NTT-style finite-field kernels.
- Keep crypto code generated through the IR so constant-time behavior can be
  inspected and tested.
- Use published, peer-reviewed algorithms as the cryptographic basis; optimize
  the VM/hardware execution path, not secret proprietary crypto math.

## Phase 8: FPGA/ASIC and Production Hardware Backends

Status: future.

Goal: move the validated semantics beyond the reference VM and one-off GPU
launch validation into production-quality hardware backends, starting with an
FPGA prototype that can run real Phase 5 benchmark programs.

Major goals:

- Broaden SYCL/CUDA kernels from lane ALU launch validation into selected
  numeric/vector operations.
- Harden device allocator implementations.
- FPGA instruction decoder derived from `ternary_isa.h`.
- Scalar register file and simple fetch/decode/execute pipeline.
- FPGA-friendly trit full-adder, lane ALU, accumulator, and `vmac.t1` modules.
- ASIC-oriented decode/execute mapping.
- Conformance tests shared across CPU VM, SIMD, GPU, FPGA simulation, and ASIC models.
