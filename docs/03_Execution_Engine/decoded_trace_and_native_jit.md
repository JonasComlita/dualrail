# Decoded Trace Executor and Native JIT

The portable hot-path backend is the **decoded trace executor**. The old
`TraceJit` C++ spelling remains a transition alias, but this backend does not
emit native code.

Each decoded instruction is represented by a host-neutral `VMMicroOp` with:

- an architectural PC and one-instruction accounting unit;
- an explicit trap point;
- register and result guards;
- read, write, or no-memory effect;
- possible side exits for unsupported operations, invalid operands/results,
  translation failures, memory faults, branches leaving a trace, and
  generation changes.

Trace cache keys include ISA version, required features, ASID, PC, instruction
memory generation, privilege and MMU state, user mappings/page-table roots,
executable-mapping generation, and MMU generation. User and MMU-enabled traces
use the same translation helpers as the interpreter and exit precisely to the
faulting PC when a guard fails.

## x86-64 backend status

`VMExecutionBackend::NativeX64Jit` is dependency-free and supports Windows x64
and System V x86-64 calling conventions. It emits ABI-correct code into RW
memory, changes the mapping to RX, flushes the instruction cache, and never
keeps a mapping writable and executable at the same time. NOP/MOV and
canonical T40 COPY, the integral T40 Add/Sub/TCmp/Mul subset, raw-valid T40
Neg/Abs, guarded dense identity-physical T40 Load/Store, and hot internal
branch control, immediate `CALL` link/branch commits, and dynamic
`RET`/`CALLR`/`JMPR` control are emitted inline.
The `CALL` lowering guards the link-register destination pair and side-exits
before mutation if a live T50/L50 pair would be invalidated. Width-qualified COPY uses the portable
`readView` conversion helper (and commits through the same native block), while
MMU-translated or sparse memory, tagged/fractional address views, active
reservations, and unsupported operations retain precise helper side exits.

Dynamic control has a deliberately narrow direct contract. `RET`, `CALLR`, and
`JMPR` require the selected register view to be canonical `T40`, the physical
word to be a valid T40 payload, and the represented value to be exactly
integral and within the host `int` PC range. The generated guard mirrors
`VMState::validateControlTarget`: kernel targets must be in instruction
memory; non-kernel targets must be in the user executable range when MMU is
off, while MMU-on virtual targets remain the portable fetch/permission
boundary. A failed mode, payload, integrality, privilege, or target-range
guard restores the faulting PC and returns before changing PC, LR, or any
destination, so the portable interpreter observes the original instruction.
Valid `CALLR` targets are retained before the link-register write, which makes
`CALLR r25` aliasing safe; the LR destination-pair guard also side-exits for a
live T50/L50 pair rather than partially invalidating it. A successful dynamic
commit writes the validated target PC, records one cycle and one direct native
instruction, and leaves the trace for the next dispatcher entry.

The code-block inventory distinguishes these paths explicitly:
`VMNativeX64CodeBlock::direct_instruction_count` counts only micro-ops whose
architectural commit is emitted into x86-64, while
`helper_instruction_count` counts guarded arithmetic/memory fallbacks and
malformed or otherwise ineligible control micro-ops that call a C++ helper. Runtime
`native_x64_jit_stats.direct_instructions` is likewise incremented only by
successful inline commits; guard exits and helper instructions still count
toward the architectural instruction total, while a guard-exit instruction is
completed by the portable dispatcher after the native block returns.

Arithmetic is T40 floating ternary arithmetic: add/subtract align and round
mantissas, multiply normalizes a product, and all operations must preserve
overflow/underflow encodings. Inline Add/Sub/Mul/TCmp decode only genuinely
integral normalized T40 values and side-exit before commit for fractional,
special, or out-of-range values. Inline Neg/Abs transform the valid raw T40
mantissa while preserving its exponent, so valid fractional payloads remain
exact. Portable T40 arithmetic remains the authority for every guarded case.
Guarded memory must perform privilege/MMU
translation, sparse-page access, memory-fault routing, and reservation
invalidation. These semantics are not equivalent to unchecked host integer
instructions, so the guarded subset and helper paths remain intentional. A helper validates its
operands, sets the faulting PC, and returns a side exit before the portable
interpreter resumes; focused tests compare status, trap code, PC, cycle count,
and instruction count for invalid arithmetic and out-of-range memory.

`test_execution_backends_benchmark` reports seven-run median wall time after two warmups
for arithmetic, guarded-memory, and branch workloads; the acceptance gate also
requires each seven-sample coefficient of variation to stay below 3%. Every
repeat contributes a deterministic FNV fingerprint of status, PC, steps,
cycles, backend counters, and the final register payload; the gate requires
all seven fingerprints to match on the controlled host. Native execution may
become a default only after it reaches at least 1.15x on two workloads and is
no more than 3% slower on the third, with repeatability and CV gates passing.
Decode-count reduction is diagnostic only, not a performance acceptance gate.
On x86-64, a failed wall-time, repeatability, or stability gate returns nonzero
so a benchmark result cannot be mistaken for acceptance. The controlled-host
contract is now archived in the ignored
`build/native-x64-jit-acceptance/native_x64_jit_acceptance.v1.json` report, so
the host runtime selects NativeX64Jit by default on x86-64. Non-x86 builds
retain CachedBlockInterpreter, and `TosRuntimeConfig::execution_backend` keeps
explicit backend selection available; the benchmark emits a
`native_x64_repeatability_record` key/value line for evidence collectors.
The same executable runs a separate `dynamic_control` workload and emits a
`native_x64_dynamic_control_record` line containing direct commits, portable
side exits, repeatability, and the controlled-host stability result. This
workload is an acceptance check for the dynamic lowering, but it does not
alter the historical three-workload default-enable speed gate.
The accepted controlled report records three independent processes, 21
matching dynamic-control fingerprints, cross-process median CV below 3%, and
the primary three-workload speed gate passing on the pinned host.

## Controlled-host acceptance artifact

The benchmark emits one `native_x64_sample version=1` record for each measured
sample. The records include the workload/backend, timing, architectural
status, PC, steps, cycles, result, fingerprint, direct/helper lowering counts,
code-block count, and portable side exits. The records remain additive to the
human-readable timing lines and do not change any acceptance threshold.

`tools/collect_native_x64_jit_acceptance.py` is the owner-side collector. It
builds the Release benchmark target, verifies the x86-64 executable and its
SHA-256, records commit/compiler/OS/CPU/affinity/power-profile metadata, and
runs three independent processes pinned to one verified logical CPU. Each
process retains its two warmups and seven measured samples. A passing run
requires every process gate, all 21 dynamic-control native fingerprints to
match, and cross-process native median CV below 3%. Only then does it promote
the versioned `trit.native_x64_jit_acceptance.v1` JSON report; failed attempts
remain under the ignored build diagnostics directory. NativeX64Jit remains
opt-in until that artifact is accepted by the primary integration agent.
This proves the speed path but not yet repeatable default-on stability, so the
production default remains disabled.

Focused parity and safety coverage lives in `tests/test_vm_widths.cpp`,
including deterministic randomized differential execution, user-mode memory,
ASID cache separation, precise fallback, and W^X checks.
