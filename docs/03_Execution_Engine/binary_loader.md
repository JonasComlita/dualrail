# Binary Loader & Boot Sequence

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| ✅ **Stable** | 2026-05-15 | `ternary_vm_state.h` |

---

## 💾 The Boot Image (`.bin`)
The Trit-Stack does not use a complex ELF or PE header for its native binaries. The boot image is a raw memory dump optimized for fast hardware loading.

### Image Layout
1.  **IMEM Segment**: A sequence of 27-trit `TritWord27` instructions.
2.  **DMEM Segment**: A sequence of 50-trit `LongTriple` data words.

*   **Trit Ordering**: Words are stored in **Little-Trit-Endian** format (Trit 0 is stored in the least-significant bits of the host byte-stream).
*   **Extension**: Canonical raw images use the **`.t3`** extension.

---

## 🏗️ The Loading Process

### 1. Mapping & The Boundary
Because the `.t3` format lacks a header, the loader must be provided with the **Segment Boundary** (the number of instruction words) via the host environment or a sidecar metadata file.

*   **Offset 0 to Boundary** $\rightarrow$ `IMEM[0]`.
*   **Boundary to EOF** $\rightarrow$ `DMEM[0]`.

### 2. Initialization
Once memory is populated, the loader performs a **Soft Reset**:
1.  **PC** is set to `0`.
2.  **SP** is set to the very end of `DMEM`.
3.  **r0** is confirmed as `0`.
4.  **status** is set to `RUNNING`.

### Interpreting the Stack Pointer in a Register Dump

`r26` starts at `DMEM.size() - 1` and the stack grows downward. A normal
function prologue subtracts its frame size and its epilogue adds that size back
before `RET`, so a successful program commonly ends with `r26` at the original
top-of-memory value. That final value is not the amount of stack used; peak
usage is the difference between the initial `r26` and the lowest `r26` reached
while the program ran. For example, decimal `999999` is the balanced-ternary
value `+-0-0-++-+-000`.

---

## 🚩 Entry Point Invariants
*   **The Zero Rule**: Every valid Trit-Stack program must start at `IMEM[0]`. 
*   **The Trap Rule**: If the loader detects an `INVALID` (0b11) trit pattern in the incoming IMEM stream, it must refuse to start the machine and signal a `LoaderFault`.

> [!IMPORTANT]
> **Static Data**: Global variables initialized in the source code (e.g., `.data` sections) are pre-baked into the DMEM segment of the binary. The loader must ensure these are written to DMEM before the first instruction is fetched.
