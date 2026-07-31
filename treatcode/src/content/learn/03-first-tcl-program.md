---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "first-tcl-program",
  "title": "Your first TCL program",
  "module": "3. Language",
  "level": "beginner",
  "order": 30,
  "summary": "Write a minimal .trit function and move from a guided example into a runnable TreatCode challenge.",
  "prerequisites": ["tritwise-operations"],
  "sources": [
    { "path": "TCL_Spec_1.0.md", "label": "TCL 1.0 language specification", "kind": "specification" },
    { "path": "tcl_lexer.trit", "label": "Native TCL lexer", "kind": "source" },
    { "path": "tcl_parser.trit", "label": "Native TCL parser", "kind": "source" },
    { "path": "treatcode/src/App.tsx", "label": "TreatCode challenge surface", "kind": "product" }
  ],
  "evidence": [
    { "path": "tests/test_compiler_frontend_smoke.trit", "label": "Compiler frontend smoke input", "kind": "test" },
    { "path": "tests/test_phase7_compiler.cpp", "label": "Compiler and runtime tests", "kind": "test" }
  ],
  "next": "isa-vm-execution",
  "interactive": {
    "kind": "code",
    "title": "Try the first function",
    "starter": "import ulib;\n\nfn first_value() -> t40 {\n    return 1 + 2;\n}",
    "expectedIncludes": ["fn first_value", "return 1 + 2"],
    "explanation": "This small check confirms the shape of a TCL function. Use Open challenges to send a real solution through the TreatCode compiler and VM."
  }
}
---

## The smallest useful program

TCL source files use the `.trit` extension. A function has a name, typed
parameters, a return type, and a body:

```tcl
import ulib;

fn first_value() -> t40 {
    return 1 + 2;
}
```

The example returns a numeric `t40`. It does not pretend that a host integer is
the language's type system; the compiler still sees the declared ternary width.

## Prerequisites

Know the numeric/lane distinction and the idea that `t1` is a three-valued
predicate. No local compiler setup is required for this page: use the guided
check, then use the in-app challenge surface to compile and run a solution.

## What to inspect next

The language source is parsed into an AST, checked, lowered, and eventually
assembled. The compiler and VM pages follow that path while keeping the
production files and test evidence beside the explanation.

## Next step

Continue to **ISA and VM execution** to see what the function becomes after
source-level checking.
