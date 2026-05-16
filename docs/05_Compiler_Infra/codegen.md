# Codegen & Lowering Strategies

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| 🛠️ **Draft** | 2026-05-15 | `ternary_ir.h` |

---

## 🏗️ The Lowering Pipeline
Codegen is the process of recursively visiting [AST Nodes](ast_nodes.md) and emitting [Ternary IR](ternary_ir.md) instructions.

### 1. The Recursive Visitor
The compiler uses a **Depth-First Search (DFS)** to process the tree.
*   **Expressions**: Visit children first, then emit the operation using the children's result values.
*   **Statements**: Process in linear order. Control flow nodes (Loops/Ifs) emit labels and branches to manage the jump logic.

---

## ⚡ Optimization Strategies

### 1. Register Pressure & Re-use
The most critical optimization in the Trit-Stack is the **Immediate Release** of temporary registers.
*   **Heuristic**: After a `BinaryOpNode` emits its instruction, it must call `program.release()` on both input operands if they were temporary values (not named variables).
*   **Benefit**: This allows complex expressions to be computed using only 3–4 physical registers.

### 2. Constant Folding
To minimize instruction count, the codegen visitor must evaluate expressions involving only `LiteralNodes` at compile-time.
*   **Example**: `x = 1 + 2` is lowered directly as `MOV rX, 3` rather than a `MOV/MOV/ADD` sequence.

### 3. Strength Reduction & Peephole
The codegen layer should substitute expensive instructions for cheaper alternatives and perform a final "Peephole" pass:
*   **`MUL r1, r1, 3`** $\rightarrow$ `TLSHIFT r1, r1, 1` (1-trit left shift).
*   **Redundancy**: Remove any `COPY rX, rX` or `ADD rX, rX, 0` patterns produced by naive lowering.

### 4. Unique Label Generation
When lowering `IfNodes` or `WhileNodes`, the codegen must generate globally unique labels (e.g., `_L0`, `_L1`) to ensure that multiple control-flow blocks do not collide in the final IR stream.

### 3. Fused AI Kernels
When the codegen detects a pattern of matrix multiplication followed by an activation function, it should collapse the nodes into the **Fused VDOT-VACT Cycle**:

```asm
; Fused Neural Cycle
ACLR.t1
VDOT.t1  v1, v2    ; Matrix multiply
VACT.t1  r1, rA    ; Fused activation & store
```

---

## 🛡️ Correctness Checks
1.  **Type Propagation**: The codegen must ensure that the width of the destination register matches the width of the inputs (e.g., adding two `T5` values must produce a `T5` result).
2.  **Stack Integrity**: For function calls, the codegen must emit the [ABI-compliant prologue and epilogue](../04_Binary_Contract/abi_spec.md) to preserve the `lr` and `sp` registers.
