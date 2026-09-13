# Traps, Interrupts, CSRs, and Privilege

Sources of truth: `ARCHITECTURE_MANIFEST.json` for CSR IDs and access classes,
`ternary_isa.h` for trap/privilege constants, and `ternary_vm_state.h` for trap
entry, return, and CSR side effects.

---

## Privilege Modes

```cpp
enum class PrivilegeMode : int8_t {
    Kernel     = T_NEG,   // -1  (most privileged)
    Supervisor = T_ZER,   //  0
    User       = T_POS,   // +1  (least privileged)
};
```

The VM starts in **Kernel mode**. User processes run in User mode. Mode is stored in the `status` CSR.

**Transition into kernel:** when trap routing has been enabled by a valid write
to `tvec`, a trap saves `PC` → `epc`, saves current mode and interrupt state,
and jumps to `tvec`. Address zero is a valid vector; routing is represented by
a separate architectural flag rather than inferred from `tvec != 0`.

**Return from kernel:** `ERET` requires an active routed trap, restores the saved
mode and interrupt state, jumps to `epc`, and clears the handled `r27` record.
An `ERET` with no active trap is illegal.

There is one hardware trap save frame. A synchronous fault inside an active
handler is a terminal nested trap: it does not overwrite the original `epc`,
`cause`, or previous privilege state.

---

## The Trap Register (r27)

r27 is a two-trit ternary fault record written by the VM on any fault. It is **read-only from the ISA**.

```
r27.trit[0] = fault_valid:
  0  → no fault (TRAP_NONE)
 +1  → fault is live; trit[1] is meaningful

r27.trit[1] = fault_class:
 -1  → TRAP_DIV_ZERO     (division by zero in DIV)
  0  → TRAP_MEM_FAULT    (out-of-range LOAD or STORE)
 +1  → TRAP_ILLEGAL_OP   (unknown opcode or 0b11 trit in word)
```

On clean execution r27 = `TRAP_NONE` (fault_valid = 0). Routed syscalls and
timer interrupts are events rather than legacy faults, so they leave r27 at
`TRAP_NONE` and identify themselves through `cause`. A routed synchronous fault
sets r27 while its handler runs; successful `ERET` clears it.

---

## OS Trap Causes (`CSR cause`)

When a trap fires, a **cause code** is written to `CSR cause`:

| Constant | Value | Description |
|----------|-------|-------------|
| `OS_CAUSE_ILLEGAL_INSTRUCTION` | 1  | Undefined opcode or malformed word |
| `OS_CAUSE_FETCH_FAULT`         | 2  | PC out of IMEM bounds |
| `OS_CAUSE_LOAD_FAULT`          | 3  | LOAD address out of DMEM bounds |
| `OS_CAUSE_STORE_FAULT`         | 4  | STORE address out of DMEM bounds |
| `OS_CAUSE_PROTECTION_FAULT`    | 5  | Privilege violation |
| `OS_CAUSE_DIV_ZERO`            | 6  | Division by zero |
| `OS_CAUSE_SYSCALL`             | 7  | SYSCALL instruction (normal trap) |
| `OS_CAUSE_FETCH_PAGE_FAULT`    | 8  | MMU page fault on instruction fetch |
| `OS_CAUSE_LOAD_PAGE_FAULT`     | 9  | MMU page fault on data load |
| `OS_CAUSE_STORE_PAGE_FAULT`    | 10 | MMU page fault on data store |
| `OS_CAUSE_TIMER_IRQ`           | −1 | Timer interrupt (asynchronous) |

Positive causes = synchronous exceptions. Negative causes = asynchronous interrupts.

---

## Page Fault Access Type (`CSR page_fault_access`)

| Value | Meaning |
|-------|---------|
| `OS_PAGE_ACCESS_FETCH` = −1 | Instruction fetch |
| `OS_PAGE_ACCESS_LOAD`  =  0 | Data load |
| `OS_PAGE_ACCESS_STORE` = +1 | Data store |

---

## SYSCALL Protocol

```
1. Write service ID to CSR syscall_id (CSR index 14)
2. Set arguments: r13 = arg0, r14 = arg1, r15 = arg2, r16 = arg3
3. Execute SYSCALL instruction
4. Kernel handles trap, executes service, returns via ERET
5. Read results: r13 = status, r14 = payload, r15 = detail
```

`status == 2` means **blocked** (the kernel has not yet fulfilled the request; process is descheduled).

See `SYSCALL_MANIFEST.json` for the full service ID table.

---

## CSR Access Instructions

| Instruction | Operation |
|-------------|-----------|
| `CSRR Rd, imm` | `Rd ← CSR[imm]` |
| `CSRW imm, Rs1` | `CSR[imm] ← Rs1` |
| `CSRRW Rd, Rs1, imm` | Atomic: `Rd ← CSR[imm]; CSR[imm] ← Rs1` |

Valid CSR indices are 0–52. Each CSR has generated read and write access levels;
there is no numeric privilege cutoff. The complete table is generated in
[`architecture_v2.md`](../04_Binary_Contract/architecture_v2.md).

User mode retains its console, input, graphics, and architecture-discovery
interfaces. Block I/O, power control, timer/MMU state, page-table roots, and
trap-frame mutation require Kernel mode. Writes to architecturally read-only
CSRs are illegal in every mode.

---

## Timer Interrupt

The VM has a built-in timer controlled via CSRs:

| CSR | Purpose |
|-----|---------|
| `timer_reload`   | Reload value (set this to configure period) |
| `timer_counter`  | Current count (decrements each cycle) |
| `timer_enable`   | Enable/disable timer |
| `timer_pending`  | Pending interrupt flag (write 0 to clear) |

When `timer_counter` reaches 0, `OS_CAUSE_TIMER_IRQ` fires if the timer is enabled.

---

## GPU / Framebuffer Protocol

Apps draw by writing to GPU CSRs (no DMA, no memory-mapped framebuffer pointer):

```
1. Set gpu_x1, gpu_y1, gpu_x2, gpu_y2 (region)
2. Set gpu_color
3. Set gpu_cmd (draw rect, clear, blit, etc.)
4. Read gpu_page to know which page is active
```

The framebuffer dimensions are set in the `.tboot` header (`framebuffer_width`, `framebuffer_height`).

---

## Block Device Protocol

```
1. Optionally read block_count and block_words to discover device geometry
2. Set block_index (device block index)
3. Set block_addr (DMEM or IMEM transfer address)
4. Set `block_cmd`: 1=read block to DMEM, 2=write DMEM to block, 3=read block
   to IMEM, 4=flush backing storage; a negative value clears status
5. Poll block_status
```

---

## Console I/O Protocol

```
Read:  CSRR console_in       → character (or 0 if empty)
Write: CSRW console_out, ch  → output one character
Flush: CSRW console_ctrl, 1  → flush output buffer
```
