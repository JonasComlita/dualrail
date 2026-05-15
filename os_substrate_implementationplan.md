# OS Roadmap Pivot and Next Sprint

## Summary
Pivot the roadmap from “language before OS” to “machine contract before both language and OS.” The next sprint should be an architecture-contract sprint: document the OS-critical ISA/ABI rules that the VM, assembler, IR compiler, future C/LLVM backend, and kernel will all depend on.

Use these defaults:
- Next sprint: architecture contract first.
- ABI: preserve the current split, with `r1-r12` callee-saved and `r13-r24` caller-saved/argument/return/scratch.
- Memory model: sequential consistency for single-core now; define `FENCE`/atomics as the future multicore boundary.

## Roadmap Updates
Update `implementationplan.md`:
- Mark Phase 2 ISA completion as verified, including the 15 new opcodes and passing VM/IR/lane/native/numeric tests.
- Insert a new Phase 3: **OS Architecture Contract**.
- Move current “Infrastructure and Performance” work after that as the next implementation phase.
- Move language/frontend work later, after ABI/trap/memory contracts are documented enough to compile correct kernel code.
- Add explicit Phase 3 outputs:
  - Interrupt/exception architecture.
  - Calling convention and ABI.
  - Memory model and `FENCE` semantics.
  - Atomic operation design.
  - Reset/boot contract.
  - Trit order/data layout/instruction alignment.
  - Floating-point sticky exception flags.

Update `README.md`:
- Reframe current Phase 6 as “OS-capable ternary machine and toolchain,” not primarily language frontend work.
- Add a Track 6.x architecture-contract track before OS/runtime/toolchain implementation tracks.
- Expand the xv6 gap section into an ordered dependency chain:
  1. Trap/interrupt/privilege model.
  2. ABI/calling convention.
  3. Memory model and atomics.
  4. Reset/boot sequence.
  5. Kernel-facing syscall/trap path.
  6. Minimal C/toolchain path.
  7. xv6-style kernel bring-up.

## Next Sprint
Sprint title: **Phase 3 — OS Architecture Contract**

Deliverables:
- Document privilege modes:
  - Kernel mode and user mode are required.
  - Optional supervisor/driver mode may be reserved, but not required for first kernel.
  - Reset starts in kernel mode with interrupts disabled.
- Document trap/interrupt architecture:
  - Separate synchronous traps from asynchronous interrupts.
  - Define trap vector, saved PC, cause code, status/mode save, and trap return.
  - Current `SYSCALL` becomes a user-to-kernel trap path in OS mode; sandbox buffer behavior remains VM/test-only.
- Document ABI:
  - `r0` remains zero.
  - `r1-r12` are callee-saved.
  - `r13-r18` are argument registers.
  - `r13` is primary return value.
  - `r19-r24` are caller-saved temporaries.
  - `r25` is link register.
  - `r26` is stack pointer.
  - `r27` remains trap/status.
- Document memory model:
  - Single-core execution is sequentially consistent.
  - Multicore behavior is not promised until atomics are added.
  - `FENCE` is architectural now, even if currently a VM no-op.
- Document atomic direction:
  - Prefer ternary CAS/LR-SC semantics later.
  - Reserve a trit status convention: success, value mismatch, reservation/collision failure.
- Document reset/boot:
  - VM reset may keep PC `0`.
  - Architecture should allow an implementation-defined reset vector for FPGA.
  - Boot begins in kernel mode with a valid stack contract before OS entry.
- Document data layout:
  - Trit 0 is least significant.
  - Instruction words are fixed 27-trit units.
  - Branch/call targets are instruction-word aligned.

## Verification
Acceptance criteria for this sprint:
- `implementationplan.md` clearly shows the pivot and new ordering.
- `README.md` explains why OS architecture comes before language/frontend expansion.
- Phase 2 is marked complete only after the known passing targets are recorded.
- The next implementation sprint can begin without re-deciding privilege, ABI, memory-model, boot, or syscall direction.
