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
and System V x86-64 calling conventions. Its first stage emits an ABI-correct
per-trace thunk into RW memory, changes the mapping to RX, flushes the
instruction cache, and invokes the precise portable micro-op helper. No mapping
is writable and executable at the same time.

This is an intentionally conservative bootstrap, not the final optimizing
backend. Arithmetic and guarded memory semantics work through the helper, but
individual micro-ops are not yet lowered inline to x86-64. The native backend
therefore remains disabled by default.

`test_execution_backends_benchmark` reports seven-run median wall time after two warmups
for arithmetic, guarded-memory, and branch workloads. Native execution may
become a default only after it reaches at least 1.15x on two workloads and is
no more than 3% slower on the third. Decode-count reduction is diagnostic only,
not a performance acceptance gate.

Focused parity and safety coverage lives in `tests/test_vm_widths.cpp`,
including deterministic randomized differential execution, user-mode memory,
ASID cache separation, precise fallback, and W^X checks.
