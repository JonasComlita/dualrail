# Vector Engine

Source of truth: `ternary_isa.h`, `ternary_vm_state.h`, `ternary_vm.h`, `ternary_simd.h`, and `tests/test_vm_widths.cpp`.

---

## State Model

The VM owns one vector file:

| Field | Meaning |
|-------|---------|
| `VECTOR_REGISTER_COUNT` | 8 architectural vector registers: `v0`..`v7` |
| `VMState::vector_length` | Active lane count, default `DEFAULT_VECTOR_LENGTH = 27` |
| `VMState::vregfile` | Array of `TernaryVectorRegister` objects |
| `TernaryVectorRegister::lane` | `std::vector<TernaryValue>` payload, one tagged value per lane |
| `VMState::vector_faults` | Per-lane fault mask and trap class |

`prepareVectorOp()` runs at the start of vector instructions. It resizes vector registers and fault arrays to the current `vector_length`, then clears prior vector faults. This means vector fault state describes the most recent vector instruction, not a cumulative history.

---

## Widths And Tags

Most vector numeric instructions decode the same numeric width suffix as scalar arithmetic:

```text
.t1 .t5 .t10 .t20 .t40 .t50
```

Each lane stores a tagged `TernaryValue`, so operations validate both the requested width and the source lane tag. A lane tagged `L20` is not accepted as a `.t20` numeric lane unless the instruction explicitly performs a conversion.

Predicate lanes use `L1`. `VCMP` writes `L1` predicates, and `VSEL` / `VBLEND` read `L1` predicates.

---

## Opcode Groups

### Elementwise Numeric

| Opcode | Behavior |
|--------|----------|
| `VADD` | `vd[lane] = va[lane] + vb[lane]` |
| `VSUB` | `vd[lane] = va[lane] - vb[lane]` |
| `VNEG` | `vd[lane] = -vs[lane]` |
| `VMUL` | `vd[lane] = va[lane] * vb[lane]` |
| `VDIV` | `vd[lane] = va[lane] / vb[lane]`; zero divisor records a lane `TRAP_DIV_ZERO` |
| `VCMP` | Writes an `L1` predicate lane: -1, 0, or +1 |

`VADD`, `VSUB`, `VNEG`, and `VCMP` try a batch SIMD path for `.t1` and `.t5` when `vector_length <= 512` and all input lanes convert cleanly. Other widths, failed batch validation, `VMUL`, and `VDIV` use the scalar per-lane fallback.

### Selection

| Opcode | Behavior |
|--------|----------|
| `VSEL` | Three-way select from `vneg`, `vzero`, `vpos` using `L1` predicate lanes |
| `VBLEND` | Same dispatcher behavior as `VSEL`; assembler syntax is the blend/plumbing form |

The condition vector must contain `L1` lanes. The three source arms must exactly match the requested numeric mode.

### Broadcast And Length

| Opcode | Behavior |
|--------|----------|
| `VBCAST` | Converts one scalar register to the requested numeric width and writes every lane |
| `VLEN` | Writes the current `vector_length` to a scalar destination register |

`VBCAST` treats a lane-family scalar source as a structural error and traps the VM.

### Contiguous Memory

| Opcode | Address pattern |
|--------|-----------------|
| `VLOAD` | `vd[lane] = DMEM[base + imm9 + lane]` |
| `VSTORE` | `DMEM[base + imm9 + lane] = vs[lane]` |

The public ISA-v2 vector-memory extension stores the vector register in the
`rd` field, the scalar base register in `rs1`, the width in the vector-memory
`func` field, an extension selector in `[12:9]`, and a signed 9-trit immediate
in `[8:0]`.

Loaded values are converted to the suffix type before entering the vector register. Stored lanes are converted to the suffix type before writing DMEM.

### Accumulator And T1 AI

| Opcode | Behavior |
|--------|----------|
| `VDOT.t1 rd, va, vb` | Computes a dot product over `L1` trit lanes and writes the scalar destination as a `LongTriple` value |
| `VMAC.t1 va, vb` | Computes the same dot product and adds it into `VMState::accumulator` using `T40` accumulator arithmetic |
| `VACT.t1 vd, vs` | Writes `L1` sign predicates for numeric source lanes |

`VDOT` and `VMAC` require `FUNC_T1`, but their vector inputs are `L1` predicate/trit lanes. Invalid input lanes set lane faults and are skipped for the dot sum.

### Conversion And Plumbing

| Opcode | Behavior |
|--------|----------|
| `VPACK` | Converts each numeric lane from source width to target width |
| `VUNPACK` | Same conversion machinery, opposite source/target spelling |
| `VPERMUTE` | Uses an index vector to select lanes from another vector |
| `VSWAP` | Swaps whole vector register payloads |

`VPACK` and `VUNPACK` are width conversions in the VM. They do not bit-pack multiple lanes into one scalar word.

### Indexed Memory

| Opcode | Address pattern |
|--------|-----------------|
| `VGATHER` | `vd[lane] = DMEM[base + index[lane]]` |
| `VSCATTER` | `DMEM[base + index[lane]] = vs[lane]` |

The base is a scalar numeric register. The index vector lanes must be numeric. Bad index lanes or physical out-of-range addresses set lane faults. In user mode, address-translation failure routes through the VM trap path because page-table faults are architectural traps, not merely lane-local soft errors.

### Reductions

| Opcode | Behavior |
|--------|----------|
| `VSUM` | Reduces all lanes by addition and writes scalar `rd` |
| `VHMIN` | Writes the minimum lane to scalar `rd` |
| `VHMAX` | Writes the maximum lane to scalar `rd` |

Reduction input lanes must convert to the requested numeric width. Conversion failure traps the VM rather than producing a partial scalar reduction.

---

## Fault Model

Vector instructions use two different fault paths.

Lane-local faults:

- Set `vector_faults.fault_valid[lane] = 1`.
- Store the trap class in `vector_faults.fault_class[lane]`.
- Usually write a typed zero to the destination lane.
- Do not set `VMStatus::TRAPPED`; execution continues to the next instruction.

Structural faults:

- Invalid vector register number.
- Invalid width suffix.
- Scalar base/source register with the wrong family.
- Invalid reduction input where a scalar result cannot be safely completed.

Structural faults call `vm.trap()` or `vm.trapWithCause()` and stop normal execution.

---

## Test Coverage

`tests/test_vm_widths.cpp` covers:

- `VLEN` default length and vector fault reset sizing.
- Elementwise `.t20` and `.t5` vector arithmetic.
- `VCMP` predicate output and three-arm `VSEL`.
- Contiguous `VLOAD`/`VSTORE` conversion.
- Lane-local `VDIV`, wrong-tag, and memory faults.
- Structural `VBCAST` trap behavior.
- Accumulator operations, `VDOT`, `VMAC`, and `VACT`.
- `VPACK`, `VUNPACK`, `VPERMUTE`, `VBLEND`, `VSWAP`.
- `VGATHER` and `VSCATTER` with an out-of-range lane-local fault.
