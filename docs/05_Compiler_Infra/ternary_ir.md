# SSA Intermediate Representation

Source of truth: `ternary_compiler_ir.h`

---

## Overview

The compiler uses an **SSA (Static Single Assignment) IR** between the parser and code generation. The IR is structured as:

```
Module
 └── Function[]
      └── BasicBlock[]
           ├── Instr[]     (sequential instructions)
           └── Terminator  (one per block: return/jump/branch3/halt)
```

---

## Key Types

### `ValueId`

```cpp
using ValueId = int;
```

An integer ID for each SSA value (definition). `-1` = no value.

### `Instr` (Instruction)

```cpp
struct Instr {
    ValueId def;          // SSA destination (-1 if void)
    InstrOpcode opcode;   // What operation
    TypeRef type;         // Result type
    std::vector<ValueId> args; // Input value IDs
    long long imm;        // Immediate constant
    int aux;              // Auxiliary (width/CSR index/etc.)
    std::string symbol;   // Symbol name for calls/labels
    Effect effect;        // Side-effect class
    SourceSpan span;      // Source location (for diagnostics)
};
```

### `InstrOpcode` (IR opcodes, not ISA opcodes)

| Opcode | Description |
|--------|-------------|
| `Alloca` | Stack-allocate a local |
| `Const` | Load a constant value |
| `Copy` | Copy one SSA value to another |
| `Add/Sub/Mul/Div` | Arithmetic |
| `Cvt` | Type conversion |
| `Cmp` | Comparison → T1 result |
| `Phi` | SSA phi node (merge at join points) |
| `FieldAddr` | Address of struct field |
| `IndexAddr` | Address of array element |
| `AddrOf` | Take address of variable |
| `Deref` | Dereference pointer |
| `Load` | Load from memory |
| `Store` | Store to memory |
| `Syscall` | Issue syscall |
| `Fence` | Memory fence |
| `Tldr/Tstr` | Trit-level load/store |
| `Csrr/Csrw/Csrrw` | CSR access |
| `Call` | Direct function call |
| `CallR` | Indirect function call |
| `Ret` | Return from function |
| `Swap` | Swap two values |
| `Nop` | No operation |

### `Effect` (Side-effect classification)

| Effect | Meaning |
|--------|---------|
| `Pure` | No side effects; safe to reorder/eliminate |
| `ReadMem` | Reads memory |
| `WriteMem` | Writes memory |
| `Syscall` | Issues a syscall |
| `CSR` | Accesses a CSR |
| `Atomic` | Atomic operation (cannot reorder across fence) |
| `Control` | Control flow (branch, return) |

### `Terminator`

```cpp
struct Terminator {
    TerminatorKind kind;  // None/Return/Jump/Branch3/Halt
    ValueId condition;    // For Branch3: T1 value to branch on
    std::string target_neg;   // Branch target if condition == -1
    std::string target_zero;  // Branch target if condition == 0
    std::string target_pos;   // Branch target if condition == +1
    std::string target;       // Jump/Return target
};
```

`Branch3` maps directly to the ISA's three-way branch pattern (TCMP + BRN + BRP).

---

## Module Structure

```cpp
struct Module {
    std::string name;
    std::vector<Function> functions;
    std::vector<Diagnostic> diagnostics;
    std::map<std::string, std::string> metadata;
};

struct Function {
    std::string name;
    std::vector<std::pair<std::string, TypeRef>> params;
    TypeRef return_type;
    std::vector<BasicBlock> blocks;
    bool exported;
    bool unsafe_allowed;
    int ir_value_ceiling;   // Next SSA value ID to assign
};
```

---

## Optimizer Passes

The optimizer tracks its work via `OptimizerStats`:

| Pass | Stat field |
|------|-----------|
| mem2reg (alloca → SSA phi) | `mem2reg_promotions` |
| Constant folding | `constant_folds` |
| Copy propagation | `copy_props` |
| Strength reduction | `strength_reductions` |
| Common subexpression elimination | `cse_hits` |
| Dead instruction elimination | `dead_instrs` |
| Branch simplification | `branch_simplifications` |
| Swap optimization | `swaps` |

---

## Register Allocation

The allocator produces an `AllocationResult`:

```cpp
struct AllocationResult {
    bool success;
    std::map<ValueId, int> scalar_registers;  // SSA value → r0..r26
    std::map<ValueId, int> vector_registers;  // SSA value → v0..v7
    std::map<ValueId, int> spill_slots;       // SSA value → stack slot
    int spills;
    std::set<int> callee_saved_used;
    std::set<int> caller_saved_live_across_calls;
    int coalesced_moves;
    int interference_edges;
};
```

Spilled values are stored in DMEM via the stack pointer (r26).

---

## Compile Pipeline

```
Source (.trit)
  ↓ Lexer (ternary_compiler_lexer.h)
TokenStream
  ↓ Parser (ternary_compiler_parser.h)
ModuleAst
  ↓ Type inference (ternary_compiler_types.h)
Typed AST
  ↓ IR lowering (ternary_compiler_ir.h)
SSA Module
  ↓ Optimizer (ternary_compiler_codegen.h)
Optimized SSA Module
  ↓ Register allocator
AllocationResult
  ↓ Code generator (ternary_compiler_codegen.h)
ObjectModule (assembly text)
  ↓ Linker + Assembler (ternary_asm.h)
ExecutableImage (TritWord27[])
```

---

## Syscall IDs in IR

The `runtime` namespace in `ternary_compiler_ir.h` provides C++ constants for all syscall IDs, matching `SYSCALL_MANIFEST.json`:

```cpp
namespace sandbox::compiler::runtime {
    constexpr int sys_write_int = 1;
    constexpr int sys_open = 12;
    constexpr int sys_fork = 20;
    // ... (57 total)
}
```

When writing compiler-generated syscall sequences, use these constants — not raw integers.

---

## `CompileResult`

The full output of one compilation:

```cpp
struct CompileResult {
    bool success;
    std::vector<Diagnostic> diagnostics;
    std::shared_ptr<ModuleAst> typed_ast;
    Module ssa_module;
    Module optimized_module;
    AllocationResult allocation;
    LayoutTable layout_table;
    OptimizerStats optimizer_stats;
    ObjectModule object;
    std::string assembly;
};
```

`assembly` is the final textual `.tasm` assembly before linking.
