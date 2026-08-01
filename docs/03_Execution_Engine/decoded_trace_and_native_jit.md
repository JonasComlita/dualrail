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
keeps a mapping writable and executable at the same time. NOP/MOV/COPY and
hot internal branch control are emitted inline; arithmetic and guarded memory
retain precise helper side exits until their data paths are lowered inline.

`test_execution_backends_benchmark` reports seven-run median wall time after two warmups
for arithmetic, guarded-memory, and branch workloads. Native execution may
become a default only after it reaches at least 1.15x on two workloads and is
no more than 3% slower on the third. Decode-count reduction is diagnostic only,
not a performance acceptance gate. On x86-64, a failed wall-time gate returns
nonzero so a benchmark result cannot be mistaken for acceptance. The current
direct branch-loop stage passes the wall-time contract on the measured host,
but this does not yet authorize the native backend as the default until the
remaining helper-backed operations are covered.

Focused parity and safety coverage lives in `tests/test_vm_widths.cpp`,
including deterministic randomized differential execution, user-mode memory,
ASID cache separation, precise fallback, and W^X checks.
