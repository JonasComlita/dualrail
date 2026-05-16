# Ternary Architecture Contracts

Status: contract draft for the OS-capable VM after Track 6.11.

This document locks the architectural rules that compiler, assembler, kernel,
debugger, FPGA, and future multicore work must share. It also names the
ternary-native extensions we want before implementing them or have now seeded in
the VM. The guiding rule is:
use a trit when the machine truly has three meaningful states; do not add a
third state just for decoration.

## 1. Memory Model And Consistency

### Current Contract

- The current VM is single-core and sequentially consistent.
- One architecturally executed instruction is one step.
- A load observes the latest prior store to the same word in program order.
- Kernel and user modes share the same physical memories; user virtual
  translation is applied only when `MMU_ENABLE` is set.
- `FENCE.-1`, `FENCE.0`, and `FENCE.+1` are architectural and compiler-visible,
  but they are VM no-ops while the machine is single-core and has no DMA/device
  reordering.

### Ternary-Native Memory Order Trit

Memory-order-bearing operations use one trit to encode ordering strength:

- `-1`: relaxed
- `0`: acquire-release
- `+1`: sequential consistency

This maps naturally to assembler spellings:

- `fence.-1`
- `fence.0`
- `fence.+1`

The same order trit is used by `TLDR` and `TSTR`. This is a real ternary win:
the ISA carries the three useful software memory-order tiers without a multi-bit
enum.

### Multicore Contract Direction

When multicore exists, the baseline should be:

- Data-race-free programs using acquire-release or stronger atomics behave
  sequentially consistently.
- Relaxed atomics are atomic but may reorder around ordinary loads/stores.
- Ordinary non-atomic data races have undefined architectural behavior.
- `fence.+1` is a global sequential-consistency barrier.
- `fence.0` orders prior release-side memory effects before later acquire-side
  effects.
- `fence.-1` is a compiler/annotation barrier only, unless an implementation
  chooses to give it stronger behavior.

## 2. Atomic Operations And Synchronization

### Implemented Atomic Pair

Implemented operations:

- `TLDR`: ternary load-reserved
- `TSTR`: ternary store-conditional

Behavior:

- `TLDR rd, rAddr, order`: load a word and establish a reservation for `rAddr`.
- `TSTR rd, rAddr, rNew, rExpected, order`: attempt to store `rNew` if the
  reservation is valid and the current memory value matches `rExpected`.

`TSTR` returns a ternary status in `rd`:

- `-1`: reservation collision or reservation lost
- `0`: value mismatch
- `+1`: success

This avoids a second compare in common retry loops. Binary LR/SC usually tells
software only success/fail; this ISA can distinguish "someone touched the
reservation" from "the value changed away from the expected value."

VM v1 details:

- Addresses are word addresses held in scalar registers.
- Reservations are tracked by translated physical DMEM word.
- Any scalar or vector store to the reserved physical word clears the reservation.
- Trap entry clears the reservation.
- Single-core memory-order behavior remains sequentially consistent; the order
  trit is preserved architecturally for compiler and future multicore use.

### Lock ABI Direction

The first kernel lock ABI exposes or should expose:

- `lock_try`: returns `+1` acquired, `0` busy, `-1` reservation collision/retry.
- `lock_acquire`: loops using `TLDR`/`TSTR` and `fence.0` or stronger.
- `lock_release`: stores unlocked with release ordering.

Current single-core kernel critical sections may use interrupt disable/restore
through `STATUS`; that is not a replacement for atomics once multicore exists.

## 3. Interrupt And Exception Architecture

### Current Contract

The current OS substrate implements:

- Privilege modes:
  - `-1`: kernel
  - `0`: supervisor/reserved
  - `+1`: user
- Synchronous traps and asynchronous interrupts route through `TVEC` when trap
  routing is enabled.
- Positive causes are synchronous exceptions.
- Negative causes are interrupts.
- `EPC` stores the interrupted virtual PC for user code.
- `CAUSE` stores the signed trap/interrupt cause.
- `STATUS` packs current mode, interrupt enable, previous mode, and previous
  interrupt enable.
- Trap entry disables interrupts and enters kernel mode.
- `ERET` restores previous mode/interrupt state and resumes at `EPC`.
- `CSRRW sp, scratch, sp` is the trap-save primitive.

### Trap Entry Convention

While running user code:

- `SCRATCH` holds the current task kernel context/stack pointer.
- User `sp` is in `r26`.

First trap instruction:

```asm
csrrw sp, scratch, sp
```

After this:

- `sp` points to the kernel context/stack.
- `SCRATCH` contains interrupted user `sp`.

Before `ERET`, the handler performs the reverse swap.

### Vectoring Direction

The current VM uses direct `TVEC`. Future FPGA/OS work should reserve a trap
vector mode trit:

- `-1`: direct mode, all traps jump to `TVEC`.
- `0`: cause-indexed table, target is `TVEC + abs(CAUSE)`.
- `+1`: class/bank mode, interrupts and exceptions use separate banks.

This gives a compact ternary expression of three useful vectoring policies.

## 4. Calling Convention And ABI

### Register Roles

- `r0`: zero
- `r1-r12`: callee-saved
- `r13-r18`: argument registers
- `r13`: primary return value
- `r14-r18`: additional return values when needed
- `r19-r24`: caller-saved temporaries
- `r25`: link register
- `r26`: stack pointer
- `r27`: legacy trap/status visibility, not a normal GPR

### Function Calls

- `CALL` and `CALLR` write `LR = PC + 1`.
- `RET` jumps to `LR`.
- `JMPR` performs indirect control transfer without modifying `LR`.
- Stack grows downward unless a future ABI document explicitly changes it.
- Function-call stack alignment is 9 words.
- Kernel task contexts use the fixed 32-word layout already defined in the VM.

### Ternary Status ABI

When a routine has a status result, prefer a `T1` status:

- `-1`: error/failure
- `0`: retry/pending/neutral
- `+1`: success

This is the same shape used by the proposed `TSTR` result and should become a
consistent kernel ABI pattern.

### Syscall ABI v1

Current routed syscall ABI:

- User executes `SYSCALL imm`.
- Kernel receives `CAUSE = OS_CAUSE_SYSCALL`.
- `SYSCALL_ID = imm`.
- Arguments are in `r13-r18`.
- Return/status is in `r13`.
- Syscall handler advances `EPC` before `ERET`.

Implemented minimal syscall ids:

- `1`: write integer
- `2`: newline
- `3`: clear console output

## 5. Endianness And Data Layout

### Trit Order

- Trit 0 is the least significant trit.
- This is little-endian ternary at the trit level.
- All scalar pack/unpack, PTE layout, instruction fields, debugger display, and
  future file/network formats must preserve this convention unless explicitly
  documented as external byte serialization.

### Memory Layout

- IMEM and DMEM are separate Harvard address spaces.
- Addresses are word addresses, not byte addresses.
- The native VM word is `T40`.
- `T50` is extended precision, not the default machine word.
- Page size is 27 words.
- Default vector length is 27 lanes.
- Page-table entries are raw T40 storage fields, not numeric T40 values.

### PTE Layout

Low trits:

- trit 0: valid/present
- trit 1: user accessible
- trit 2: read
- trit 3: write
- trit 4: execute
- trit 5 and above: physical page number

Permission trits use `+1` for enabled/present and `-1` for absent/disabled.
`0` remains reserved/invalid for low flag trits.

## 6. Instruction Encoding Density And Alignment

### Current Contract

- Base ISA instructions are fixed 27-trit words.
- PC addresses instruction words.
- Branch, call, trap, and `EPC` values are instruction-word addresses.
- Branch offsets are in instruction words.
- User fetch under MMU translates virtual instruction-word addresses to
  physical instruction-word addresses.

The fixed 27-trit format is intentionally simple for decode and FPGA bring-up.
The binary storage detail (currently 54 bits / 7 bytes when serialized) is not
the architectural address unit.

### Future Compression Direction

If code density becomes important, prefer a ternary-native compressed extension:

- pack three 9-trit short instructions into one 27-trit instruction word.

This keeps fetch alignment at 27 trits while allowing dense short instruction
groups. Do not introduce variable-length base instructions before FPGA decode
is stable.

## 7. Floating-Point Exception Flags

### Current Contract

- Synchronous hard faults use `r27`/trap routing.
- There are no sticky floating-point exception flags yet.

### Reserved Sticky Flags

Reserve future CSRs for IEEE-style sticky flags:

- invalid operation
- divide by zero
- overflow
- underflow
- inexact

Ternary-native flag severity should use one trit per flag:

- `0`: clear
- `+1`: sticky non-trapping warning
- `-1`: trapping or substituted-result exceptional condition

This lets numerical software distinguish "inexact but usable" from "exceptional
and policy-relevant" without requiring every event to become a trap.

## 8. Reset And Boot Sequence

### Current VM Contract

On VM reset:

- PC is `0`.
- Privilege is kernel.
- Interrupts are disabled.
- MMU is disabled.
- SP is initialized to the top of DMEM.
- Trap routing is disabled until the kernel writes `TVEC`.

The minimal kernel artifact currently boots at PC `0`, initializes `TVEC`,
`SCRATCH`, page-table CSRs, timer CSRs, `EPC`, and `STATUS`, then enters user
mode through `ERET`.

### FPGA Direction

FPGA and hardware targets should define:

- reset vector address
- boot ROM or boot image format
- SRAM initialization contract
- first kernel entry ABI
- trap-vector setup requirements

Reserve a boot-cause trit:

- `-1`: cold reset
- `0`: software reset
- `+1`: wake/resume/interrupt boot

## 9. Kernel, Device, And Scheduler Contract

### Current Kernel Substrate

Implemented:

- bootable minimal kernel assembly artifact
- routed syscall path
- console output CSRs
- deterministic timer preemption
- interrupt-disabled critical-section semantics
- `TLDR`/`TSTR` atomics and lock ABI proof
- 32-word task context
- process table seed:
  - `proc_count`
  - `current_proc`
  - `proc_table[]`

### Next Kernel Direction

The next implementation layer should add:

- scheduler policy beyond round-robin table selection
- process lifecycle states
- blocking/wakeup paths for syscalls and devices
- richer devices beyond the current console buffer

## 10. Decisions Locked By This Document

- Ternary order/status trits are meaningful only where all three states are
  useful.
- Single-core VM behavior remains sequentially consistent.
- Future memory ordering uses `-1 relaxed`, `0 acquire-release`, `+1 sequential
  consistency`.
- Future store-conditional returns `-1 collision`, `0 value mismatch`,
  `+1 success`.
- Positive `CAUSE` means synchronous exception; negative `CAUSE` means
  interrupt.
- Privilege remains `-1 kernel`, `0 supervisor/reserved`, `+1 user`.
- Trit 0 is least significant.
- Base instructions are fixed 27-trit words.
- Page size remains 27 words for MMU v1.
- Syscall ABI v1 uses `SYSCALL_ID`, `r13-r18` arguments, and `r13` return.
