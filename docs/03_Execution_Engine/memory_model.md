# Memory Model

Source of truth: `ternary_vm_state.h`, `ternary_vm.h`, `ternary_isa.h`, `tests/test_kernel.cpp`, and `tests/test_vm_widths.cpp`.

---

## Harvard Layout

The VM uses separate instruction and data memories.

| Memory | Type | Default size | Word type |
|--------|------|--------------|-----------|
| IMEM | `TernaryInstructionMemory` | 4096 words | `TritWord27` instruction |
| DMEM | `TernaryMemory` | 1,000,000 words | tagged `TernaryValue` data |

The PC indexes IMEM. `LOAD`, `STORE`, vector memory operations, atomics, GPU helpers, block-device helpers, and the kernel data paths access DMEM. There is no byte addressing and no instruction that stores into IMEM directly from guest code.

Host backing can be dense or sparse. `MemoryBacking::Auto` switches to sparse backing above `SPARSE_MEMORY_DENSE_LIMIT_WORDS` (4 Mi words). Sparse host pages use `SPARSE_VM_PAGE_WORDS = 4096`; this is separate from the guest MMU page size.

---

## Word Addressing

All architectural memory addresses are word addresses.

- `step()` normally advances `pc` by 1 instruction word.
- `LOAD` and `STORE` compute `addr = toLong(rs1) + imm16`.
- `VLOAD` and `VSTORE` compute `base + imm9 + lane` in the public ISA-v2 encoding.
- `VGATHER` and `VSCATTER` compute `base + index[lane]`.
- MMU pages contain `MMU_PAGE_WORDS = 27` words.

Out-of-range memory access returns `MemFaultCode::OUT_OF_RANGE` from the memory container. The dispatcher converts that into `TRAP_MEM_FAULT` or a lane-local vector memory fault, depending on the instruction.

---

## Scalar Load And Store

`LOAD`:

1. Requires the base register to hold a numeric `TernaryValue`.
2. Computes virtual address `base + imm16`.
3. Calls `translateLoadAddress()`.
4. Loads one DMEM word from the translated physical address.
5. Writes the loaded tagged value to `rd`.

`STORE`:

1. Requires the base register to hold a numeric `TernaryValue`.
2. Computes virtual address `base + imm16`.
3. Calls `translateStoreAddress()`.
4. Stores the tagged value from `iw.rs_store`.
5. Invalidates a matching atomic reservation.

Encoding quirk: `STORE` reuses the `rd` field position as the source register.
`VersionedInstructionCodec::decode()` exposes that semantic register as
`rs_store`.

---

## Address Translation

Kernel mode bypasses user range and MMU translation, but still requires the final address to be in range.

User mode has two paths:

| Mode | Fetch/load/store rule |
|------|-----------------------|
| MMU disabled | Address must be inside the matching CSR range: `user_imem_base..user_imem_limit` or `user_dmem_base..user_dmem_limit` |
| MMU enabled | Address is translated through the matching page table: `user_imem_ptbr/user_imem_pages` or `user_dmem_ptbr/user_dmem_pages` |

Page translation uses 27-word pages. A page-table entry contains:

- Physical page number (`ppn`).
- `present`.
- `user`.
- `read`.
- `write`.
- `execute`.

Translation failure records:

- `page_fault_addr`.
- `page_fault_access` (`fetch`, `load`, or `store` access code).
- A routed cause such as fetch/load/store page fault or protection fault.

If trap routing is enabled, `trapWithCause()` stores the fault PC in `epc`, writes `cause`, switches to kernel privilege, disables interrupts, and jumps to `tvec`. Without routed traps, the VM enters `VMStatus::TRAPPED`.

---

## Vector Memory

Vector memory instructions share the same translation machinery.

Contiguous operations:

- `VLOAD`: reads `base + imm9 + lane`.
- `VSTORE`: writes `base + imm9 + lane`.

Indexed operations:

- `VGATHER`: reads `base + index[lane]`.
- `VSCATTER`: writes `base + index[lane]`.

Physical out-of-range addresses are lane-local vector faults. User-mode page translation failures are routed as architectural traps because the fault belongs to the virtual memory subsystem, not just to a bad arithmetic lane.

Successful vector stores call `noteStoreForReservation()` so they can invalidate an active atomic reservation on the same physical word.

---

## Atomics

The ternary atomic instructions are `TLDR`, `TSTR`, and `FENCE`.

| Opcode | Operands | Behavior |
|--------|----------|----------|
| `TLDR` | `rd, raddr` | Loads from `raddr`, writes `rd`, and records an atomic reservation on the translated physical address |
| `TSTR` | `rstatus, raddr, rdesired, rexpected` | Stores `rdesired` only if the reservation is still valid and current memory equals `rexpected` |
| `FENCE` | memory order suffix | Validates the order suffix and acts as an architectural barrier marker |

`TSTR` status result:

| Result trit | Meaning |
|-------------|---------|
| `+1` | Reservation matched, expected value matched, store succeeded |
| `0` | Reservation matched, but current memory did not equal expected |
| `-1` | Reservation was lost before the store |

Any `TSTR` clears the reservation before returning. Ordinary scalar or vector stores invalidate the reservation if they write the reserved physical address. Traps also clear the reservation.

Memory order suffixes are encoded in the function field:

| Suffix | Order constant |
|--------|----------------|
| `.-1` | relaxed |
| `.0` | acquire-release |
| `.+1` | sequentially consistent |

The current VM executes instructions sequentially and does not reorder memory operations, so `FENCE` has no runtime side effect beyond validating the suffix and preserving the architectural marker.

---

## Consistency

The interpreter is sequential at the architectural level:

- One `step()` fetches, decodes, executes, and writes back one instruction.
- PC advances only after successful non-terminal execution.
- `HALT` and traps leave PC pointing at the terminal or faulting instruction, except routed traps which store the fault PC in `epc` and jump to `tvec`.
- There is no speculative memory access in the interpreter.

The block cache and decoded-trace paths keep architecture, mapping, MMU, ASID,
and memory generation identities so cached execution can notice relevant
changes and fall back at the precise architectural PC. The experimental
x86-64 native backend consumes the same micro-op and cache-key contracts; see
`decoded_trace_and_native_jit.md`.

---

## Test Coverage

Relevant tests:

- `tests/test_vm_widths.cpp`: scalar `LOAD`/`STORE`, vector memory, vector memory faults, `FENCE` acceptance.
- `tests/test_kernel.cpp`: user range checks, MMU fetch/data translation, page faults, protection faults, kernel MMU bypass, `TLDR`/`TSTR` success/mismatch/collision behavior.
- `tests/test_isa_asm.cpp`: assembler/disassembler syntax for `TLDR`, `TSTR`, and `FENCE` memory order suffixes.
- `tests/test_ternary_ir.cpp` and `tests/test_phase7_compiler.cpp`: IR/compiler lowering for ternary atomic operations.
