**Ternary Architecture Contract Pack**
1. Memory model and atomics
2. Interrupt/exception architecture
3. ABI/calling convention
4. Trit order/data layout
5. Instruction encoding/alignment
6. Floating-point exception flags
7. Reset/boot contract
8. Kernel/device/syscall contract

The key idea: do not just copy binary ISAs. Use ternary where it carries real information.

**1. Memory Model**
Adopt a ternary-native memory-order trit:

- `-1`: relaxed
- `0`: acquire-release
- `+1`: sequential consistency

Expose this through `FENCE.-1`, `FENCE.0`, `FENCE.+1`, or equivalent assembler syntax.

For atomics, I like your proposed pair:

- `TLDR`: ternary load-reserved
- `TSTR`: ternary store-conditional

`TSTR` should return:

- `-1`: reservation collision
- `0`: value mismatch
- `+1`: success

That is genuinely better than binary LR/SC because retry loops can distinguish “someone touched this” from “the value is no longer what I expected.”

**2. Interrupt/Exception Architecture**
Keep the sign-based cause split we already started:

- positive cause: synchronous exception
- negative cause: interrupt
- zero/reserved: no cause or neutral state

Ternary-native additions worth documenting:

- privilege mode: `-1 kernel`, `0 supervisor/reserved`, `+1 user`
- trap vector mode trit:
  - `-1`: direct `TVEC`
  - `0`: cause-indexed vector table
  - `+1`: privilege/class-indexed vector banks

We should not implement the full interrupt controller yet, but we should define the contract before FPGA work.

**3. ABI**
Keep the current register plan:

- `r0`: zero
- `r1-r12`: callee-saved
- `r13-r18`: args
- `r13`: primary return
- `r19-r24`: caller-saved temps
- `r25`: link register
- `r26`: stack pointer
- `r27`: trap/status visibility

Add ternary-native ABI rules:

- condition values are `T1`, not booleans
- syscall/function status can use `-1 error`, `0 pending/retry`, `+1 success`
- stack frames should align to a ternary boundary, probably 9 words now, maybe 27 for kernel/process frames

**4. Data Layout**
Lock this down formally:

- trit 0 is least significant trit
- memory is word-addressed
- instruction words are fixed 27 trits
- pages are 27 words
- vectors default to 27 lanes
- PTEs are raw positional trit fields, not numeric T40 values

This is already mostly true in code; it needs to become contract.

**5. Instruction Encoding**
Keep fixed 27-trit instructions as the base ISA. It is simple and FPGA-friendly.

Future ternary-native compression idea:

- pack three 9-trit short instructions into one 27-trit word

That gives a real ternary code-density story without compromising the current decoder.

**6. FP Exception Flags**
Add sticky FP flags later, but define now.

A ternary-native flag can carry severity:

- `0`: clear
- `+1`: sticky warning/non-trapping exception
- `-1`: trapping or substituted-result exceptional condition

Flags to reserve:

- invalid
- divide by zero
- overflow
- underflow
- inexact

**7. Reset/Boot**
Current VM boot is fine for now:

- PC `0`
- kernel mode
- interrupts disabled
- MMU disabled
- SP top of DMEM

But FPGA contract should add:

- reset vector
- boot cause CSR
- boot ROM/header convention
- first kernel entry requirements

A ternary boot cause could be:

- `-1`: cold reset
- `0`: software reset
- `+1`: wake/resume/interrupt boot

**8. Kernel/Device/Syscall Contract**
We now have the first version:

- user `SYSCALL imm` traps
- id goes to `SYSCALL_ID`
- args in `r13-r18`
- return/status in `r13`
- kernel writes output through `console_out` / `console_ctrl`

Next we should document this as syscall ABI v1 before adding more devices.

**My proposed next action**
Create a new document, probably:

`OS3/TERNARY_ARCHITECTURE_CONTRACTS.md`

Then update `README.md` and `implementationplan.md` to say the next phase is not more feature work, but contract lock-in.

After that, the next implementation sprint should be:

**Ternary Atomics v1**
- `TLDR`
- `TSTR`
- reservation tracking in `VMState`
- memory-order trit
- spinlock test
- kernel lock/critical-section test

That is the next real OS unlock.