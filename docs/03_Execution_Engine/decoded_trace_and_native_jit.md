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
keeps a mapping writable and executable at the same time. NOP/MOV/COPY, the
integral T40 Add/Sub/TCmp/Mul subset, raw-valid T40 Neg/Abs, guarded dense
identity-physical T40 Load/Store, and hot internal branch control are emitted
inline. MMU-translated or sparse memory, tagged/fractional address views,
active reservations, non-local control, and unsupported operations retain
precise helper side exits.

The code-block inventory distinguishes these paths explicitly:
`VMNativeX64CodeBlock::direct_instruction_count` counts only micro-ops whose
architectural commit is emitted into x86-64, while
`helper_instruction_count` counts guarded arithmetic/memory fallbacks and
non-local control micro-ops that call a C++ helper. Runtime
`native_x64_jit_stats.direct_instructions` is likewise incremented only by
inline commits; helper instructions still count toward the architectural
instruction total.

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
requires each seven-sample coefficient of variation to stay below 3%. Native execution may
become a default only after it reaches at least 1.15x on two workloads and is
no more than 3% slower on the third. Decode-count reduction is diagnostic only,
not a performance acceptance gate. On x86-64, a failed wall-time gate returns
nonzero so a benchmark result cannot be mistaken for acceptance. The native
backend remains opt-in until the measured wall-time and stability contract is
repeatably satisfied on a controlled host. Candidate `45539f7` produced one
run that missed the CV ceiling only for arithmetic (3.294%) and an immediate
quiet-host confirmation that passed all three workloads (6.31x, 5.41x, and
1.44x, with native CVs of 1.25-1.45%). This proves the speed path but not yet
repeatable default-on stability, so the production default remains disabled.

Focused parity and safety coverage lives in `tests/test_vm_widths.cpp`,
including deterministic randomized differential execution, user-mode memory,
ASID cache separation, precise fallback, and W^X checks.
