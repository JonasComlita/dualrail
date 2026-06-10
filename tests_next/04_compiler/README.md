# 04 Compiler

Focused tests for the compiler implementation.

Required areas:

- lexer, parser, and diagnostics;
- type inference and pointer-state rules;
- IR lowering;
- register allocation and spills;
- codegen and linking;
- dead stripping;
- future branch reduction and `TSEL` if-conversion.

Old reference tests: `tests/test_phase7_compiler.cpp`,
`tests/test_phase_c5_pred.cpp`.
