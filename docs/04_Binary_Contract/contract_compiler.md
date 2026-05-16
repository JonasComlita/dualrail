# Compiler Emission Contract

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_ir.h`, `ternary_asm.h` |

---

## 🎯 Goal
This contract defines the requirements for any compiler backend (e.g., Trit-Lang or C++) targeting the Trit-Stack ISA. Adherence to these rules ensures that compiled binaries are compatible with the OS Substrate and standard debuggers.

---

## 🛠️ Mandatory Emission Rules

### 1. ABI Fidelity
The compiler **must** adhere to the register roles defined in the [ABI Spec](abi_spec.md).
*   **Leaf Optimization**: Leaf functions (functions that do not call others) should avoid saving the Link Register (`lr`) to the stack.
*   **Frame Alignment**: The compiler must ensure the stack remains word-aligned at all times.

### 2. Width Optimization
Ternary hardware is most efficient when using the smallest possible width for an operation.
*   **Boolean Logic**: Use `.l1` (Lane 1) mode for logical operations.
*   **Indexing**: Use `.t5` or `.t10` for small integer loop counters and array indices to reduce hardware power/latency.
*   **Default**: All standard arithmetic should default to `.t40` to ensure consistency with the native 64-bit host emulation.

### 3. Immediate Handling
*   **Load Immediate**: For values exceeding 16 trits, the compiler must use a two-instruction sequence: `MOV` followed by `MOVH`.
*   **Global Access**: The compiler should use `r0`-relative addressing for static globals in DMEM whenever possible, taking advantage of the large 21-million-word range of `imm16`.

---

## 🏗️ Section Management
The compiler must emit source code categorized into the following directives:

| Source Entity | Assembler Directive |
| :--- | :--- |
| **Functions & Logic** | `.text` |
| **Global/Static Variables** | `.data` |
| **Constants (Read-Only)** | `.data` (Hardware does not yet support a `.rodata` fault) |

---

## 🧪 Verification
Compiled code is considered valid if:
1.  It passes the `ternary_asm.h` two-pass validation without errors.
2.  The resulting `.t3` image executes on the VM without triggering `TRAP_ILLEGAL_OP`.
3.  The function epilogue correctly restores all callee-saved registers.
