# Ternary VM Refactor - Task Tracker

## Phase 2 - ISA Completion

### Step 1 - Update Instruction Set Architecture
Status: Complete

- Opcodes 59-73 are defined.
- R4 decode/encode support is present for `TWCMP` and `TCLAMP`.
- Human-readable opcode names and disassembly are wired.

### Step 2 - Expand Virtual Machine State and Dispatcher
Status: Complete

- VM execution exists for `TWCMP`, `TCLAMP`, `CALLR`, `JMPR`, `TMOD`, `TLSHIFT`, `TRSHIFT`, `TMAC`, `TCOUNT`, `TSCAN`, `SYSCALL`, `FENCE`, `VSUM`, `VHMIN`, and `VHMAX`.
- `TWCMP` and `TCLAMP` trap on inverted bounds.
- `TMOD` traps on divisor zero and uses truncation-toward-zero quotient semantics.
- `TSCAN` returns first non-zero stored trit position and returns T1 `-1` for all-zero input.

### Step 3 - Update Assembler and IR Compiler
Status: Complete

- Added assembler mnemonics and operand parsing for all 15 new opcodes.
- Added R4 mnemonic parsing for `twcmp.tN rd, value, low, high` and `tclamp.tN rd, value, low, high`.
- Added IR builder helpers for scalar arithmetic/analysis, indirect control flow, syscall/fence, and vector reductions.
- `CALLR` and `JMPR` use absolute instruction-memory PC targets from numeric scalar registers.
- `SYSCALL` uses immediate sandbox service IDs: `1` prints `r1`, `2` appends newline, `3` clears the buffer.

### Step 4 - Comprehensive Verification
Status: In Progress

Passing:
- `test_multiwidth_vm`
- `test_ternary_ir`
- `test_native_ops`
- `test_numeric_workloads`
- `test_ternary_lanes`
