# Scheduler quantitative acceptance

`next_scheduler_acceptance` is the compiled-kernel scheduler/WAIT gate. It
compiles the current `kernel.trit` and executes the tier-one queue, PID index,
generation/membership invalidation, wait queues, timer wheel, IPC, window, and
architectural WAIT paths in the VM. The gate is included by `ci_production`.

The scaling driver records two warmups followed by seven measured samples for
one and 100 runnable slots. It compares medians using raw architectural VM
cycles, requires coefficient of variation below 3%, and accepts only a
0.8-1.2 ratio. The checked-in curve normalizes the one-runnable median to 1;
the executable enforces the corresponding raw-cycle ratio. This deterministic
metric avoids treating host wall time as scheduler evidence.

Sustained 1,000-dispatch traces require strict FIFO order, a maximum wait of
one queue rotation (100 dispatches at the production-sized fixture), and no
starvation. Stale queue generations, membership invalidation, repeated
enqueue attempts, FIFO channel wakeup, and sleep, futex, IPC, window, waitpid,
and timer completion paths verify exactly-once runnable publication. An empty
queue must enter architectural `WAITING`; only `resumeFromEvent()` may resume
it, after which a timer wake selects the sleeping process.

The machine-readable contract and scaling curve are
[`SCHEDULER_ACCEPTANCE_SCHEMA.json`](../../SCHEDULER_ACCEPTANCE_SCHEMA.json) and
[`scheduler-acceptance.v1.json`](../../benchmarks/reference/scheduler-acceptance.v1.json).
