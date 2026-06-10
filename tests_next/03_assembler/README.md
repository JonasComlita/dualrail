# 03 Assembler

Focused tests for `.tasm` parsing and encoding.

Required areas:

- labels and branch targets;
- `.org`, `.word`, and data placement;
- CSR/register aliases;
- exact emitted words;
- malformed input diagnostics.

Old reference tests: `tests/test_isa_asm.cpp`, `tests/test_tcl_asm.cpp`.
