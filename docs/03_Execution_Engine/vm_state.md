# VM State

Source of truth: `ternary_vm_state.h`

---

## Overview

`VMState` is the complete, serializable state of the ternary VM. It is a pure data structure — all execution logic lives in `ternary_vm.h`.

```cpp
struct VMState {
    // Program Counter
    int pc = 0;

    // General-purpose register file
    RegisterFile regfile;     // r0–r26; r0 always reads as zero

    // Trap register
    TernaryValue trap_reg;    // r27; written by VM on fault

    // Instruction memory (word-addressed, each word = TritWord27)
    InstructionMemory imem;

    // Data memory (word-addressed, each word = TernaryValue/T40)
    DataMemory dmem;

    // Control & Status Registers (0–46)
    CSRFile csrs;

    // Privilege mode
    PrivilegeMode privilege;

    // Vector register file (8 registers, each N lanes deep)
    VectorRegisterFile vregfile;

    // Vector length (current lane count)
    int vector_length;

    // AI accumulator
    TernaryValue accumulator;

    // Vector fault tracking
    VectorFaultFile vector_faults;
};
```

---

## Register File

`RegisterFile` holds 27 `TernaryValue` slots (r0–r26).
- `read(i)` returns zero for r0 regardless of contents.
- `write(i, val)` silently ignores writes to r0.
- All registers initialize to zero (T40 mode).

---

## Memory Model

### Instruction Memory (IMEM)

- Stores `TritWord27` instruction words.
- Word-addressed: PC is an integer index into the instruction array.
- PC starts at 0 (boot entry).
- Bounded: any fetch outside `[0, imem.size())` → `TRAP_FETCH_FAULT`.

### Data Memory (DMEM)

- Stores `TernaryValue` (T40 = `LongTriple`) words.
- Word-addressed: LOAD/STORE use word indices, not byte offsets.
- Bounded: any access outside `[0, dmem.size())` → `TRAP_MEM_FAULT`.
- `FENCE` instruction provides sequential consistency across DMEM operations.

### Address Translation (MMU)

When `CSR mmu_enable != 0`:
- User-mode IMEM accesses are translated via `user_imem_ptbr` + `user_imem_pages`.
- User-mode DMEM accesses are translated via `user_dmem_ptbr` + `user_dmem_pages`.
- Page fault → `TRAP_MEM_FAULT` with cause `OS_CAUSE_{FETCH,LOAD,STORE}_PAGE_FAULT`.

In Kernel mode, addresses pass directly without translation.

---

## CSR File

47 CSR slots (indices 0–46). Each stores a `TernaryValue`. See [register_map.md](../00_Quick_Ref/register_map.md) for the full table.

Key CSRs the VM reads internally:
- `CSR_STATUS` — current privilege mode
- `CSR_EPC` — saved PC on trap entry
- `CSR_TVEC` — trap handler address
- `CSR_SYSCALL_ID` — service ID for SYSCALL

---

## Vector Register File

8 vector registers (`VECTOR_REGISTER_COUNT = 8`).
Each register is a `std::vector<TernaryValue>` of length `vector_length`.

```cpp
struct VectorRegister {
    std::vector<TernaryValue> lane;
    void reset(int length);                         // resize, fill zero
    TernaryValue read(int lane_index) const;
    void write(int lane_index, TernaryValue val);
};
```

Width is per-instruction (via `func` field). Vector operations work on all lanes in parallel.

---

## AI Accumulator

A single `TernaryValue` at T40 precision.

| Opcode | Effect |
|--------|--------|
| `ACLR`  | `accumulator ← 0` |
| `ALOAD` | `accumulator ← convertToT40(reg[Rs1])` |
| `AADD`  | `accumulator ← accumulator + convertToT40(reg[Rs1])` |
| `ASUB`  | `accumulator ← accumulator − convertToT40(reg[Rs1])` |
| `AMUL`  | `accumulator ← accumulator × convertToT40(reg[Rs1])` |
| `ASTORE`| `reg[Rd] ← accumulator` |

The accumulator feeds into `VDOT` (T1 × T1 dot product) which returns a `LongTriple` (T50) sum.

---

## VM Execution Status

```cpp
enum class VMStatus {
    Running,    // Executed one instruction, PC advanced
    Halted,     // HALT instruction reached
    Trapped,    // Fault → r27 written, VM halted
    Blocked,    // OS syscall returned "blocked" status
};
```

---

## Trap Mechanism

When any fault occurs:
1. VM writes fault code to `r27` (`fault_valid = +1`, `fault_class = ...`).
2. VM **does not advance PC** (PC points at the faulting instruction).
3. VM status becomes `Trapped`.
4. **If OS trap handler is configured** (`CSR_TVEC != 0`): VM instead saves PC → `CSR_EPC`, saves privilege → `CSR_STATUS`, and jumps to `CSR_TVEC` in Kernel mode.

---

## Step-by-Step Execution

```cpp
// One instruction
VMStatus status = vm::step(state);

// Run until halt/trap or max_steps
vm::RunResult result = vm::run(state, max_steps);
// result.status, result.steps_executed, result.pc_final
```

`step()` performs exactly:
1. Fetch `TritWord27` at `state.pc`
2. Decode via `InstructionWord::decode()`
3. Check for malformed word → trap if so
4. Execute opcode
5. Advance PC (unless HALT/TRAP)
