# Phase 4 Completion Roadmap

Status: Phase 4 OS-substrate closure implemented and frozen.

This roadmap fixes the boundary of Phase 4 so it stops growing every time the
kernel uncovers another prerequisite. Phase 4 is complete when the VM can boot a
tiny protected OS, schedule user programs, block and wake them on timer/device
events, load static user binaries through a documented executable contract, and
run a minimal console/shell acceptance scenario.

Phase 4 is not the full source language, full compiler, full filesystem, GUI,
network stack, fork/COW process model, demand-paged virtual memory, or multicore
hardware. Those start after this closure point.

## Inputs From Strategic Architecture

`OS3/strategic_architecture.md` names the foundational work that prevents later
rewrites. The Phase 4 closure plan maps those foundations to concrete end
states:

- Unified trap entry and a 32-word trap frame: implemented.
- MMU translation hook and page tables: implemented.
- CSR privilege shield: implemented.
- Immutable syscall ABI: Phase 4 syscall ids are frozen through `waitpid`.
- O(1) scheduler data structures: ready/wait metadata is implemented for the
  Phase 4 proof.
- Dead-man timer/asynchronous preemption: implemented enough for Phase 4 through
  deterministic timer IRQs and preemptive switching.
- Dynamic width execution: useful later, but not required to close the OS
  substrate.
- 9-trit sub-word packing: document as future ABI/storage work, not a Phase 4
  blocker.
- Self-describing binaries: executable header v1 and loader helpers are
  implemented.

## Inputs From Compiler Pipeline

`OS3/compiler_pipeline.md` orders the toolchain from low level to high level:
executable/deployment, runtime, ABI/object format, linker, object metadata,
assembler, assembly, codegen, IR, parser, lexer, and language.

Phase 4 should finish the target contract for the low-level half:

- executable/deployment shape
- kernel runtime services
- ABI and syscall contract
- object/executable metadata seed
- assembler support needed to express kernel and user images

Phase 5 then builds the compiler-facing implementation on top of that contract:
structured assembly emission, codegen, IR, optimizer, parser, lexer, and the
source language. This keeps the language from designing against a moving OS
target.

## Fixed Remaining Tracks

### Phase 4.11 - Device/Event Queues And Kernel Authoring Helpers - DONE

Goal: stop scheduler and device work from being encoded as one-off assembly
paths.

Deliverables:

- Add event/wait queue metadata for non-timer waits.
- Convert timer sleep wakeups onto the same wait-channel/event mechanism.
- Add a console/input readiness event, even if the host device is minimal.
- Add run-queue or ready-list metadata so scheduler selection no longer scans
  only by accident.
- Add kernel-authoring helpers for task-context offsets, process metadata
  offsets, syscall stubs, and common trap-save/restore sequences.

Acceptance:

- A task can block on timer and a second task can block on a device/event.
- Waking an event makes the blocked task runnable without polling.
- The integrated kernel image stays green with less duplicated hand-written
  save/restore and metadata code.

### Phase 4.12 - Self-Describing Binary Loader And Object ABI Seed - DONE

Goal: make user programs first-class loadable artifacts instead of hard-coded
assembly regions.

Deliverables:

- Define a tiny executable header with magic/version, ABI version, entry virtual
  PC, text/data page counts, stack hint, required syscall ABI version, and flags.
- Extend assembler output or metadata so tests can obtain text image, data
  image, symbols, and executable header information together.
- Add loader helpers that construct task contexts, page tables, and initial
  user stacks from a static executable image.
- Reserve relocation and richer object metadata fields, but keep v1 static.

Acceptance:

- At least two user programs are loaded through executable metadata rather than
  hard-coded context literals.
- Loader setup produces the same isolation guarantees as the current manual
  page-table setup.
- Bad executable headers fail deterministically before user execution.

### Phase 4.13 - Process Creation And Lifecycle Syscalls v1 - DONE

Goal: move from a fixed demo process table to a tiny process model.

Deliverables:

- Add process IDs, parent IDs, exit status, and a free/runnable process table
  slot lifecycle.
- Add `spawn` for static executable images and `waitpid` for child completion.
- Keep `fork`, copy-on-write, dynamic linking, and demand paging out of scope.
- Preserve existing lifecycle syscalls: `yield`, `sleep_until_tick`, `exit`,
  `getpid`, and `uptime`.

Acceptance:

- A kernel task can spawn two static user programs, wait for one to exit, and
  continue scheduling the other.
- Exited tasks do not run again and their status is observable through `waitpid`.
- Process-table exhaustion returns a ternary status instead of corrupting state.

### Phase 4.14 - Console Input And Minimal Shell Proof - DONE

Goal: prove an interactive OS loop without starting the full filesystem.

Deliverables:

- Add a host-backed console input device path or input CSR/buffer.
- Add blocking console read semantics using the event queue from Track 4.11.
- Add a tiny command loop that can dispatch built-in commands and launch static
  programs through the Track 4.12 loader path.
- Keep parsing intentionally small: whitespace tokenization is enough.

Acceptance:

- The VM can feed input to the kernel, wake a blocked reader, parse a command,
  launch a static program, and print output.
- The shell idles without consuming user-task time while waiting for input.
- Console output, console input, sleep, spawn, wait, exit, and timer preemption
  coexist in one acceptance image.

### Phase 4.15 - Phase 4 Freeze And Acceptance - DONE

Goal: stop adding OS substrate scope and declare the VM a stable compiler target.

Deliverables:

- Freeze the Phase 4 syscall table, ABI, trap-frame layout, process metadata
  layout, executable header v1, and page-table v1 contract.
- Update `README.md` and architecture contracts to mark Phase 4 closed. The
  deprecated implementation-plan files are no longer canonical.
- Add one integrated acceptance test that boots the kernel, handles traps,
  schedules multiple processes, blocks on timer/device events, loads static
  binaries, runs a shell command, and exits/waits cleanly.
- Run the full regression gate.

Acceptance:

- `test_multiwidth_vm`
- `test_ternary_ir`
- `test_ternary_lanes`
- `test_native_ops`
- `test_numeric_workloads`

Phase 5 may start now that this freeze has passed.

## Phase 4 Completion Criteria

Phase 4 is complete. The acceptance state is:

- Boot starts in kernel mode, configures trap routing, MMU, timer, console, and
  process metadata, then enters user mode through `ERET`.
- User code cannot write privileged CSRs, execute `ERET`, or access unmapped or
  unauthorized memory without a routed trap.
- Timer IRQs preempt user tasks and preserve all architectural context.
- The scheduler supports runnable, running, blocked, sleeping, and exited tasks,
  with ready/wait queues rather than only fixed toggles.
- User programs are loaded through a documented executable image contract.
- Kernel syscalls cover console output, console input, yield, sleep, exit,
  getpid, uptime, spawn, and waitpid.
- A minimal shell can block for input, dispatch static programs, and continue
  after child exit.
- The ABI, syscall table, executable header, page-table format, trap-frame
  layout, and process metadata layout are documented and tested.

## Explicit Non-Goals For Phase 4

- Full C compiler or premium ternary source language.
- Full linker, dynamic loader, shared libraries, or DWARF.
- Filesystem, disk block device, networking, GUI, or framebuffer.
- `fork`, copy-on-write, signals, pipes, or POSIX compatibility.
- Multicore memory model enforcement beyond the documented order trit.
- Demand paging, TLBs, swapping, or multi-level page tables.

## Next Phase Boundary

After Phase 4.15, Phase 5 starts from a stable target and should follow the
low-to-high order from `OS3/compiler_pipeline.md`:

1. executable/object tooling refinement
2. linker/static relocation support
3. structured assembly emission
4. backend codegen
5. IR and optimization
6. parser, lexer, and source language
