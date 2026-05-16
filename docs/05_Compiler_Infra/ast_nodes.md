# AST Node Specification

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| 🛠️ **Draft** | 2026-05-15 | `ternary_ir.h` |

---

## 🌳 The Compiler Frontend
The AST represents the high-level "Trit-Lang" source code before it is lowered into the [Ternary IR](ternary_ir.md). Every node in the AST is responsible for calling the appropriate `ir::Program` method during the lowering phase.

---

## 💎 Expression Nodes (Returns `ir::Value`)
Expressions represent computations that yield a ternary value of a specific `ir::Type`.

| Node Type | Children | IR Mapping |
| :--- | :--- | :--- |
| **`LiteralNode`** | Value (Long) | `program.constant(type, value)` |
| **`BinaryOpNode`** | LHS, RHS, Op | `program.add()`, `program.sub()`, etc. |
| **`UnaryOpNode`** | Expr, Op | `program.neg()`, `program.abs()`, etc. |
| **`VariableNode`** | Name | Fetches the `ir::Value` from the Symbol Table. |
| **`TypeNode`** | Width, Mode | Resolves to an `ir::Type` (e.g., `T5`, `L40`). |
| **`FunctionCallNode`**| Name, Args | `program.callr()` or `program.jmp()` |

---

## 🏗️ Semantic Context
*   **The Symbol Table**: During the lowering of an `AssignmentNode`, the compiler assigns an `ir::Value` (register) to a name. Subsequent `VariableNodes` look up this name to retrieve the same `ir::Value`.
*   **Type Propagation**: Every expression node carries a `type` attribute. If a `BinaryOpNode` detects a type mismatch between its children, it must emit a diagnostic error before calling the IR builder.

---

## 📜 Statement Nodes
Statements define the control flow and state mutations of the program.

| Node Type | Children | IR Mapping |
| :--- | :--- | :--- |
| **`AssignmentNode`** | Var, Expr | `program.copy()` or register mapping. |
| **`IfNode`** | Cond, Then, Else | `program.label()`, `program.brz()`. |
| **`WhileNode`** | Cond, Body | `program.label()`, `program.brn()`. |
| **`ReturnNode`** | Expr | `program.copy(r1)`, `program.ret()`. |
| **`MemoryNode`** | Base, Offset | `program.load()`, `program.store()`. |

---

## 🚀 AI & Vector Nodes (Native Ternary)
These nodes provide direct access to the BitNet hardware primitives.

### 1. `VectorNode`
*   **Operations**: `VADD`, `VMUL`, `VDOT`.
*   **Lowering**: Maps to `program.vadd()`, `program.vdotT1()`.
*   **Type Safety**: Ensures operands are of `ir::Type::L1` to `L50`.

### 2. `AccumulatorNode`
*   **Operations**: `ACLR`, `AADD`, `ASTORE`.
*   **Lowering**: Maps to `program.aclr()`, `program.aadd()`, `program.astore()`.
*   **Precision**: Forces the use of 50-trit high-precision accumulation.

---

## 🧬 Lowering Protocol
The `ASTNode` interface requires a `lower()` method:

```cpp
class ASTNode {
    virtual ir::Value lower(ir::Program& prog) = 0;
};
```

> [!IMPORTANT]
> **Register Release**: The AST is responsible for calling `program.release(val)` for any intermediate values that are no longer needed after a node's computation is finished.
