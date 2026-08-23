# Ternary Core Language Specification 1.0

**Status:** Normative — the authoritative definition of TCL 1.0.
**Implements:** TERNARY_ARCHITECTURE_CONTRACTS.md §4 (Calling Convention), §2 (Atomics), §1 (Memory Order).
**Target:** The TCL compiler (`tritc`) self-hosting rewrite, Phase 9 native kernel.

---

## Part I — Philosophy and Design Principles

### 1.1 The Design Contract

TCL is a systems language for the ternary machine. It makes three unconditional guarantees, enforced entirely at compile time with zero runtime overhead:

1. **Memory safety.** No null dereference, no use-after-free, no double-free, no buffer overflow.
2. **Concurrency safety.** No data race on shared memory. The type system rejects any cross-task access that lacks sufficient ordering proof.
3. **Exhaustive control flow.** Every three-way branch is proven complete at compile time. No implicit fallthrough.

These are not library features or linting hints. They are properties of the type system: a program that violates any of the three cannot be expressed in well-typed TCL.

### 1.2 What TCL Is Not

TCL does not have a garbage collector. It does not have a runtime thread scheduler. It does not have dynamic dispatch through vtables. It does not have exceptions. None of these omissions are accidents — each would introduce unpredictable latency on silicon where cycle budgets are fixed.

### 1.3 Relationship to the ISA

TCL is defined relative to the Ternary ISA and its architecture contracts. Where this specification refers to an instruction or register convention, the ISA document is authoritative. TCL's type system is designed so that every well-typed program lowers to a legal ISA instruction sequence without requiring runtime type tags, reference counts, or garbage collection roots.

---

## Part II — Lexical Structure

### 2.1 Source Encoding

TCL source files are sequences of 7-bit ASCII characters. Non-ASCII bytes are a lexical error except inside line comments.

### 2.2 Comments

```
// single-line comment, extends to end of line
```

Block comments are not supported in TCL 1.0. A future revision may add them.

### 2.3 Keywords

```
fn  let  var  return  match  while  if  else
trit  region  own  borrow  shared  ptr  null  unknown
type  struct  enum  where  unsafe  import  pub
as  in  true  false  break  continue
```

### 2.4 Identifiers

An identifier is `[A-Za-z_][A-Za-z0-9_]*`. Identifiers beginning with a capital letter are type names. Identifiers beginning with a lowercase letter or underscore are value names. This distinction is enforced by the parser and has semantic significance in pattern matching.

### 2.5 Integer Literals

```
42          decimal
0t1021      balanced ternary (prefix 0t, digits 0/1/2)
0x1F        hexadecimal (for bit patterns only; type must be a lane type)
```

Ternary literals `0t...` are digit sequences in {0, 1, 2} and represent unbalanced base-3 positional integers. The compiler converts them to the balanced internal representation.

### 2.6 Trit Literals

The single-trit literals are spelled `(-1)`, `0`, and `(+1)`. They have type `T1`.

---

## Part III — The Type System

### 3.1 Overview

TCL uses **Hindley-Milner type inference** extended with:

- Ternary width parameters (§3.3)
- Three-valued pointer states (§3.5)
- Region annotations (§3.6)
- Memory-order annotations (§3.7)

Every expression has a unique principal type derivable by the compiler without any programmer annotation. Annotations are permitted and are checked for consistency, but they are never required except at function boundaries for exported APIs.

### 3.2 Primitive Numeric Types

TCL's numeric types are the native ternary floating-point widths from the ISA:

| Type | Trits | Description |
|------|-------|-------------|
| `T1` | 1 | Native ternary trit: values {−1, 0, +1} |
| `T5` | 5 | Small integer: range ±121 |
| `T10` | 10 | Short float: 6 mantissa + 4 exponent trits |
| `T20` | 20 | Medium float: 14 mantissa + 6 exponent trits |
| `T40` | 40 | Native word: 33 mantissa + 7 exponent trits |
| `T50` | 50 | Extended: 41 mantissa + 9 exponent trits |

T40 is the language-level native word because the architecture's explicit
design goal is a **ternary equivalent of a 64-bit computer**. Its 40 raw trits
provide `3^40` states, while a 41-trit word would exceed `2^64`; equivalence
means scalar capacity and software role, not binary-compatible representation
or the physical size of every host/wire encoding.
The 27-trit instruction word, 27 vector lanes, and T50 extended values are
separate dimensions and do not redefine the native scalar width.

Lane types (SIMD transport view of the same widths) are `L1`, `L5`, `L10`, `L20`, `L40`, `L50`.

`T1` is the native ternary boolean replacement. It is not a boolean — it carries three distinct logical values: negative (−1), zero (0), and positive (+1). The compiler enforces exhaustive matching on `T1` the same way it enforces exhaustive matching on any enum.

### 3.3 Width-Parametric Types

TCL types can be parametrized over a trit width:

```
fn dot_product<W: TritWidth>(a: [T<W>], b: [T<W>]) -> T<W>
```

`TritWidth` is a compiler-known kind whose inhabitants are `{1, 5, 10, 20, 40, 50}`. The compiler monomorphizes at each call site. The width parameter `W` participates in type inference — if the argument type can be determined from context, `W` is inferred and the call requires no annotation.

Width constraints can be combined:

```
fn narrow_cast<W: TritWidth, V: TritWidth where W <= V>(x: T<V>) -> T<W>
```

The `where` clause is checked at monomorphization time and produces a compile error if violated.

### 3.4 `T1` as the Exhaustive Condition Type

`T1` replaces the boolean in all conditional contexts. The `if` statement does not exist in TCL. The fundamental control structure is `match`:

```
match expr {
    neg  => { /* expr < 0 */ }
    zero => { /* expr == 0 */ }
    pos  => { /* expr > 0 */ }
}
```

Every `match` on a `T1` or numeric expression must cover all three arms. The compiler rejects missing arms with a type error, not a warning. This is the compile-time analog of the ISA's three-way branch instructions (`brn`, `brz`, `brp`) — the language and the hardware share the same structure.

Pattern matching on structured types:

```
match some_ptr {
    null     => { /* ptr == null, T1 trit = -1 */ }
    unknown  => { /* ptr not yet initialized, trit = 0 */ }
    valid(p) => { /* p is proven non-null, use p here */ }
}
```

The `valid(p)` arm introduces `p` as a binding with a `ptr<T, valid>` type. The binding does not exist in the other arms. This is how null safety is enforced — the value is only accessible in the arm where its validity is proven.

### 3.5 Three-Valued Pointer Types

```
ptr<T, S>
```

Where `S` is one of three pointer states, corresponding directly to the three trit values:

| State | Trit | Meaning |
|-------|------|---------|
| `null` | −1 | Explicitly absent. Dereference is a compile error. |
| `unknown` | 0 | Uninitialized or indeterminate. Dereference is a compile error. |
| `valid` | +1 | Proven present. Dereference is permitted. |

The state `S` is part of the type. `ptr<T, valid>` and `ptr<T, null>` are distinct types with no implicit conversion between them.

The only way to go from `ptr<T, unknown>` to `ptr<T, valid>` is through a `match` that checks the pointer and binds it in the `pos` arm. This check compiles to a single `TCMP` instruction — it is the same instruction that does the branch. There is no runtime overhead beyond what the branch already costs.

Pointer state narrows through match and widens through assignment:

```
let p: ptr<T40, unknown> = some_address();    // unknown state
match p {
    null    => { /* cannot use p here */ }
    unknown => { /* cannot use p here */ }
    valid(q) => {
        let x: T40 = *q;    // q is ptr<T40, valid>: dereference permitted
    }
}
// p is still ptr<T40, unknown> here — match does not widen
```

### 3.6 Ownership and Region-Based Memory Safety

TCL uses **region-based ownership** — a simpler model than Rust's lifetime system, sufficient for a systems language targeting a single-address-space microkernel.

#### 3.6.1 Owned Pointers

`own<T>` is an owned heap allocation. Ownership follows move semantics:

```
let a: own<T40> = alloc(0);    // a owns the allocation
let b = a;                     // a is moved into b; a is invalid
// drop(a) here would be a compile error: a was moved
// b is dropped at end of its lexical scope: free is inserted automatically
```

At the close of every lexical scope, the compiler inserts `free` calls for every `own<T>` that has not been moved. This is **compile-time deallocation** — no reference counting, no GC, no runtime overhead.

#### 3.6.2 Borrowed References

`borrow<T>` is a non-owning reference. The borrow must not outlive the owner:

```
fn compute(x: borrow<T40>) -> T40 { return *x; }

let val: own<T40> = alloc(42);
let result = compute(borrow(val));    // borrow is released at end of call
// val is still owned here
```

Rules:

1. There may be any number of simultaneous `borrow<T>` references to a value.
2. There may be at most one `borrow_mut<T>` reference, and no simultaneous `borrow<T>`.
3. The owner may not be dropped or moved while any borrow is live.

These rules are enforced by scope analysis in the compiler. They do not require runtime checks.

#### 3.6.3 Stack Allocation

Stack values are neither `own<T>` nor `borrow<T>` — they are plain `T`. The compiler allocates them to registers first (via graph-coloring) and to the stack frame only if register pressure demands it. No annotation is required.

#### 3.6.4 Region Annotations

For cases where values from multiple allocation sources interact, an optional region annotation is available:

```
fn copy_words<'r, 's>(dst: borrow_mut<T40, 'r>, src: borrow<T40, 's>, n: T40) -> void
    where 'r != 's
```

The `where 'r != 's` clause prevents the compiler from accepting aliased arguments. This is used in `memmove`-style functions where the no-alias property matters for correctness.

Region annotations are inferred from call sites in most cases. They are required only when the inference would be ambiguous.

### 3.7 Shared Types and Memory-Order Safety

```
shared<T, ORDER>
```

`shared<T, ORDER>` is a type for values accessed from multiple concurrent tasks. `ORDER` is one of three compile-time constants mapping directly to the ISA's memory-ordering trit:

| Constant | Trit | ISA Fence | Guarantees |
|----------|------|-----------|------------|
| `RELAXED` | −1 | `fence.-1` | Atomic but may reorder |
| `ACQ_REL` | 0 | `fence.0` | Acquire-release pairing |
| `SEQ_CST` | +1 | `fence.+1` | Total order |

The type system enforces that every access to a `shared<T, ORDER>` value produces a fence of at least `ORDER` strength. Accessing a `shared<T, ACQ_REL>` with `RELAXED` semantics is a type error.

```
let counter: shared<T40, ACQ_REL> = shared_alloc(0);

// This compiles: ACQ_REL >= ACQ_REL
let v: T40 = atomic_load(counter, ACQ_REL);

// This is a type error: RELAXED < ACQ_REL
let v: T40 = atomic_load(counter, RELAXED);    // ERROR
```

`shared<T, ORDER>` types are not `Send` by default — only types where `T` contains no interior `own<U>` pointers may be shared. This prevents accidentally sharing heap-owned data across task boundaries without transferring ownership.

The compiler lowers `shared<T, ORDER>` accesses to the appropriate `TLDR`/`TSTR`/`FENCE` instruction sequences. There is no runtime lock — the ordering is enforced by the hardware memory model, which is what the type annotation encodes.

### 3.8 The `trit` Function and Width Coercion

Numeric types are not implicitly coerced. A `T10` does not automatically widen to `T40`. The function `widen<W>(x: T<V>) -> T<W>` performs explicit widening. The function `narrow<W>(x: T<V>) -> T<W>` performs explicit narrowing (possible loss of precision, no runtime check, compile-time warning if `W < V`).

Type inference resolves the width `W` from context whenever possible:

```
let x: T40 = some_t10_value;    // ERROR: T10 not assignable to T40 without widen
let x: T40 = widen(some_t10_value);    // OK: W inferred as 40 from annotation
let x = widen(some_t10_value);         // OK: W inferred from use-site if unambiguous
```

### 3.9 Struct Types

```
struct Point {
    x: T40,
    y: T40,
}
```

Structs are value types by default: assignment copies. To heap-allocate a struct, wrap it in `own<Point>`. Field access on a pointer to a struct requires the pointer to be `valid`:

```
let p: own<Point> = alloc(Point { x: 1, y: 2 });
let q: borrow<Point> = borrow(p);
let x_val: T40 = q.x;    // borrow allows field read
```

Structs may be generic over width parameters:

```
struct Pair<W: TritWidth> {
    first:  T<W>,
    second: T<W>,
}
```

### 3.10 Enum Types

```
enum Result<T> {
    Ok(T),
    Err(T40),
}
```

Pattern matching on enums is exhaustive — every variant must be covered. Enums carry their discriminant as a trit or trit sequence in the ISA's balanced ternary encoding. There are no heap-allocated enum tags; the tag is packed into the value word where possible.

---

## Part IV — Expressions

### 4.1 Arithmetic

Binary operators `+`, `-`, `*`, `/`, `%` on numeric types. Operands must have the same type — no implicit widening. Division by zero traps via the ISA's `TRAP_DIV_ZERO` path.

Unary `-` negates. Unary `!` (logical trit inversion) maps {−1→+1, 0→0, +1→−1}.

### 4.2 Comparison

`<`, `<=`, `>`, `>=`, `==`, `!=` return `T1`. They lower to `TCMP` followed by appropriate masking. The result is exactly the signed comparison trit: −1 for less-than, 0 for equal, +1 for greater-than.

### 4.3 Carryless Lane Operations

On lane types `L<W>`:

| Operator | Meaning |
|----------|---------|
| `a :+: b` | Carryless per-trit addition (modulo 3, no carry propagation) |
| `a :-: b` | Carryless per-trit subtraction |
| `a :&: b` | Per-trit lattice minimum (ternary AND) |
| `a :|: b` | Per-trit lattice maximum (ternary OR) |
| `!a`      | Per-trit negation |

These lower directly to `TLADD`, `TLSUB`, `TLAND`, `TLOR`, `TLNEG` ISA instructions.

### 4.4 Dereference and Address-Of

`*p` dereferences a `ptr<T, valid>`. Dereferencing `ptr<T, null>` or `ptr<T, unknown>` is a compile error — not a runtime trap. Address-of `&x` produces `ptr<T, valid>` when `x` is a local variable or a `borrow_mut`-accessible location.

### 4.5 Tuple Destructuring and Swap

```
(a, b) = (b, a);
```

Swapping two local variables compiles to a single `SWAP rA, rB` instruction when both are register-resident. When either is stack-resident, the compiler emits the carryless tritwise swap sequence:

```asm
tladd.l40 r1, r1, r2    ; a = a :+: b
tlsub.l40 r2, r1, r2    ; b = a :-: b  = original a
tlsub.l40 r1, r1, r2    ; a = a :-: b  = original b
```

This sequence is overflow-immune by construction (no carry is generated) and requires no temporary register.

### 4.6 Function Calls

Function call syntax is `f(arg1, arg2, ...)`. Arguments are passed in registers `r13`–`r18` per the ABI. The return value is in `r13`. This is not visible to TCL programmers — it is an implementation detail of the calling convention.

### 4.7 `alloc` and `free`

`alloc(value: T) -> own<T>` places `value` on the heap and returns an owned pointer. `free` is not called by the programmer — it is inserted by the compiler at the end of every scope where an `own<T>` goes out of use.

Explicit `free(p: own<T>)` is available in `unsafe` blocks for interoperability with manually managed memory regions.

### 4.8 `unsafe` Blocks

```
unsafe {
    let raw: T40 = load(addr);
    store(addr, value);
}
```

Inside an `unsafe` block, the programmer may:

- Call `load(addr: T40) -> T40` (plain word load, no reservation)
- Call `store(addr: T40, val: T40)` (plain word store)
- Call `tldr(addr: T40, order: ORDER) -> T40` (load-reserved)
- Call `tstr(addr: T40, desired: T40, expected: T40, order: ORDER) -> T1` (store-conditional, returns {−1=collision, 0=mismatch, +1=success})
- Call `fence(order: ORDER)` (memory barrier)
- Call `csr_read(name) -> T40` and `csr_write(name, value: T40)` (CSR access)
- Dereference raw `T40` values as addresses without pointer-state checking

The `unsafe` keyword is a contract that the programmer is reasoning about memory manually. The compiler still type-checks the code inside — `unsafe` removes pointer-state enforcement, not the type system.

---

## Part V — Statements

### 5.1 `let` and `var`

```
let x = expr;          // immutable binding, type inferred
let x: T40 = expr;    // immutable binding, type annotated
var y = expr;          // mutable binding
var y: T40 = expr;    // mutable binding, annotated
```

`let` bindings may not be assigned after their initial value. `var` bindings may be reassigned with `=`. Both are scoped to their enclosing `{}` block.

### 5.2 `match`

The fundamental control structure. Three-arm match on a numeric expression:

```
match expr {
    neg  => { ... }
    zero => { ... }
    pos  => { ... }
}
```

The arms are not ordered — all three must be present. The compiler lowers this to `brn`/`brz`/`brp` with a single `TCMP` (if the expression is not already a comparison result). There is no double-compare.

Structural match on an enum:

```
match result {
    Ok(v)  => { use v here }
    Err(e) => { use e here }
}
```

Structural match on a pointer:

```
match p {
    null     => { ... }
    unknown  => { ... }
    valid(q) => { use q: ptr<T, valid> here }
}
```

### 5.3 `while`

```
while cond_expr { body }
```

`cond_expr` must have type `T1`. The loop continues while `cond_expr == +1` (positive). `while` is the only loop form in TCL 1.0.

`break` exits the innermost `while`. `continue` advances to the next iteration.

### 5.4 `return`

`return expr` exits the current function with `expr` as the return value. `return` without an argument is valid only in `fn ... -> void` functions.

---

## Part VI — Functions

### 6.1 Syntax

```
fn name(param1: Type1, param2: Type2) -> ReturnType {
    body
}
```

Function parameter types and return types are always annotated. This is the only place in TCL where annotations are required — at abstraction boundaries, the principal type cannot be inferred across compilation units without explicit signatures.

### 6.2 Generic Functions

```
fn max<W: TritWidth>(a: T<W>, b: T<W>) -> T<W> {
    match a - b {
        neg  => { return b; }
        zero => { return a; }
        pos  => { return a; }
    }
}
```

Width parameters are listed in `<...>` after the function name. The compiler monomorphizes — one copy of the function is emitted per distinct width used at call sites in the program.

### 6.3 Recursion

Recursive functions are supported. The compiler does not transform tail calls automatically in TCL 1.0; this is a planned extension.

### 6.4 Visibility

```
pub fn exported_function(...) -> ... { ... }
fn internal_function(...) -> ... { ... }
```

`pub` marks a function as part of the module's exported API. Non-pub functions are internal. Linking respects this distinction.

---

## Part VII — Modules and Imports

### 7.1 Module Structure

Each `.trit` source file is a module. The module name is the filename without extension. There is no subdirectory hierarchy in TCL 1.0.

### 7.2 Imports

```
import ulib;
import kernel_abi;
```

An import makes the exported names of the named module available without qualification. Name conflicts between modules are a compile error. Selective imports are not supported in TCL 1.0.

### 7.3 Linking

The linker takes a set of object modules (compiled `.trit` files) and resolves cross-module references. The entry point is `pub fn main() -> T40` in the root module. The executable header is generated from the linker's knowledge of text page count, data page count, and stack hint.

---

## Part VIII — The TCL 1.0 Type Inference Algorithm

### 8.1 Algorithm W

The type inference algorithm is Algorithm W over the Hindley-Milner system, extended for:

1. Width-parametric types (width variables unify like type variables)
2. Pointer-state types (state is a type-level constant, not inferred — it is determined by control flow)
3. Region annotations (fresh region variables per `borrow` introduction, unified at call sites)
4. Memory-order annotations (order is a type-level constant, not inferred — it must be spelled at `shared<T, ORDER>` declaration)

Width inference: width variables are introduced fresh for each polymorphic call and unified via `min` (widening always succeeds; narrowing produces a warning). The principal type of a width-polymorphic expression is the minimal width consistent with all constraints.

Pointer-state inference: pointer states are *not* inferred via unification. They are computed by a forward dataflow analysis over the control flow graph, conservatively narrowing at merge points (two paths that join where one has `valid` and the other has `unknown` produce `unknown` at the join).

### 8.2 Generalization

A `let` binding is generalized (becomes a polymorphic scheme) when:

1. The bound expression is a value (not a function call with effects)
2. No free type variables in the binding's type appear in the environment

This is the value restriction, which prevents unsoundness from mutable bindings being generalized.

### 8.3 Type Error Messages

The compiler reports type errors by printing the two types that failed to unify and the expression that caused the failure. Width errors additionally report the width constraint chain. Pointer-state errors report the control flow path that produced each state, so the programmer can identify which code path left a pointer in `unknown` state.

---

## Part IX — Lowering to TCL IR and TASM

### 9.1 The TCL IR

The compiler translates TCL source to a typed SSA IR before emitting TASM. The IR has:

- Basic blocks with typed phi nodes
- Explicit ownership transfers as `move` IR instructions
- Borrow introductions as `borrow` IR instructions with scope annotations
- Width conversions as explicit `cvt` IR instructions
- Three-way branches as `branch3 cond, neg_target, zero_target, pos_target` IR instructions

The IR is the primary optimization artifact. Passes that operate on the IR include: constant propagation, dead code elimination, common subexpression elimination, and mem2reg (which promotes stack allocations to register-resident SSA values where possible).

### 9.2 Register Allocation

Register allocation uses graph coloring over the SSA interference graph. The register file has 24 allocatable registers (r1–r24; r0 is zero, r25 is the link register, r26 is the stack pointer). Callee-saved registers (r1–r12) are preferred for values that live across calls. Caller-saved temporaries (r13–r24) are preferred for short-lived values.

Spills insert `STORE` instructions to the stack frame before their last use and `LOAD` instructions before their first use after the spill point. The stack frame is allocated statically at function entry.

### 9.3 Ownership Lowering

`own<T>` allocations lower to `sys_sbrk` calls at their introduction and `sys_free` calls (or equivalent) at their scope exit. The IR records the ownership transfer at every `move` operation so the compiler can insert exactly one `free` per allocation.

### 9.4 Pointer-State Lowering

`ptr<T, valid>` dereferences lower to plain `LOAD` instructions — no null check is emitted because validity is already proven at the type level. `match p { null => ... valid(q) => ... }` on a `ptr<T, unknown>` lowers to a `TCMP` of the address against zero, followed by `brn`/`brz`/`brp`.

### 9.5 `shared<T, ORDER>` Lowering

Reads lower to `TLDR.ORDER` followed by the read value. Writes lower to a `TLDR`/`TSTR` compare-and-swap loop with the appropriate order fence. `fence(ORDER)` lowers to `FENCE.ORDER`.

### 9.6 Three-Way Branch Lowering

```
match expr {
    neg  => { B_neg  }
    zero => { B_zero }
    pos  => { B_pos  }
}
```

Lowers to:

```asm
tcmp.t40 r_cmp, r_expr, r0    ; r_cmp = sign(expr)
brn r_cmp, L_neg
brz r_cmp, L_zero
; fall through to L_pos
L_pos: ... B_pos ...  jmp L_end
L_neg: ... B_neg ...  jmp L_end
L_zero: ... B_zero ... jmp L_end
L_end:
```

The hot arm (statistically most frequent based on PGO data, or `pos` by default) is placed at the fall-through path, minimizing branch misprediction cost.

---

## Part X — Formal Grammar (EBNF)

```ebnf
program         = { import_decl } { item } ;

import_decl     = "import" IDENT ";" ;

item            = fn_decl | struct_decl | enum_decl | type_alias ;

fn_decl         = [ "pub" ] "fn" IDENT [ type_params ] "(" [ param_list ] ")"
                  "->" type_expr [ where_clause ] block ;

type_params     = "<" type_param { "," type_param } ">" ;
type_param      = IDENT ":" kind ;
kind            = "TritWidth" ;
where_clause    = "where" constraint { "," constraint } ;
constraint      = IDENT "<=" IDENT | IDENT "!=" IDENT ;

param_list      = param { "," param } ;
param           = IDENT ":" type_expr ;

struct_decl     = "struct" TYPE_IDENT [ type_params ] "{" field_list "}" ;
field_list      = field { "," field } [ "," ] ;
field           = IDENT ":" type_expr ;

enum_decl       = "enum" TYPE_IDENT [ type_params ] "{" variant_list "}" ;
variant_list    = variant { "," variant } [ "," ] ;
variant         = TYPE_IDENT [ "(" type_expr ")" ] ;

type_alias      = "type" TYPE_IDENT [ type_params ] "=" type_expr ";" ;

type_expr       = "T1" | "T5" | "T10" | "T20" | "T40" | "T50"
                | "L1" | "L5" | "L10" | "L20" | "L40" | "L50"
                | "void"
                | "trit"
                | "ptr" "<" type_expr "," ptr_state ">"
                | "own" "<" type_expr ">"
                | "borrow" "<" type_expr [ "," lifetime ] ">"
                | "borrow_mut" "<" type_expr [ "," lifetime ] ">"
                | "shared" "<" type_expr "," order_expr ">"
                | "T" "<" IDENT ">"
                | "[" type_expr ";" expr "]"
                | TYPE_IDENT [ "<" type_expr { "," type_expr } ">" ]
                ;

ptr_state       = "null" | "unknown" | "valid" ;
order_expr      = "RELAXED" | "ACQ_REL" | "SEQ_CST" ;
lifetime        = "'" IDENT ;

block           = "{" { stmt } [ expr ] "}" ;

stmt            = let_stmt | var_stmt | assign_stmt | while_stmt
                | return_stmt | expr_stmt | unsafe_block
                | tuple_swap_stmt ;

let_stmt        = "let" IDENT [ ":" type_expr ] "=" expr ";" ;
var_stmt        = "var" IDENT [ ":" type_expr ] "=" expr ";" ;
assign_stmt     = expr "=" expr ";" ;
tuple_swap_stmt = "(" IDENT "," IDENT ")" "=" "(" IDENT "," IDENT ")" ";" ;
return_stmt     = "return" [ expr ] ";" ;
expr_stmt       = expr ";" ;

while_stmt      = "while" expr block ;
unsafe_block    = "unsafe" block ;

expr            = match_expr | binary_expr ;

match_expr      = "match" expr "{" match_arm match_arm match_arm "}" ;
match_arm       = arm_pattern "=>" block ;
arm_pattern     = "neg" | "zero" | "pos"
                | "null" | "unknown" | "valid" "(" IDENT ")"
                | TYPE_IDENT [ "(" IDENT ")" ]
                ;

binary_expr     = unary_expr [ bin_op binary_expr ] ;
bin_op          = "+" | "-" | "*" | "/" | "%" | "<" | "<=" | ">" | ">="
                | "==" | "!=" | ":+:" | ":-:" | ":&:" | ":|:" ;

unary_expr      = [ "-" | "!" | "&" | "*" ] postfix_expr ;

postfix_expr    = primary_expr { postfix_suffix } ;
postfix_suffix  = "." IDENT                   (* field access *)
                | "[" expr "]"                (* index *)
                | "(" [ arg_list ] ")"        (* call *)
                ;

primary_expr    = IDENT
                | INTEGER_LIT
                | TRIT_LIT
                | "(" expr ")"
                | struct_literal
                ;

struct_literal  = TYPE_IDENT "{" field_init { "," field_init } [ "," ] "}" ;
field_init      = IDENT ":" expr ;

arg_list        = expr { "," expr } ;
```

---

## Part XI — Invariants and Proofs

### 11.1 Memory Safety Theorem

For every well-typed TCL program P:

1. Every `load` or dereference of a pointer in P has type `ptr<T, valid>` at its use site.
2. Every `own<T>` value is freed exactly once, at the end of its scope or at its move destination's scope.
3. No `borrow<T>` reference outlives the `own<T>` or stack value it refers to.

**Consequence:** P cannot produce a null dereference, a use-after-free, a double-free, or a buffer overread.

### 11.2 Concurrency Safety Theorem

For every well-typed TCL program P:

Every access to a `shared<T, ORDER>` value in P is paired with a memory barrier of at least `ORDER` strength, as defined by the ISA fence ordering trit.

**Consequence:** P is data-race-free on any data-race-free-programs-are-sequentially-consistent (DRF-SC) implementation of the ternary memory model.

### 11.3 Exhaustive Branching Theorem

For every well-typed TCL program P:

Every `match` expression in P covers all reachable variants of its subject type.

**Consequence:** Control flow in P cannot fall through a conditional silently. Every three-way branch is complete.

---

## Part XII — Future Extensions (Not in Spec 1.0)

These are explicitly deferred and not part of the 1.0 contract. They are listed here so implementers can leave architectural space for them.

**Closures.** Higher-order functions with captured environments. Deferred because of interaction with the ownership system — captured `own<T>` values require move semantics in the closure, which requires the closure type to carry ownership metadata.

**Trait objects and dynamic dispatch.** A mechanism for runtime polymorphism without vtables (using trampoline dispatch through a CSR-resident dispatch table). Deferred because the microkernel does not require it.

**Tail call optimization.** Automatic transformation of tail-recursive functions into loops. Deferred because the interaction with the region system requires careful treatment of borrowed references at the tail position.

**Inline assembly.** A `tasm! { ... }` block that embeds literal TASM into the instruction stream with typed register bindings. Deferred because it requires a formal register binding syntax that is currently underspecified.

**Compile-time evaluation (`comptime`).** Evaluation of expressions at compile time, producing constants or types. The width-parametric system already covers the main use case (specialization over width), so this is lower priority.

**Associated types on structs.** Type members of struct definitions that vary with generic parameters. Deferred pending the trait system.

---

## Appendix A — Mapping to `ulib.trit` v1 Signatures

The following table shows how existing `ulib.trit` functions map to their TCL 1.0 typed signatures. This drives the rewrite of `ulib.trit` under the new compiler.

| Function | TCL 1.0 signature |
|---|---|
| `malloc(words)` | `fn malloc(words: T40) -> own<[T40]>` |
| `free(ptr)` | `fn free(p: own<[T40]>) -> void` (compiler-inserted; not called manually) |
| `memcpy(dest, src, n)` | `fn memcpy<'r,'s>(dst: borrow_mut<T40,'r>, src: borrow<T40,'s>, n: T40) -> void where 'r != 's` |
| `strcmp(a, b)` | `fn strcmp(a: borrow<T40>, b: borrow<T40>) -> T1` |
| `strlen(s)` | `fn strlen(s: borrow<T40>) -> T40` |
| `read(fd, buf, n)` | `fn read(fd: T40, buf: borrow_mut<T40>, n: T40) -> T40` |
| `write(fd, buf, n)` | `fn write(fd: T40, buf: borrow<T40>, n: T40) -> T40` |
| `ring_write(rb, val)` | `fn ring_write(rb: borrow_mut<RingBuf>, val: T40) -> T1` |
| `vec_push(vec, val)` | `fn vec_push<W: TritWidth>(vec: borrow_mut<Vec<W>>, val: T<W>) -> T1` |
| `heap_push(h, val)` | `fn heap_push<W: TritWidth>(h: borrow_mut<MinHeap<W>>, val: T<W>) -> T1` |
| `setenv(name, val)` | `fn setenv(name: borrow<T40>, val: borrow<T40>) -> T1` |
| `getenv(name)` | `fn getenv(name: borrow<T40>) -> ptr<T40, unknown>` |

`getenv` returns `ptr<T40, unknown>` because the key may or may not be present. The caller must match on the pointer state before using the value — this is the compile-time enforcement of the pattern that `ulib.trit` currently handles with manual null checks.

---

## Appendix B — TCL 1.0 vs Current `ternary_compiler.h`

This table tracks which features of the compiler prototype are superseded, retained, or extended by this specification.

| Feature | `ternary_compiler.h` | TCL 1.0 |
|---|---|---|
| Type inference | HM with type variables | HM + width variables + pointer-state dataflow |
| Boolean | None; T40 used as condition | `T1` is the native trit type |
| Null safety | `UserPtrState` enum, manual match | `ptr<T, S>` is part of the type; match is forced |
| Memory safety | Manual `free` in source | `own<T>` auto-dropped at scope exit |
| Shared memory | `SharedWord` struct, manual | `shared<T, ORDER>` type with order enforcement |
| Branching | `match sign(...)` with three arms | `match expr { neg => ... zero => ... pos => ... }` |
| Width polymorphism | Not implemented | `fn f<W: TritWidth>(...) -> T<W>` |
| Register allocation | Unconnected from codegen | Directly drives instruction selection |
| Unsafe | `unsafe { load(...) store(...) }` | Same; also adds `tldr`, `tstr`, `fence`, CSR ops |
| Control flow | `while pos(...)` | `while expr` where expr: T1 |
