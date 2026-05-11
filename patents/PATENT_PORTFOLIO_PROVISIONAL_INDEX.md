# Provisional Portfolio Index

## Filing-Support Draft

This document is a technical index for a provisional patent packet. It is not
legal advice and is not a substitute for review by a registered patent
practitioner. Its purpose is to give the filing packet a single coherent spine
and to identify which disclosures should be treated as primary claim pillars,
supporting embodiments, drawings, and source-code exhibits.

## Proposed Portfolio Title

System and Method for Balanced Ternary Computation with Native Three-Way
Predication, Width-Typed Arithmetic, and Cross-Platform Device Execution

## Technical Improvement Framing

The invention produces a measurable reduction in the instructions,
execution-unit invocations, or pipeline-disrupting control events required to
perform three-way conditional selection, single-trit ternary
multiply-accumulate, or vector arithmetic with per-lane fault isolation,
compared with conventional binary instruction-set implementations of the same
operations.

The filing should not rely on broad statements such as "no binary ISA has
this." The stronger framing is the specific combination of balanced ternary
predicate semantics, width-typed register and memory behavior, explicit
conversion boundaries, lane-local execution semantics, and cross-platform
device-safe implementation support.

## Independent Pillars

### 1. TSEL/VSEL Three-Armed Predicate Selection

Primary draft: `PATENT_1_TSEL_VSEL.md`

Discloses a scalar and vector selection mechanism in which one first-class
balanced ternary predicate value selects one of three complete source operands.
The selected value is copied without arithmetic coercion, normalization,
rounding, or implicit type conversion. The R5 instruction encoding places five
register fields inside one fixed-width instruction word.

Key technical effects:

- single-instruction three-way register selection;
- no branch predictor event;
- no binary flags register dependency;
- no secondary compare instruction;
- tag-preserving scalar writeback; and
- lane-wise vector selection with local fault masks.

### 2. T1 Multiply-Accumulate Without Multiplier Invocation

Primary draft: `PATENT_2_T1_MAC.md`

Discloses a multiply-accumulate path in which the weight operand is an
architecturally visible single balanced trit. The execution unit maps the
weight to add, subtract, or skip, and accumulates into a wider accumulator
without invoking a multiplier for the T1 weight application.

Key technical effects:

- replaces T1 weight multiplication with conditional add/subtract/skip;
- preserves a wide accumulation path for neural and lattice workloads;
- gives the ISA explicit knowledge that the weight is T1; and
- distinguishes algebraic T1 multiplication from ternary lattice operations
  such as TMIN.

### 3. Two-Family Encoding With Explicit CVT Boundary

Primary draft: `PATENT_3_TWO_FAMILY_CVT.md`

Discloses a separation between numeric formats having arithmetic meaning and
lane formats having transport or wire meaning. Numeric values use base-3
positional semantics. Lane values use two-bit-per-trit packing. Crossing the
boundary requires an explicit `cvt.src.dst` instruction or equivalent
conversion operation.

Key technical effects:

- prevents hidden reinterpretation between arithmetic and lane encodings;
- makes precision loss and family crossing visible in the instruction stream;
- prevents binary arithmetic on lane payloads through structural type
  separation; and
- supports CPU, GPU, FPGA, and ASIC mappings without mixing representation
  contracts.

### 4. Vector Fault-Valid and Fault-Class Masks

Primary draft: `PATENT_4_VECTOR_FAULT_MASKS.md`

Discloses per-lane fault-valid and fault-class masks for vector execution.
Faulting lanes write deterministic typed-zero values and record a lane-local
fault, while non-faulting lanes continue execution and preserve useful
results.

Key technical effects:

- avoids whole-vector trap for a single lane fault;
- separates whether a fault occurred from what class of fault occurred;
- supports divide-by-zero, invalid encoding, memory fault, illegal operation,
  and wrong-tag conditions; and
- provides scalar and vector-compatible fault semantics.

### 5. Cross-Platform Device-Safe Ternary Lane Backend

Primary draft: `PATENT_5_DEVICE_SAFE_TERNARY_BACKEND.md`

Discloses a device-safe lane backend for two-bit-per-trit values. The same raw
helpers are callable from scalar CPU code, SIMD batch code, CUDA/SYCL-style
kernel wrappers, and hardware-oriented models. The backend provides canonical
encode/decode, validity, carryless operations, full-adder lane operations,
compare, min, max, and raw64/raw128 batch interfaces.

Key technical effects:

- avoids divergent CPU/GPU/FPGA implementations of ternary lane semantics;
- preserves identical invalid-state handling across backends;
- creates a reusable device-side ALU layer for lane operations; and
- supports later kernel launch wrappers without rewriting arithmetic.

### 6. 27-Trit ISA and R5 Overlay

Primary draft: `PATENT_7_27_TRIT_ISA_R5.md`

Discloses a 27-trit instruction word using a single-trit format discriminator
and a fixed opcode field. The R5 overlay reuses existing R-type field space to
decode five register operands for TSEL/VSEL-style instructions without adding a
new instruction format trit or requiring a second instruction word.

Key technical effects:

- preserves fixed-width 27-trit instruction fetch;
- uses a balanced ternary format discriminator;
- extracts instruction fields through two-bit-per-trit slices;
- supports R-type, I-type, B-type, and R5 overlay decoding; and
- reserves opcode space by keeping widths in suffix or func fields instead of
  creating per-width opcodes.

Recommended exhibits:

- `ternary_isa.h`
- `ternary_asm.h`
- `ternary_vm.h`
- `test_multiwidth_vm.cpp`

## Supporting and Dependent Embodiments

### Portable T50 Arithmetic and UInt128 Preferred Embodiment

Supporting draft: `PATENT_6_PORTABLE_UINT128_T50.md`

This should not be presented as a standalone claim to a generic two-limb
integer. The stronger role is a dependent or supporting disclosure for portable
fifty-trit balanced ternary arithmetic across CPU, CUDA, SYCL, VM snapshot,
and hardware-model targets. The two-limb `UInt128 { lo, hi }` layout is the
preferred embodiment of a stable payload representation.

### 243-Trit Cache-Line Geometry

Disclose a memory/cache geometry in which ternary data is grouped around
243-trit units, matching a power-of-three structure and allowing predictable
alignment of ternary words, lanes, and instruction groups. If benchmark data is
added later, this can be promoted into an independent or stronger dependent
claim.

Suggested drawing:

```text
---------------------------------------------------------------+
| 243-trit line = 3^5 trits                                    |
| aligned groups of T1/T5/T10/T20/T27/T40/T50 payload regions   |
+---------------------------------------------------------------+
```

### VLEN Abstract Vector Length

Disclose a vector length query instruction or capability value allowing the
same vector ISA to map onto AVX2, AVX-512, CUDA, SYCL, FPGA block RAM, or ASIC
lanes without baking a physical vector width into opcode names.

Suggested drawing:

```text
same program -> vlen -> backend-reported lane count
             -> AVX2 / AVX-512 / CUDA / SYCL / FPGA / ASIC
```

### Ternary Heap With TCMP Routing

Disclose a priority queue or routing structure in which a single ternary
compare result selects among negative, zero, and positive paths, including
three-child heap traversal. The emphasis should be the reduced instruction
count and depth compared with binary routing on the same processor.

Suggested drawing:

```text
binary heap depth:  log2(N)
ternary heap depth: log3(N)

N = 10^3, 10^6, 10^9 comparison-depth table
```

### VMState Allocator Backend Substitution

Disclose a VM state whose memory allocation is abstracted behind host, CUDA
unified memory, SYCL shared memory, or future hardware-backed allocators while
leaving the fetch-decode-execute loop and payload layouts unchanged.

Suggested drawing:

```text
VMState
  |
  +-- Host allocator
  +-- CUDA managed allocator
  +-- SYCL shared allocator
  +-- FPGA/ASIC memory map
```

### Two-Pass Word-Addressed Ternary Assembler

Disclose the assembler as a support embodiment: first pass builds label
addresses, second pass resolves PC-relative word offsets, and output is loaded
directly into word-addressed ternary instruction memory.

## Drawing Checklist

- TSEL scalar data path: predicate register controlling a 3:1 mux.
- R5 instruction layout:
  `[fmt:1 | opcode:4 | rd:3 | rCond:3 | rNeg:3 | rZero:3 | rPos:3 | reserved:7]`.
- VSEL lane diagram showing per-lane three-source selection.
- Comparison diagram: binary compare plus branch sequence versus ternary
  predicate plus select.
- Tag-preserving writeback diagram copying `{mode, payload}` unchanged.
- T1 MAC add/subtract/skip execution path with wide accumulator.
- Two-family conversion firewall between numeric formats and lane formats.
- Vector fault-valid and fault-class masks with continued execution.
- Device-safe raw lane backend feeding scalar, SIMD, CUDA/SYCL, and FPGA paths.
- Portable T50 payload layout with optional internal fast paths.
- VLEN backend substitution diagram.
- Ternary heap depth comparison diagram.
- 243-trit cache-line geometry diagram.
- VMState allocator substitution diagram.

## Source-Code Exhibit Checklist

Attach or preserve copies of the following files with the provisional filing
materials:

- `ternary_isa.h`
- `ternary_backend.h`
- `ternary_lanes.h`
- `ternary_vm_state.h`
- `ternary_native_ops.h`
- `ternary_uint128.h`
- `ternary_vm.h`
- `ternary_asm.h`
- `ternary_kernel.h`
- `ternary_gpu_kernels.h`
- `ternary_device_allocators.h`
- `ternary_ir.h`
- `test_multiwidth_vm.cpp`
- `test_ternary_lanes.cpp`
- `test_ternary_ir.cpp`
- `benchmark_architecture.cpp`
- `README.md`
- `optimization_baseline.md`
- `architecture_benchmark_results.md`
- `tuning_results.md`

## Filing Packet Order

1. Portfolio index and summary.
2. Primary drafts: Patents 1 through 5 and Patent 7.
3. Supporting embodiment draft: Patent 6.
4. Drawings.
5. Source-code exhibits.
6. Benchmark and tuning exhibits.

## Risk Notes

- Avoid claiming balanced ternary arithmetic itself.
- Avoid relying on broad "no binary ISA has this" language.
- Avoid presenting a generic two-limb integer as the invention.
- Avoid abstract "software conditional" or "mathematical selection" framing.
- Tie each claim to instruction count, execution-unit selection, control-flow
  reduction, memory-layout stability, lane-local fault isolation, or
  cross-backend execution effects.
- Use source-code exhibits as reduction-to-practice support.
