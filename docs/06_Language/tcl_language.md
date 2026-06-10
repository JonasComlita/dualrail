# TCL — Ternary C-Like Language

Source of truth: `TCL_Spec_1.0.md`, `tritc.cpp`, `ternary_compiler_*.h`
Self-hosted compiler: `tcl_lexer.trit`, `tcl_parser.trit`, `tcl_backend.trit`, `tcl_asm.trit`

---

## Overview

TCL (Ternary C-Like) is a statically-typed, systems programming language designed for the ternary stack. It compiles to ternary assembly (`.tasm`) and is the implementation language for:

- The OS kernel (`kernel.trit`)
- All system libraries (`ulib.trit`, `os_sdk.trit`, `libwidget.trit`)
- All bundled apps
- Itself (self-hosted compiler in `tcl_*.trit`)

File extension: `.trit`

---

## Type System

### Primitive Types

| Type | Description | Trit width |
|------|-------------|-----------|
| `t1` | 1-trit integer {−1, 0, +1} | 1 |
| `t5` | 5-trit integer [−121, +121] | 5 |
| `t10` | 10-trit float | 10 |
| `t20` | 20-trit float | 20 |
| `t40` / `trit` | 40-trit float (native word) | 40 |
| `t50` | 50-trit float (wide) | 50 |
| `int` | Alias for `t40` | 40 |
| `float` | Alias for `t40` | 40 |
| `bool` | Alias for `t1` (−1=false, +1=true, 0=unknown) | 1 |
| `str` | String (pointer + length) | N/A |
| `void` | No value | — |

### Compound Types

| Syntax | Description |
|--------|-------------|
| `*T` | Pointer to T |
| `[N]T` | Fixed-size array of N elements of type T |
| `struct { field: T; ... }` | Record type |
| `fn(T1, T2) -> T3` | Function type |

---

## Language Features

### Variable Declaration

```tcl
let x: int = 42;
let mut y: float = 3.14;
let ptr: *int = &x;
```

`let` — immutable binding. `let mut` — mutable binding.

### Functions

```tcl
fn add(a: int, b: int) -> int {
    return a + b;
}

// External (syscall or import)
extern fn write(fd: int, buf: *byte, len: int) -> int;
```

### Control Flow

```tcl
// Ternary if-else (three-way)
if x > 0 {
    // positive
} elif x == 0 {
    // zero
} else {
    // negative
}

// Loop
loop {
    if done { break; }
}

// While
while condition {
    // ...
}

// For
for i in 0..n {
    // ...
}
```

### Ternary-Specific Features

```tcl
// Three-way comparison operator → t1
let cmp: t1 = a <=> b;   // -1, 0, or +1

// Three-way select
let val = tsel(cond, neg_branch, zero_branch, pos_branch);

// Trit access
let t: t1 = x.trit[i];
```

### Structs

```tcl
struct Point {
    x: float;
    y: float;
}

let p: Point = Point { x: 1.0, y: 2.0 };
let q = p.x + p.y;
```

### Unsafe Blocks

```tcl
unsafe {
    // raw pointer manipulation
    // CSR access
    // inline assembly
}
```

Functions that touch hardware or raw memory must be marked:
```tcl
unsafe fn csrr(csr_id: int) -> int { ... }
```

### Inline Assembly

```tcl
asm {
    CSRR r13, 14     ; read syscall_id CSR
    CSRW 22, r13     ; write to console_out
}
```

---

## Compile Pipeline

```
.trit source file
  ↓ tritc (CLI: tritc.cpp)
  ↓ Lexer    → token stream
  ↓ Parser   → ModuleAst
  ↓ Type inference + checking
  ↓ IR lowering → SSA Module
  ↓ Optimizer (mem2reg, CSE, const-fold, ...)
  ↓ Register allocator
  ↓ Code generator → .tasm text
  ↓ Linker + Assembler → .tboot or VM-loadable image
```

### Using the Host Compiler

```powershell
# Compile a .trit file to assembly
tritc myapp.trit -o myapp.tasm

# Compile and link to a loadable image
tritc myapp.trit --link -o myapp.tboot
```

### In-OS Compiler

`/bin/tcc` (compiled from `apps/tcc.trit`) is the in-OS TCL compiler. Invokable from the shell:
```sh
tcc /tmp/hello.trit -o /tmp/hello
/tmp/hello
```

---

## Standard Library (`ulib.trit`)

The standard library provides:
- Memory: `malloc`, `free`, `realloc`, `memset`, `memcpy`
- String: `strlen`, `strcpy`, `strcmp`, `sprintf`, `sscanf`
- Math: `pow3`, `abs`, `min`, `max`, ternary `sin`/`cos`/`exp`/`ln`
- Collections: `vec` (dynamic array), `hmap` (hash map from `ulib_hmap.trit`)
- I/O: `print`, `println`, `read_line`, file wrappers
- Process: `fork`, `exec`, `exit`, `getpid`

Import with:
```tcl
import "ulib.trit";
```

---

## Module System

```tcl
// Export a symbol
export fn my_function() -> int { ... }

// Import from another .trit file
import "ulib.trit";
import "os_sdk.trit";
```

The linker resolves cross-module references using the `symbols` map in `ObjectModule`. Dead functions are stripped when `LinkOptions::dead_strip_functions = true` (roots: `["main"]`).

---

## Self-Hosted Compiler

The self-hosted compiler pipeline (in `tcl_*.trit`) mirrors the host C++ compiler:

| File | Role |
|------|------|
| `tcl_token.trit` | Token definitions |
| `tcl_lexer.trit` | Lexer |
| `tcl_ast.trit` | AST node types |
| `tcl_parser.trit` | Parser |
| `tcl_type.trit` | Type checker |
| `tcl_infer.trit` | Type inference |
| `tcl_ir.trit` | IR representation |
| `tcl_backend.trit` | Code generator |
| `tcl_asm.trit` | Assembler |
| `tcl_frontend.trit` | Driver |
| `tcl_pointer_dataflow.trit` | Pointer analysis |

This self-hosted compiler is **invoked by `/bin/tcc`** inside the OS.

---

## Current Status

- ✅ Language spec (`TCL_Spec_1.0.md`)
- ✅ Host C++ compiler pipeline (`tritc.cpp` + `ternary_compiler_*.h`)
- ✅ Self-hosted compiler (`tcl_*.trit`)
- ✅ Standard library (`ulib.trit`)
- ✅ All kernel and app sources compile successfully
- ⚠️ Partial: generics / parametric polymorphism
- ⚠️ Partial: advanced type inference for complex nested expressions

See `ROADMAP_STATUS.json` phase `compiler-runtime` for evidence.
