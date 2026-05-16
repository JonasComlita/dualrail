# Ternary IR (Intermediate Representation)

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_ir.h` |

---

## 🏗️ Design Philosophy
The Ternary IR is a **Linear Builder IR**. Unlike LLVM, which uses a complex graph, the Trit-Stack IR uses a stream of instructions that map 1:1 to assembly but provide **automatic register allocation** and **static type checking**.

---

## 🧬 Type System
The IR enforces the hardware's tiered width model through the `ir::Type` enum.

| Category | Types | Hardware Field |
| :--- | :--- | :--- |
| **Numeric** | `T1`, `T5`, `T10`, `T20`, `T40`, `T50` | `FUNC_TX` |
| **Lane** | `L1`, `L5`, `L10`, `L20`, `L40`, `L50` | `FUNC_LX` |

---

## 📦 The `Value` Container
Every operation in the IR returns an `ir::Value` object.

```cpp
struct Value {
    Type type;    // The trit-width/mode
    int  reg;     // The physical register (0-26 or v0-v7)
    bool vector;  // True if stored in the Vector Register File

    bool valid() const { return reg >= 0; }
};
```

*   **The Zero Rule**: `program.zero(type)` returns a special `Value` mapped to `r0`. This allows the IR to use the hardware's zero-wire for efficient clearing and comparison logic without allocating a temporary register.

---

## 🛠️ The `Program` Builder API
The `ir::Program` class maintains the emission state and the free-register pools.

### 1. Register Management
*   **Allocation**: `allocScalar()` and `allocVector()` automatically pick the next available register from the free pool.
*   **Release**: `release(value)` returns a register to the pool for reuse. This is the IR's primary mechanism for minimizing register pressure.

### 2. Instruction Emission
Methods like `prog.add(a, b)` perform three steps:
1.  Verify `a.type == b.type`.
2.  Allocate a destination `out = allocScalar(a.type)`.
3.  Emit the assembly line: `add.t40 out, a, b`.

### 3. Branching & Labels
Labels are emitted as unique strings. The IR builder does not calculate offsets; it relies on the [Two-Pass Assembler](../04_Binary_Contract/asm_syntax.md) to resolve label addresses.

---

## 🧪 Lowering to Binary
The `program.lower()` method is the final bridge.
1.  Converts the internal instruction stream into a single C++ `std::string`.
2.  Calls `vm::assembler::assemble()` on the text.
3.  Returns an `AssemblyResult` containing the bootable binary image.

> [!TIP]
> **Diagnostic Logging**: The IR builder captures semantic errors (e.g., "type mismatch in ADD") before the assembler is ever called, providing much clearer compiler diagnostics than raw assembly.
