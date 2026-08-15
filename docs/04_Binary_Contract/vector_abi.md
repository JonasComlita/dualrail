# Vector ABI Specification

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| Hardware register contract stable; compiler function boundary reserved | 2026-08-13 | `ternary_vm_state.h`, `ternary_compiler_types.h`, `ternary_compiler_codegen.h` |

---

## 🚀 SIMD Register Convention

The architectural vector register file is available to VM/ISA operations,
but its existence does not by itself define a source-language function ABI.
The compiler's default function contract is
`trit.compiler.function-abi.v2` (version 2). An opt-in
`trit.compiler.function-abi.v3` profile now defines caller-owned aggregate
returns, but both profiles keep first-class vector function boundaries
fail-closed until the VM/vector-state contract is complete.

### 1. Vector Register Roles
The 8 vector registers are assigned specific architectural roles:

| Register | Name | Role | Preservation |
| :--- | :--- | :--- | :--- |
| **v0** | `va0` | Argument 0 / Return Value | Caller-Saved |
| **v1 - v3** | `va1 - va3` | Arguments 1 - 3 | Caller-Saved |
| **v4 - v7** | `vt0 - vt3` | Temporary Vectors | Caller-Saved |

These roles are an architectural design target, not an enabled compiler
boundary. In ABI v2 and v3, a function with a `vec<T>` parameter or return
value is rejected before target emission. The compiler must not scalarize the
value, silently pass a scalar register, or replay the AST with a private
convention.

### 2. The Accumulator Protocol (`rA`)
The high-precision 50-trit accumulator is a global shared resource.
*   **Ownership**: The accumulator is **Caller-Saved**.
*   **Initialization**: Any function intending to use `VDOT` or `VMAC` for a fresh calculation must explicitly execute `ACLR` (Accumulator Clear) in its prologue.
*   **Persistence**: A function may return a "running sum" in `rA` if documented in the API signature.

---

## ⚙️ Control State Management

### Vector Length (`VLEN`)
The hardware `vector_length` setting affects all subsequent vector operations.
*   **Preservation**: `VLEN` is **Callee-Saved**.
*   **Contract**: If a function modifies the vector length to optimize a local loop, it must save the original length and restore it before the `RET` instruction.

The VLEN rule is a requirement for a future enabled vector function profile;
it does not make VLEN preservation available at either compiler function
boundary profile.

### Lane Faults
The `VectorFaultState` is **Volatile**. 
*   Functions are not required to preserve or clear the lane fault bits of the caller. 
*   If a function completes successfully, the caller can assume the faults in the return registers are either zero or relevant to the new result.

---

## 📦 Data Alignment
*   **Memory Pointers**: When passing pointers to vector data (DMEM), the pointers should be word-aligned.
*   **Strides**: For matrix operations, the row-stride should ideally be a multiple of the hardware `VLEN` (default 27) to maximize throughput.

> [!WARNING]
> **Register Spilling**: Spilling a single vector register to the stack requires 27 (or `VLEN`) `STORE` instructions. High-performance code should be structured to avoid vector spills entirely through register-pressure analysis at the compiler level.

## Compiler boundary checklist for a future ABI version

Before first-class vectors can be enabled, a versioned contract must specify
all of the following as one change: fixed lane width (or a dynamic-length
descriptor), argument and return registers, caller/callee preservation of
vector registers and VLEN, lane-fault and accumulator state, word-aligned
stack spill encoding, and cross-version link rejection. ABI v3 publishes the
reserved boundary name `unsupported-vm-vector-register-boundary` so tools and
linkers can distinguish this dependency from a generic type error; it still
fails closed until VM/vector-state ownership and spill lowering are added.
