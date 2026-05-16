# Symbol Table & Scope Management

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| 🛠️ **Draft** | 2026-05-15 | `ternary_ir.h` |

---

## 🎯 The Variable Registry
The Symbol Table is the central database used by the compiler frontend to resolve names into [Ternary IR](ternary_ir.md) values.

### 1. Symbol Entry
Each entry in the table represents a declared variable or function parameter.

```cpp
struct Symbol {
    std::string name;
    ir::Type    type;   // T1, T5, T40, etc.
    ir::Value   value;  // The IR-allocated register
    int         depth;  // Nesting level
};
```

---

## 🏗️ Scope Hierarchy
The Trit-Lang compiler uses a **Nested Map** structure to handle variable visibility.

### 1. Global Scope
Contains system-wide constants, global variables in `.data`, and function entry points. These symbols are never released.

### 2. Local (Function) Scope
Contains parameters and local variables. When a function finishes lowering, all symbols in this scope are popped, and their `ir::Value` registers are returned to the `ir::Program` free-pool.

### 3. Block Scope (Nested)
Variables declared inside an `if` or `while` block.
*   **Shadowing**: A variable in a deeper scope can "shadow" a variable of the same name in a parent scope.
*   **Cleanup**: When the block ends, the registers for these variables are released immediately.

---

## 🛡️ Resolution Logic
1.  **Declaration**: The compiler checks the *current* scope level. If the name exists, it throws a "Redeclaration Error." If not, it calls `program.allocScalar(type)` and stores the result.
2.  **Lookup**: The compiler searches from the *deepest* scope up to the global scope. The first match found is used.
3.  **Type Safety**: The Symbol Table enforces that the `ir::Type` of the retrieved symbol matches the expected type of the expression.

> [!TIP]
> **Parameter Passing**: Function parameters are inserted into the Symbol Table's first local scope level using the `a0-a3` registers defined in the [ABI Spec](../04_Binary_Contract/abi_spec.md).
