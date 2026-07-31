---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "isa-vm-execution",
  "title": "ISA and VM execution",
  "module": "4. Execution",
  "level": "programmer",
  "order": 40,
  "summary": "Trace a 27-trit instruction word through decode, registers, memory, and three-way control flow.",
  "prerequisites": ["tritwise-operations"],
  "sources": [
    { "path": "ternary_isa.h", "label": "Instruction formats and opcodes", "kind": "source" },
    { "path": "ternary_vm_state.h", "label": "VM state model", "kind": "source" },
    { "path": "ternary_vm.h", "label": "VM execution loop", "kind": "source" },
    { "path": "docs/00_Quick_Ref/opcode_table.md", "label": "Opcode quick reference", "kind": "documentation" }
  ],
  "evidence": [
    { "path": "tests/test_isa_asm.cpp", "label": "ISA and assembler tests", "kind": "test" },
    { "path": "tests/test_vm_widths.cpp", "label": "VM width tests", "kind": "test" }
  ],
  "next": "compiler-pipeline",
  "interactive": {
    "kind": "choice",
    "title": "Decode check",
    "prompt": "What does the format trit of a TritWord27 choose?",
    "options": ["The VM memory size", "R-, I-, or B-type instruction shape", "The host CPU endian order"],
    "answer": 1,
    "explanation": "The format trit distinguishes register, immediate, and branch forms before the rest of the instruction fields are decoded."
  }
}
---

## State before instructions

`VMState` keeps a program counter, 27 general-purpose registers, a separate
trap register, instruction memory, word-addressed data memory, CSRs, and vector
registers. Instruction and data memory are separate so a data store cannot
rewrite the program being executed.

## Three-way control

`TCMP` produces `-1`, `0`, or `+1`. The branch family can select one of those
three states, and `TSEL` can choose a value without a branch. That is a machine
level consequence of `t1` being a trit, not a boolean stored in disguise.

## Instruction shape

The ISA stores a 27-trit instruction in 54 meaningful host bits. Its format trit
selects R-type, I-type, or B-type decoding. Register fields store an offset from
the middle register index, and immediates use signed balanced ternary fields.

## Prerequisites

Understand lanes and `t1` predicates. The first TCL program provides the source
shape; this page provides the execution shape.

## Next step

Continue to **Compiler pipeline** and follow how source is transformed into
these instruction words.
