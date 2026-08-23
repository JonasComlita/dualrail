# Vector ABI Specification

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| Hardware register contract and ABI v3 boundary integrated; v2 compatibility retained | 2026-08-17 | `executable_header_v3.h`, `ternary_vm_state.h`, `ternary_compiler_types.h`, `ternary_compiler_codegen.h`, `kernel.trit` |

---

## 🚀 SIMD Register Convention

The architectural vector register file is available to VM/ISA operations,
but its existence does not by itself define a source-language function ABI.
The release compiler and image builder use
`trit.compiler.function-abi.v3` (version 3) by default. An explicit
`trit.compiler.function-abi.v2` compatibility path remains available for
legacy applications and migration fixtures.

## Executable and function ABI v3

The v3 executable identity is separate from the existing `.tboot` container
version. It uses function ABI 3, vector ABI 1, fixed `VLEN = 27`, eight vector
registers, and a 279-word process vector-context contract. Required feature
bits identify the v3 executable profile, vector geometry, vector context, and
vector spill support. `executable_header_v3.h` provides versioned validation
and round-trip helpers while the v2 decoder remains unchanged.

Each of the 27 lanes carries one architectural T40 word. Thus, `VLEN = 27`
means 27 lanes and a spill size of 27 T40 words; it never means a 27-trit lane.
This preserves the T40 native scalar contract and the design goal of a ternary
equivalent of a 64-bit computer.

The vector function rules are:

- `v0`–`v3` carry vector arguments and `v0` carries a vector return;
- `v4`–`v7`, the accumulator, and lane-fault state are caller-saved;
- `VLEN` is callee-preserved and must be 27 at entry and exit;
- vector arguments beyond the first four use 27-word, 9-aligned outgoing
  stack slots sharing the scalar/T50/aggregate stack cursor;
- vector spills use 27 words with 9-word alignment.

Vector syscalls, atomics, and foreign interfaces remain fail-closed. The
versioned loader, image propagation, trap/scheduler integration, and exact
tagged context ownership are integrated. The release gate keeps v2 readable
and rejects unknown versions, mixed object ABIs, malformed vector geometry,
and v3 images without VECTOR_CONTEXT support.

### 1. Vector Register Roles
The 8 vector registers are assigned specific architectural roles:

| Register | Name | Role | Preservation |
| :--- | :--- | :--- | :--- |
| **v0** | `va0` | Argument 0 / Return Value | Caller-Saved |
| **v1 - v3** | `va1 - va3` | Arguments 1 - 3 | Caller-Saved |
| **v4 - v7** | `vt0 - vt3` | Temporary Vectors | Caller-Saved |

ABI v2 rejects first-class vector function parameters and returns before target
emission. ABI v3 admits only the explicitly supported vector signatures and
SSA operations; unsupported element types and interfaces still fail closed.
The compiler must not scalarize the value, silently pass a scalar register, or
replay the AST with a private convention.

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

The VLEN rule is enforced by the v3 contract. The kernel saves and restores
the separately allocated vector context across entry, preemption, timer and
syscall switches, fork, exec, exit, and task-slot reuse.

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

## Compatibility and release checklist

The focused codec/compiler test is `test_executable_abi_v3`; the OS and
production suites additionally cover loader, image, scheduler, and process
handoff behavior. ABI v2 applications must continue to boot unchanged, while
new release images use v3 unless `TRIT_BUNDLED_APP_FUNCTION_ABI=2` is set.
