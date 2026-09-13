# Register Map

Sources of truth: `ARCHITECTURE_MANIFEST.json` for CSR IDs/access and
`ternary_isa.h` for register constants and instruction fields.

---

## General-Purpose Register File (r0–r26)

| Register | Alias | ABI Role | Notes |
|----------|-------|----------|-------|
| `r0`     | `zero` | Hardwired zero | Writes are silently discarded |
| `r1`–`r12` | — | Callee-saved | Must be preserved across calls |
| `r13`    | `a0` / `ret` | Argument 0 / Return value | First arg in, return value out |
| `r14`    | `a1` | Argument 1 | Second arg |
| `r15`    | `a2` | Argument 2 | Third arg |
| `r16`    | `a3` | Argument 3 | Fourth arg |
| `r17`–`r24` | — | Caller-saved | Scratch; may be clobbered by callee |
| `r25`    | `lr` | Link Register | Written by `CALL`; read by `RET` |
| `r26`    | `sp` | Stack Pointer | Grows downward (decrements on push) |

## Trap Register (r27)

| Register | Role | Notes |
|----------|------|-------|
| `r27`    | Trap register | Written by VM on any fault; **read-only from ISA** |

`r27` is a two-trit ternary fault record:
- `trit[0]` = `fault_valid`: 0 = no fault, +1 = fault is live
- `trit[1]` = `fault_class`: −1 = DIV_ZERO, 0 = MEM_FAULT, +1 = ILLEGAL_OP

---

## Vector Register File (v0–v7)

Eight vector registers (`VECTOR_REGISTER_COUNT = 8`).
Each register is a dynamic array of `TernaryValue` elements; length set by `vm.vector_length`.
Width is selected per-instruction via the `func` field (T1, T5, T10, T20, T40, T50).

| Register | Description |
|----------|-------------|
| `v0`–`v7` | General-purpose vector registers |

---

## AI Accumulator

A single wide accumulator register used by `ACLR`, `ALOAD`, `AADD`, `ASUB`, `AMUL`, `ASTORE`.
Stores `TernaryValue` at T40 precision. Used for `VDOT` / `VMAC` dot-product accumulation.

---

## CSR File (Control & Status Registers)

Accessed via `CSRR` / `CSRW` / `CSRRW`. IDs 0–52. The generated
[architecture contract](../04_Binary_Contract/architecture_v2.md) is the
authoritative per-CSR read/write privilege table.

| CSR ID | Name | Purpose |
|--------|------|---------|
| 0 | `epc` | Exception Program Counter (saved PC on trap) |
| 1 | `cause` | Trap cause code |
| 2 | `status` | Privilege status |
| 3 | `tvec` | Trap vector base address |
| 4 | `scratch` | Kernel scratch register |
| 5 | `cycle` | Cycle counter |
| 6 | `timer_reload` | Timer reload value |
| 7 | `timer_counter` | Timer current count |
| 8 | `timer_enable` | Timer enable flag |
| 9 | `timer_pending` | Timer interrupt pending |
| 10 | `user_imem_base` | User instruction memory base |
| 11 | `user_imem_limit` | User instruction memory limit |
| 12 | `user_dmem_base` | User data memory base |
| 13 | `user_dmem_limit` | User data memory limit |
| 14 | `syscall_id` | Syscall service ID (set before `SYSCALL`) |
| 15 | `mmu_enable` | Enable/disable MMU |
| 16 | `user_imem_ptbr` | User IMEM page table base register |
| 17 | `user_imem_pages` | User IMEM page count |
| 18 | `user_dmem_ptbr` | User DMEM page table base register |
| 19 | `user_dmem_pages` | User DMEM page count |
| 20 | `page_fault_addr` | Address that caused a page fault |
| 21 | `page_fault_access` | Access type: −1=fetch, 0=load, +1=store |
| 22 | `console_out` | Write character to console |
| 23 | `console_ctrl` | Console control (flush, etc.) |
| 24 | `console_in` | Read character from console |
| 25 | `console_in_ctrl` | Console input control |
| 26 | `mouse_x` | Mouse X coordinate |
| 27 | `mouse_y` | Mouse Y coordinate |
| 28 | `mouse_btn` | Mouse button state |
| 29–32 | `gpu_x1/y1/x2/y2` | GPU draw region coordinates |
| 33 | `gpu_color` | GPU draw color |
| 34 | `gpu_cmd` | GPU command (draw rect, clear, etc.) |
| 35 | `gpu_page` | Framebuffer page select |
| 36 | `gpu_draw_base` | Framebuffer draw base address |
| 37 | `gpu_mode` | GPU mode (text/pixel) |
| 38–40 | `sprite_x/y/attr` | Sprite position and attributes |
| 41 | `block_index` | Block device index |
| 42 | `block_addr` | Block device address |
| 43 | `block_cmd` | Block device command |
| 44 | `block_status` | Block device status |
| 45 | `block_count` | Read-only block-device capacity in blocks |
| 46 | `block_words` | Read-only words per storage block |
| 47 | `power_control` | Kernel-only host power/reboot request |
| 48 | `isa_version` | Read-only ISA version discovery |
| 49 | `isa_features` | Read-only supported-feature word |
| 50 | `mmu_base_page_words` | Read-only base-page geometry |
| 51 | `mmu_superpage_words` | Read-only superpage geometry |
| 52 | `asid` | Read-only current address-space identifier |

---

## Register Encoding

A 3-trit register field covers r0–r26 (27 values).
The field stores `register_index − 13` (balanced, from −13 to +13).
`REG_FIELD_OFFSET = 13`.

Example: r13 is stored as balanced trit `0`, r0 is stored as balanced trit `−13`.

---

## Quick ABI Summary

```
Callee-saved:  r1–r12          (must save/restore if used)
Caller-saved:  r13–r24         (scratch, may be clobbered)
Args in:       r13, r14, r15, r16
Return value:  r13
Link register: r25             (CALL writes here)
Stack pointer: r26             (grows downward)
Zero register: r0              (always reads as zero)
Trap register: r27             (written by VM, not ISA)
```
