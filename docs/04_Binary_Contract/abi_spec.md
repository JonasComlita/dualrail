# Calling Convention and ABI

Source of truth: `ternary_isa.h` (register constants), `SYSCALL_MANIFEST.json` (syscall ABI), `ternary_compiler_codegen.h` (compiler enforcement).

---

## Calling Convention

### Register Roles

| Register | Role | Notes |
|----------|------|-------|
| r0       | Zero | Hardwired; writes discarded |
| r1–r12   | Callee-saved | Callee must save/restore if used |
| r13      | Arg 0 / Return value | First argument in; return value out |
| r14      | Arg 1 | Second argument |
| r15      | Arg 2 | Third argument |
| r16      | Arg 3 | Fourth argument |
| r17–r24  | Caller-saved scratch | May be clobbered by callee |
| r25      | Link Register | Written by CALL; read by RET |
| r26      | Stack Pointer | Grows **downward**; word-aligned |
| r27      | Trap register | Read-only from ISA |

### Argument Passing

- Up to 4 arguments pass in **r13–r16**.
- Additional arguments are passed on the stack (pushed before the call, popped by the caller after).
- Arguments larger than one word (e.g., structs) are split across consecutive registers or passed by pointer.

### Return Values

- Single-word return: **r13**.
- The callee writes the return value to r13 before executing `RET`.

### Stack Frame Layout

```
  high address
  ┌─────────────────────┐  ← caller's SP before call
  │   arg4, arg5, ...   │  (if >4 args; pushed by caller)
  ├─────────────────────┤
  │ callee-saved regs   │  (r1–r12 that callee uses)
  ├─────────────────────┤
  │   local variables   │
  ├─────────────────────┤
  │   ...               │
  └─────────────────────┘  ← callee's SP (r26)
  low address
```

Stack grows **downward** (SP decrements on push). SP is always **word-aligned** (one word = one T40 register = one DMEM slot).

### Call Sequence

```asm
; Caller:
MOV  r13, <arg0>
MOV  r14, <arg1>
CALL target_fn          ; r25 ← PC+1; PC ← target_fn

; Callee prologue:
; (save callee-saved regs if needed)
; do work
; MOV r13, <return value>

; Callee epilogue:
RET                     ; PC ← toLong(r25)
```

---

## Syscall Calling Convention

Source: `SYSCALL_MANIFEST.json`

```
Caller-side (user process):
  1. CSRW csr_syscall_id, <service_id>
  2. MOV r13, <arg0>
  3. MOV r14, <arg1>
  4. MOV r15, <arg2>
  5. MOV r16, <arg3>
  6. SYSCALL

Kernel-side return (via ERET):
  r13 = status  (0=OK, 2=blocked, negative=error)
  r14 = payload (service-specific return value)
  r15 = detail  (service-specific extra data)
```

`status == 2` means the process is **blocked** (rescheduled; will be resumed when the resource is ready).

---

## Vector ABI

Vector operations use a separate vector register file (v0–v7). Calling convention for functions that use vectors:
- Vectors are NOT saved by the standard callee-saved convention.
- Functions that modify vector registers must document this explicitly.
- `VLEN Rd` writes the current vector length (number of lanes) into `Rd`.

---

## Stack Hints in Link Options

When linking TCL programs, `LinkOptions::stack_hint_words` specifies the initial stack allocation (default: 24 words). The kernel uses this from `APP_MANIFEST.json` (`stack_words` field per app) to allocate stack when spawning processes.

---

## Binary Object Format

Compiled TCL programs produce an `ObjectModule` containing:

| Field | Description |
|-------|-------------|
| `assembly` | Textual ternary assembly (`.tasm`) |
| `ssa` | SSA IR `Module` |
| `symbols` | Symbol name → instruction address map |
| `function_order` | Ordered function list for linking |
| `function_refs` | Cross-function reference graph (for dead-stripping) |

The linker (`LinkResult`) assembles all modules into a single `ExecutableImageHeader` + `AssemblyResult` ready for the VM.

---

## Executable Image Header

After linking, the final image contains:

```
ExecutableImageHeader:
  boot_entry    — PC value at start (default: address of "main")
  text_words    — instruction count
  data_words    — static data word count
```

This maps to the `.tboot` payload's `boot_entry` + `program[]` + `data_words[]` fields.
