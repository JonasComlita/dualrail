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

- Up to 4 scalar ABI words pass in **r13–r16**.
- Additional words are passed in the caller-owned outgoing stack area (pushed
  before the call, reclaimed by the caller after the call).
- The compiler-facing function contract is explicitly versioned as
  `trit.compiler.function-abi.v2` (version 2). Struct and array parameters do
  **not** split their payload across registers: each crosses the boundary as
  one caller-owned pointer word. The register/stack cursor advances by one
  word for that pointer, and the callee treats it as the aggregate base
  address for word-wise loads and stores.
- Compiler function ABI v3 is an opt-in profile,
  `trit.compiler.function-abi.v3`. It keeps the v2 scalar and aggregate
  parameter rules and reserves the first ABI word for a hidden caller-owned
  structure-return (`sret`) pointer whenever the function's result is a
  struct or array. User arguments begin at the next register/stack word.

### Return Values

- Scalar returns use **r13**. A wide T50 value occupies the ABI pair
  **r13–r14**.
- The callee writes the return value to the ABI return register(s) before
  executing `RET`.
- Aggregate-valued returns have no representation in function ABI v2 and are
  rejected by the compiler. Source code must pass a caller-owned aggregate
  output pointer explicitly until a future ABI version defines an `sret`
  contract.
- Under function ABI v3, an aggregate-returning callee receives the hidden
  `sret` pointer in the first ABI word (`r13`, or the corresponding outgoing
  stack word after register words are exhausted). It copies the result into
  that caller-owned storage and returns with no scalar payload in `r13`.
  The caller owns the storage lifetime through the call and may pass its
  address to subsequent aggregate loads/copies.

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

Vector operations use a separate vector register file (v0–v7). The hardware
register roles are described in [vector_abi.md](vector_abi.md), but they are
not a compiler function boundary contract. Function ABI v2 intentionally
defines no vector argument, return, or spill representation. A source
function with a first-class `vec<T>` parameter/return/value is therefore
rejected with a diagnostic naming the selected compiler profile; it is never
scalarized or emitted through an AST fallback. A future version must define
lane width, register assignment, VLEN preservation, fault state, and stack
spill layout together before these boundaries can be enabled.

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

The linker (`LinkResult`) assembles all modules into a single
`ExecutableImageHeaderV2` plus `AssemblyResult` ready for the v2 VM. Selecting
compiler function ABI v3 changes the object/link profile and sret lowering,
but does not silently rewrite the executable header: the image envelope and
kernel loader remain v2 until a corresponding header version is implemented.
Linkers must therefore match object profile metadata exactly and expose the
selected profile separately from the image envelope.

---

## Executable Image Header

After linking, the final image contains:

```
ExecutableImageHeaderV2:
  executable_version, function_abi_version, syscall_abi_version
  isa_version, required_features
  boot_entry    — PC value at start (default: address of "main")
  text_words    — instruction count
  data_words    — static data word count
```

The header also records exact text/data words, stack words, scalar width,
base-page size, flags, and a checksum. The production linker, assembler,
loader, VM, kernel, and image builders accept only v2. v1 executable and
live-storage compatibility is intentionally absent; the standalone offline
migrator converts preserved inputs from `trit-v1-final`.
