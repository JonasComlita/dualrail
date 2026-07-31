---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "compiler-pipeline",
  "title": "Compiler pipeline",
  "module": "5. Compilation",
  "level": "programmer",
  "order": 50,
  "summary": "Follow TCL from tokens to AST, type checks, IR, register allocation, TASM, and an image.",
  "prerequisites": ["first-tcl-program", "isa-vm-execution"],
  "sources": [
    { "path": "ternary_compiler_lexer.h", "label": "Host compiler lexer", "kind": "source" },
    { "path": "ternary_compiler_parser.h", "label": "Host compiler parser", "kind": "source" },
    { "path": "ternary_compiler_ast.h", "label": "Compiler AST", "kind": "source" },
    { "path": "ternary_compiler_codegen.h", "label": "TASM code generation", "kind": "source" },
    { "path": "tritc.cpp", "label": "Compiler driver", "kind": "source" }
  ],
  "evidence": [
    { "path": "tests/test_phase7_compiler.cpp", "label": "Compiler pipeline tests", "kind": "test" },
    { "path": "tests/test_tcl_asm.cpp", "label": "TCL assembler tests", "kind": "test" }
  ],
  "next": "boot-and-traps",
  "interactive": {
    "kind": "choice",
    "title": "Pipeline check",
    "prompt": "Which artifact is emitted immediately before the assembler encodes instruction words?",
    "options": ["Raw source text", "TASM text and structural IR metadata", "A screenshot of the VM"],
    "answer": 1,
    "explanation": "The code generator emits textual TASM plus structural metadata; the assembler then resolves labels and encodes TritWord27 values."
  }
}
---

## Source to image

The reference pipeline is:

```text
.trit source -> tokens -> ModuleAst -> type checks -> structural IR
  -> optimization -> register allocation -> TASM -> assembler/linker image
```

The type layer catches wrong widths, invalid conditions, unsafe operations,
pointer-state mistakes, and moved ownership before code generation. This is why
the compiler is more than a text-to-text translator.

## Prerequisites

Read the first TCL and ISA pages. You should know both the source shape and the
instruction shape before studying the lowering boundary.

## Production and evidence

The header files are the current host compiler implementation. The native
`tcl_*.trit` files mirror that architecture for the in-OS path; the focused
compiler and assembler tests show which behavior is currently validated.

## Next step

Continue to **Boot and traps** and inspect what happens when an encoded image
crosses into execution.
