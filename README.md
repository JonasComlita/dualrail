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
- `SYSCALL` keeps legacy sandbox service ids when trap routing is disabled; in OS-routed user mode it traps to the kernel with the id in `SYSCALL_ID` and arguments in `r13-r18`.
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

#### Track 6.4: OS Contract Lock-In and Trap-Save Primitive (Implemented)
- **Trap-Save CSR Primitive**: Added `CSRRW` as opcode 77 so trap handlers can atomically swap `sp` with `scratch`, matching the kernel-stack handoff pattern needed for safe interrupt context save.
- **Trap Entry Convention**: While running user code, `SCRATCH` holds the current task kernel context/stack pointer. The first trap-handler instruction should be `csrrw sp, scratch, sp`; before `ERET`, the handler performs the reverse swap.
- **ABI Defaults**: `r0` is zero; `r1-r12` are callee-saved; `r13-r18` are argument registers with `r13` as primary return; `r19-r24` are caller-saved temporaries; `r25` is link register; `r26` is stack pointer; `r27` remains legacy trap/status visibility.
- **Memory and Boot Contract**: Single-core execution is sequentially consistent. `FENCE` is an architectural/compiler barrier and VM no-op for now. Reset starts in kernel mode with interrupts and MMU disabled, PC `0`, and SP at the top of DMEM.

#### Track 6.5: Memory Management and Page Tables (Implemented)
- **Single-Level MMU v1**: Added CSR-controlled user IMEM/DMEM page tables with fixed 27-word pages. Kernel mode bypasses translation; user mode fetch/load/store use virtual word addresses when `MMU_ENABLE` is set.
- **PTE Layout**: Page tables live in DMEM as raw T40 PTE words: low trits encode valid/user/read/write/execute flags, and trit 5 onward encodes the physical page number.
- **Fault Reporting**: Page faults set `PAGE_FAULT_ADDR` and `PAGE_FAULT_ACCESS`; fetch/load/store page faults use dedicated causes, while permission failures use protection faults.

#### Track 6.6: Preemptive Timer and Two-Task Switch (Implemented)
- **Task Context Layout**: Defined a 32-word task context carrying saved virtual PC, packed status, IMEM/DMEM page-table CSRs, and `r1-r26`.
- **Timer Context Switch Proof**: Added a timer-driven trap handler proof using `CSRRW` to preserve user `sp`, save/restore all task registers and paging CSRs, re-arm the timer, and return through `ERET`.
- **Isolation Acceptance**: The VM test suite now switches between two user tasks sharing the same virtual PC/data address while mapping their counters through different physical DMEM pages.

#### Track 6.7: Minimal Kernel Bring-Up (Implemented)
- **Bootable Assembly Artifact**: Added `OS3/minimal_kernel_bringup.tasm`, a single assembly image that boots at PC `0`, installs `TVEC`, configures user page tables, enables the MMU and timer, and enters user mode with `ERET`.
- **Assembler Kernel Support**: Added `.org` for physical text/data placement and `.pte` for raw T40 page-table entries, so kernel images can carry page tables and task contexts without C++ host patching.
- **Kernel Acceptance Test**: The VM now runs the artifact as a boot image and verifies timer-driven switching between two user tasks with separate physical counters.

#### Track 6.8: Kernel Syscall and Console Device Path (Implemented)
- **Console CSRs**: Added `console_out` and `console_ctrl` as kernel-visible device CSRs. `console_out` appends a decimal word to the VM output buffer; `console_ctrl` appends newline for positive writes, clears for negative writes, and reads back output length.
- **Syscall ABI v1**: Routed user `SYSCALL imm` keeps the syscall id in `SYSCALL_ID`, passes arguments in `r13-r18`, and returns status/value in `r13`.
- **Kernel-Mediated I/O**: The minimal kernel artifact now handles syscall ids `1` write integer, `2` newline, and `3` clear through the console CSRs while preserving timer preemption.

#### Track 6.9: Critical Sections and Process Table Seed (Implemented)
- **Interrupt-Disable Critical Sections**: Verified that timer interrupts remain pending while interrupts are disabled and route precisely once kernel code re-enables interrupts through `STATUS`.
- **Process Table Seed**: Replaced the fixed two-context toggle in the minimal kernel with `proc_count`, `current_proc`, and `proc_table[]`, where each table entry points to a task context.
- **Scheduler State Verification**: The boot artifact test now verifies process-table metadata, context pointers, current-process bounds, and continued two-task preemption.

#### Track 6.10: Ternary Architecture Contract Pack (Implemented)
- **Contract Document**: Added `OS3/TERNARY_ARCHITECTURE_CONTRACTS.md` as the shared architecture contract for VM, assembler, compiler, kernel, debugger, FPGA, and future multicore work.
- **Ternary-Native Memory/Atomic Direction**: Locked the planned memory-order trit (`-1` relaxed, `0` acquire-release, `+1` sequential consistency) and the proposed `TLDR`/`TSTR` result convention (`-1` collision, `0` value mismatch, `+1` success).
- **Cross-Cutting Contracts**: Captured trap/interrupt rules, ABI, data layout, instruction alignment, FP sticky flag direction, reset/boot, syscall/device rules, and scheduler substrate boundaries.

#### Track 6.11: Ternary Atomics and Lock ABI (Implemented)
- **Atomic ISA**: Added `TLDR` and `TSTR` with the ternary memory-order trit encoded as `-1` relaxed, `0` acquire-release, and `+1` sequential consistency.
- **Store-Conditional Status**: `TSTR` returns `-1` for reservation loss/collision, `0` for expected-value mismatch, and `+1` for success.
- **Memory-Order FENCE**: `FENCE` now accepts the same ternary order spelling (`fence.-1`, `fence.0`, `fence.+1`) while remaining a single-core VM ordering marker.
- **Lock ABI Proof**: Added VM tests for atomic success, value mismatch, reservation collision, and a minimal acquire/critical-section/release sequence.

#### Track 6.12: Scheduler Policy and Process Lifecycle (Implemented)
- **Process States**: Added runnable, running, blocked, sleeping, and exited process-state constants and sidecar process metadata.
- **Run Queue Policy**: The minimal kernel scheduler now wakes ready sleepers, skips blocked/sleeping/exited tasks, and chooses the next runnable process instead of blindly toggling contexts.
- **Timer Accounting**: Tracks per-process timer ticks, quantum remaining, and preemption counts.
- **Blocking Surface**: Added `proc_wakeup_tick` metadata and tests proving blocked tasks are skipped and sleeping tasks wake deterministically.

#### Track 6.13: Kernel Services v1 (Implemented)
- **Lifecycle Syscalls**: Added kernel-handled `yield`, `sleep_until_tick`, `exit`, `getpid`, and `uptime` syscalls while preserving console syscalls.
- **Wait Metadata**: Added wait-channel metadata so timer sleepers are woken by the scheduler instead of polling.
- **Idle Task**: Expanded the minimal kernel image to three user tasks plus an idle task, with idle selected when no user process is runnable.
- **Kernel Acceptance Image**: The boot artifact now proves sleep/yield/exit/getpid behavior, task exit removal, timer wakeup, console output, and idle progress in one image.

#### Track 6.14: Device/Event Queues and Kernel Authoring (Implemented)
- **Event Queues**: Generalized timer sleeping into wait-channel metadata that also handles console input and child-exit waits.
- **Device Blocking**: Added host-fed console input CSRs and blocking read behavior.
- **Ready/Wait Queues**: Added ready/wait queue metadata for the Phase 4 scheduler proof.
- **Kernel Authoring Ergonomics**: The kernel artifact now uses shared queue, wakeup, and syscall paths instead of fixed two-task toggles.

#### Track 6.15: Self-Describing Binary Loader and Object ABI Seed (Implemented)
- **Executable Header v1**: Added fixed executable headers with magic/version, ABI version, entry virtual PC, text/data page counts, stack hint, syscall ABI version, and flags.
- **Assembler Metadata**: `.execheader` emits header words and records executable metadata in `AssemblyResult`.
- **Static Loader**: Added loader helpers that validate executable metadata and initialize task contexts/page-table bindings.
- **Compiler Pipeline Boundary**: Phase 4 now owns the executable/runtime/ABI/metadata/assembler contract layers named in `OS3/compiler_pipeline.md`.

#### Track 6.16: Process Creation and Lifecycle Syscalls v1 (Implemented)
- **Process Identity**: Added free/runnable lifecycle state, PID convention, parent PID, exit status, wait target, and queue sidecars.
- **Spawn/Wait**: Added static `spawn` and `waitpid` behavior for predeclared executable images.
- **Lifecycle Compatibility**: Preserved `yield`, `sleep_until_tick`, `exit`, `getpid`, and `uptime`.

#### Track 6.17: Console Input and Minimal Shell Proof (Implemented)
- **Console Input Device**: Added host-fed word/ASCII input queue support with `console_in` and `console_in_ctrl` CSRs.
- **Tiny Command Loop**: The shell proof reads ASCII commands, launches static programs, waits for child exit, prints results, and exits.
- **Integrated Proof**: The acceptance image covers input wakeup, shell dispatch, spawn/wait, console output, timer preemption, and idle behavior together.

#### Track 6.18: Phase 4 Freeze and Acceptance (Implemented)
- **Scope Freeze**: The syscall table, trap-frame layout, process metadata, executable header v1, and page-table v1 contract are frozen for Phase 4.
- **Documentation Lock**: `OS3/PHASE4_COMPLETION_ROADMAP.md` is now canonical; deprecated implementation-plan files are no longer synchronized.
- **Regression Gate**: `test_multiwidth_vm`, `test_ternary_ir`, `test_ternary_lanes`, `test_native_ops`, and `test_numeric_workloads` pass.

#### Track 6.19: Advanced IR Expansion
- **Structural Node AST**: Replace string-based code emission with structured instruction node types (`IrInstr`) containing explicit source/destination operand payloads.
- **Type Auto-Widening Lattice**: Establish automatic numeric widening rules (`T1 < T5 < T10 < T20 < T40 < T50`) with implicit conversion node insertion.
- **Control Flow Graphs (CFG)**: Build explicit `BasicBlock` topologies supporting structured high-level closure builders (`ifTernary`, `whileLoop`, `forRange`).
- **Analysis & Optimization Pipeline**: Implement pre-lowering verification passes, explicit liveness analysis supporting automatic register release, and localized SSA optimization passes (Copy Propagation → Constant Folding → CSE → DCE → Strength Reduction).
- **Module Abstraction**: Implement `Function` calling conventions and `Module` containers for multi-function compilation.

#### Track 6.20: High-Level Source Language Frontend
- **Language Design**: Design a premium ML-style ternary-native language mapping types directly to precision layers (`t1..t50`, `l1..l50`, `vec<t20>`), exposing first-class three-way conditional blocks (`match sign(x)`), replacing booleans with ternary conditions, and embedding dedicated carryless logic operators (`|+|`, `|-|`, `/\`, `\/`, `~`).
- **Frontend Stages**: Implement an end-to-end driver orchestrating tokenization (Lexer), recursive descent parsing (Parser → AST), type checking/widening resolution, lowering to structural IR blocks, optimization passes, and backend assembly compilation.

### Critical Gaps to Bridge for `xv6` OS Execution
The VM now has the Phase 4 OS substrate: privilege state, CSR control registers, routed trap/interrupt entry, `ERET`, syscall traps, deterministic timer IRQs, `CSRRW` trap-save support, single-level user page tables, page-fault reporting, a bootable minimal kernel artifact, kernel-mediated console output/input, interrupt-disabled critical-section behavior, process lifecycle state, ready/wait metadata, timer-backed sleeping, lifecycle syscalls, static spawn/waitpid, an idle task, executable header v1, a minimal shell proof, a written ternary architecture contract, and the first ternary atomics/lock ABI proof. The next path is compiler/toolchain work against this frozen OS target contract.

#### A. Privilege Rings (Kernel vs. User Mode)
Implemented in substrate v1:
* **Ternary Protection Mode**: `-1` kernel, `0` supervisor/reserved, `+1` user.
* **Restricted Opcodes**: CSR writes and `ERET` are kernel-only and route protection faults from user mode.
* **V1 Protection**: User fetch/load/store obey IMEM/DMEM base-limit CSRs; kernel bypasses those v1 bounds.

#### B. Memory Management Unit (MMU) & Page Tables
Implemented as substrate v1:
* **Virtual Address Translation**: User fetch/load/store translate through separate single-level IMEM/DMEM page tables when `MMU_ENABLE` is set.
* **Protection**: PTEs carry user/read/write/execute permissions. Kernel mode uses physical IMEM/DMEM and bypasses the MMU.
* **Fault Metadata**: Page-fault CSRs record the virtual address and access class for precise handler diagnostics.

#### C. Preemptive Timer Interrupts (`IRQ`)
A deterministic timer IRQ exists now and can route to `TVEC` when interrupts are enabled.
* **Scheduler Context**: The minimal kernel now saves/restores `EPC`, `STATUS`, page-table CSRs, `r1-r26`, and user `sp` using the `SCRATCH`/`CSRRW` convention, then selects the next task through a tiny process table.

The definitive Phase 4 closure roadmap is `OS3/PHASE4_COMPLETION_ROADMAP.md`, and it is now complete. Compiler and source-language work should resume against this stable OS target contract.

### Critical Path

The current OS-oriented sequence prioritizes the machine contract before the full source language:
```
Phase 0/1 foundations -> Phase 6.1 ISA expansion -> Phase 6.2 VM tooling bridge ->
Phase 6.3 core VM OS substrate -> Phase 6.4 trap-save/ABI contract ->
Phase 6.5 MMU page tables -> Phase 6.6 timer two-task switch ->
Phase 6.7 minimal kernel bring-up -> Phase 6.8 syscall/console substrate ->
Phase 6.9 critical sections/process table seed -> Phase 6.10 architecture contracts ->
Phase 6.11 TLDR/TSTR atomics and lock ABI -> Phase 6.12 scheduler/process lifecycle ->
Phase 6.13 kernel services v1 -> Phase 6.14 device/event queues and kernel helpers ->
Phase 6.15 self-describing binary loader -> Phase 6.16 spawn/wait process lifecycle ->
Phase 6.17 console input and shell proof -> Phase 6.18 Phase 4 freeze [closed] ->
Phase 6.19 advanced IR -> Phase 6.20 source language frontend
```
Full language and optimization work now grows from the frozen ABI, trap/interrupt, memory-model, atomic, page-table, scheduler, executable-header, and syscall contracts.

Validation:

- End-to-end lowering verification passing multi-function AST input through lexing, parsing, lowering, and VM reference checks.
- Static liveness test verifications guaranteeing register reuse without leaks or manual release invocations.

Non-goals:

- Do not implement arbitrary memory-hard garbage collectors in the initial source language runtime.
- Do not add target-specific backends directly inside the source language lowering phase.

## Phase 7 (Next): High-Level Compiler Toolchain And SSA Universal Bus

Status: in progress; Phase 7 compiler/toolchain v1 is implemented.

Naming note: the older Phase 4 closure roadmap maps to this README's Phase 6
OS-capable machine work. The Phase 6.19 and Phase 6.20 items above are now
absorbed into Phase 7 so the next phase has one clear compiler/toolchain scope.

Goal: turn the frozen OS substrate into a real high-level compiler target. Phase
7 should not try to finish the xv6 alternative directly; it should make the
compiler, IR, object tooling, runtime stubs, and developer loop strong enough
that Phase 8 can implement the remaining xv6-class kernel/userland pieces in a
high-level systems language instead of hand-written `.tasm`.

Current implementation note:

- `ternary_compiler.h` now provides the Phase 7 v1 public API:
  `CompilerOptions`, structural `Module`/`Function`/`BasicBlock`/`Instr` IR
  types, `compileSource`, `linkModules`, runtime syscall ids, verifier helpers,
  optimizer hooks, and a register allocation helper surface.
- The v1 frontend supports explicit function signatures, inferred local
  bindings, mutable locals, arithmetic, direct calls, returns, `while pos(...)`,
  exhaustive `match sign(...)`, tuple swap lowering, pointer validity
  diagnostics, unsafe CSR/atomic intrinsics, and static executable linking
  through the existing assembler.
- `test_phase7_compiler` verifies source-to-SSA compilation, linking, VM
  execution, runtime syscall wrappers, call/loop lowering, pointer and width
  diagnostics, unsafe CSR/atomic lowering, verifier behavior, allocation
  surface, duplicate-symbol handling, and executable-header emission.

Inputs:

- `OS3/PHASE4_COMPLETION_ROADMAP.md`: Phase 4/README Phase 6 is closed at the
  frozen trap frame, syscall ABI, executable header v1, page-table v1, atomics,
  scheduler, console, static spawn/wait, and shell-proof boundary.
- `OS3/strategic_architecture.md`: the deferred strategic ideas now become
  compiler-facing concerns where appropriate: dynamic width/type-aware
  execution, future-aware binary metadata, 9-trit packing hooks, and tooling
  that does not paint the VM into a corner.
- `OS3/xv6.md`: the current microkernel already has privilege isolation, MMU
  protection, preemptive scheduling, ready/wait queues, console I/O, static
  spawn/wait, and a tiny shell. The gaps to an xv6 alternative are storage/files,
  dynamic `fork`/`exec`, and a user heap via `brk`/`sbrk`.
- `todo.md`: cache latency, TLB modeling, and true threaded multicore are future
  realism/stress tracks. Phase 7 should emit profiling and layout metadata that
  those tracks can consume later, but should not block the compiler on them.
- `OS3/language_optimizations.md`: Phase 7 should adopt Hindley-Milner type
  inference, SSA/CFG IR, graph-coloring allocation, LLVM-style IR as the shared
  artifact, zero-cost ownership/region safety, first-class `T1`/three-way
  branching, width polymorphism, typed memory-order annotations, and native swap
  lowering.

### Track 7.1: Structural SSA IR And Module Model

- Replace flat string-first IR emission with typed instruction nodes carrying
  explicit operands, defs, uses, effects, widths, and source spans.
- Add `Module`, `Function`, `BasicBlock`, and terminator concepts so the IR can
  represent multi-function programs before assembly lowering.
- Make SSA values explicit and introduce phi/block-argument support for
  structured branches and loops.
- Preserve the current assembly-emitting backend as the first lowering target so
  existing assembler and VM tests remain the oracle.
- Keep OS substrate operations first-class in IR: `SYSCALL`, `CSRR/CSRW/CSRRW`,
  `ERET`, `FENCE`, `TLDR`, `TSTR`, executable metadata, and ABI-visible calls.

Acceptance:

- Existing `test_ternary_ir` programs still lower and run through the VM.
- A multi-block SSA function with `brn`/`brz`/`brp`, a loop, and a function call
  lowers to valid `.tasm`.
- IR diagnostics report source spans and verifier failures before assembly.

### Track 7.2: Type System, Width Inference, And Safety Core

- Implement a Hindley-Milner style inference engine over numeric widths, lane
  widths, vectors, structs, arrays, pointers, and functions.
- Define the widening lattice `T1 < T5 < T10 < T20 < T40 < T50` and lane-family
  equivalents, with explicit conversion nodes inserted during typing.
- Make `T1` the language condition type and require exhaustive three-way
  handling where a value can be negative, zero, or positive.
- Add trit-width polymorphism for reusable functions over ternary precision.
- Define a pointer model with validity states for valid, unknown/uninitialized,
  and null, and require proven validity before dereference.
- Add region/ownership checks for stack, static, kernel, user, and borrowed
  references without introducing a garbage collector.
- Model shared references with typed memory ordering:
  relaxed `-1`, acquire-release `0`, and sequential `+1`.

Acceptance:

- Type inference can compile unannotated arithmetic, branches, function calls,
  and width-polymorphic helpers.
- Invalid dereferences, missing three-way match arms, unsafe shared mutation,
  and accidental width narrowing fail at compile time.
- Valid ownership and region proofs generate no runtime safety checks beyond the
  branch or trap instructions the program already needs.

### Track 7.3: Control Flow, Optimization, And Register Allocation

- Build explicit CFG construction for `if`, ternary `match`, `while`, `for`, and
  early returns.
- Add verifier and analysis passes: dominance, liveness, use-def chains, escape
  analysis, effect classification, and syscall/CSR side-effect barriers.
- Add optimizations in conservative order: constant folding, copy propagation,
  dead-code elimination, common-subexpression elimination, strength reduction,
  branch simplification, and swap recognition.
- Replace manual `release()`-driven register reuse with liveness-based
  allocation.
- Implement graph-coloring allocation for scalar registers and a compatible
  allocator for vector registers, then add stack spills/prologues/epilogues.
- Enforce the documented ABI: `r13-r18` arguments, `r13` return, `r1-r12`
  callee-saved, `r19-r24` caller-saved, `r25` link, `r26` stack pointer.

Acceptance:

- Programs with more live temporaries than physical registers spill and run
  correctly.
- Caller/callee-save preservation is verified by multi-function VM tests.
- Swap-like assignments lower to `SWAP` for register values and to the approved
  memory-safe fallback sequence only when needed.

### Track 7.4: Frontend Language MVP

- Add a lexer, parser, AST, and source map for a small ternary-native systems
  language rather than a full C++ frontend.
- MVP syntax should cover functions, blocks, local bindings, literals, structs,
  arrays, pointers, loops, three-way matches, calls, module imports, and
  explicit syscall/CSR/atomic intrinsics.
- Add inline assembly or intrinsic escape hatches only for kernel-grade
  operations that cannot yet be expressed safely.
- Keep the language ergonomic enough to write the next shell utilities,
  allocator code, filesystem code, and kernel helpers.

Acceptance:

- Source text can compile through lexer -> parser -> type checker -> SSA IR ->
  optimizer -> assembly -> executable header -> VM.
- A small high-level program can call console syscalls, perform loops and
  function calls, and run as a static user executable under the existing kernel.
- Compiler diagnostics are stable enough to guide users without inspecting IR.

### Track 7.5: Object, Linker, And Executable Tooling

- Extend assembler/object metadata so compiled modules carry symbols,
  relocations, data sections, executable headers, ABI version, stack hint,
  syscall ABI version, and optional debug/source metadata.
- Implement a static linker that combines compiler-produced modules into the
  current executable header v1 contract.
- Reserve but do not require richer future metadata for 9-trit packed sections,
  dynamic linking, profile data, and cache/TLB layout hints.
- Add a single driver command path for build/run so Phase 8 can compile many
  userland and kernel-support programs repeatably.

Acceptance:

- Two or more source modules can link into one static executable.
- Undefined symbols, duplicate definitions, ABI mismatches, invalid executable
  headers, and relocation overflow fail deterministically.
- Generated executables load through the same static loader path already proven
  by Phase 6.

### Track 7.6: Freestanding Runtime And Syscall Surface

- Provide startup code, stack-frame setup, panic/abort, integer formatting,
  minimal memory copy/set helpers, and syscall stubs for the frozen ABI.
- Provide high-level wrappers for console output/input, `yield`,
  `sleep_until_tick`, `exit`, `getpid`, `uptime`, static `spawn`, and `waitpid`.
- Add compile-time feature gates for future Phase 8 syscalls such as `open`,
  `read`, `write`, `close`, `brk`/`sbrk`, `fork`, and `exec`.
- Keep heap allocation minimal in Phase 7. A region or bump allocator for
  compiler acceptance tests is allowed; kernel-backed `malloc` waits for Phase
  8's `brk`/`sbrk`.

Acceptance:

- A high-level user program uses runtime syscall wrappers and exits with a
  visible status.
- Runtime helpers do not depend on host services or a garbage collector.
- Future syscall declarations can exist without pretending the kernel already
  implements them.

### Track 7.7: Compiler Validation And Developer Loop

- Add unit tests for lexer, parser, type inference, ownership, IR verification,
  optimization, register allocation, object metadata, linker behavior, and
  runtime helpers.
- Add end-to-end VM tests for compiled high-level programs, including an
  executable launched by the existing shell/static spawn path.
- Add golden assembly tests where ABI stability matters.
- Add instruction-count and size reporting so future cache/TLB/PGO work has
  baseline data.
- Keep a simple `tritc`/driver workflow as the seed of the eventual 30-second
  developer loop.

Phase 7 completion criteria:

- The repository can compile a nontrivial multi-function high-level source
  program into the frozen executable header v1 format and run it under the
  Phase 6 microkernel.
- The compiler has structural SSA IR, module/function lowering, type inference,
  width inference, zero-cost pointer/ownership checks, liveness-based register
  allocation, stack spills, calls, syscalls, atomics, and source diagnostics.
- Generated code obeys the frozen ABI, syscall contract, trap/CSR restrictions,
  page-table expectations, and executable loader contract.
- The remaining xv6 gaps are no longer blocked on hand-written assembly.

Phase 7 non-goals:

- Do not implement the block device, filesystem, `fork`, disk-backed `exec`, or
  kernel-backed `brk`/`sbrk`; those belong to Phase 8.
- Do not port POSIX libc, a full Unix shell, SQLite, curl, or other applications
  yet.
- Do not build a full C++ frontend or LLVM backend as the primary path.
- Do not add a garbage collector.
- Do not block Phase 7 on cache simulation, TLB emulation, threaded multicore,
  dynamic linking, demand paging, or FPGA/ASIC work.

## Phase 8 (The Platform Boot): Ternary xv6-Class Platform Boot

Status: in progress; Phase 8 platform substrate v1 is implemented.

Goal: use the Phase 7 compiler to turn the proven microkernel substrate into an
xv6-class ternary platform with storage, dynamic program loading, a user heap,
and a clean modular shell.

Current implementation note:

- `ternary_phase8.h` adds the first concrete Phase 8 platform layer: device-tree
  validation, deterministic 27-word block storage, a tiny inode filesystem,
  `T1` syscall-style result triples, user pointer state wrappers, shared
  acquire-release word helpers, per-process heap metadata, fork copy semantics,
  executable metadata attachment, and disk-backed `exec` modeling.
- `ternary_compiler.h` now reserves runtime syscall wrapper ids `12-21` for
  `open`, `close`, `read`, `write`, `stat`, `readdir`, `brk`, `sbrk`, `fork`,
  and `exec`, while keeping ids `1-11` unchanged.
- `test_phase8_platform` verifies the new platform contracts and compiler
  lowering for the Phase 8 syscall wrappers.

Accepted ternary-native policies:

- New kernel APIs use `T1` status convention: `r13 = -1/0/+1`,
  `r14 = payload`, and `r15 = errno/detail`.
- Runtime and compiler-facing pointer wrappers model `ptr<T, user,
  valid|null|unknown>`; the kernel still validates user addresses at syscall
  boundaries.
- Shared block-cache and scheduler/device state should use `shared<T, order>`
  semantics and lower through `TLDR`/`TSTR`/`FENCE.0` where concurrency matters.
- Current executable header v1, syscall ids `1-11`, trap-frame layout, page
  size, and PTE v1 remain compatible. Compact three-state permission PTEs are a
  documented future PTE v2 idea, not a Phase 8 replacement.

Major tracks still to push from substrate into the running assembly kernel:

- Boot the minimal kernel from a device-tree/HAL description for console, timer,
  block storage, page size, disk geometry, and root filesystem.
- Route the simulated block device through real VM/kernel service paths instead
  of only the host-side Phase 8 facade.
- Move the tiny filesystem into kernel-owned storage and implement path lookup,
  file descriptors, directory reads, and executable file loading from the mounted
  disk image.
- Add routed syscalls `12-21` in the assembly kernel with the `T1` result
  convention and user-pointer validation.
- Add per-process `heap_start`, `heap_break`, `heap_limit`, heap page ownership,
  `brk`/`sbrk`, and a freestanding allocator backing `malloc`/`free`.
- Add disk-backed `exec` and eager-copy `fork`; copy-on-write, demand paging,
  dynamic linking, networking, users/permissions, and native compiler
  self-hosting remain future work.
- Rebuild the shell in the Phase 7 high-level language with `ls`, `cat`,
  `echo/write`, `run/exec`, `forkwait`, `mem`, `pid`, `help`, and `exit`.
- Use existing `SWAP` only where the compiler or assembly proves
  register-to-register swaps are safe. Do not depend on hardware register-rename
  zero-cycle context switches for correctness.

Phase 8 completion criteria:

- The VM can boot a device-tree-described kernel image, mount a disk image, load
  user executables from the filesystem, allocate heap memory, run a high-level
  shell, launch child programs, and wait for their exit statuses.
- Storage, process creation, and heap allocation close the three current gaps
  between the Phase 6 microkernel and the xv6 alternative described in
  `OS3/xv6.md`.
- The full regression gate includes `test_phase8_platform`,
  `test_phase7_compiler`, `test_kernel`, `test_ternary_ir`,
  `test_multiwidth_vm`, `test_ternary_lanes`, `test_native_ops`, and
  `test_numeric_workloads`.

## Phase 9: Security and Post-Quantum Crypto Primitives

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

## Phase 10: FPGA/ASIC and Production Hardware Backends

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
