# Traps, Interrupts, CSRs, and Privilege

Source of truth: `ternary_isa.h` — `TrapCode`, `OS_CAUSE_*`, `PrivilegeMode`, CSR constants.

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

**Transition into kernel:** any trap automatically saves `PC` → `CSR epc`, saves current mode → `status`, and jumps to `CSR tvec`.

**Return from kernel:** `ERET` restores mode from saved status and jumps to `CSR epc`.

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

On clean execution r27 = `TRAP_NONE` (fault_valid = 0).

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

Valid CSR indices: 0–46 (`CSR_MAX_ID = CSR_BLOCK_WORDS = 46`).

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
1. Set block_index (device index)
2. Set block_addr (block address)
3. Set block_count / block_words
4. Set block_cmd (read=T_NEG, write=T_POS, sync=T_ZER)
5. Poll block_status
```

---

## Console I/O Protocol

```
Read:  CSRR console_in       → character (or 0 if empty)
Write: CSRW console_out, ch  → output one character
Flush: CSRW console_ctrl, 1  → flush output buffer
```
