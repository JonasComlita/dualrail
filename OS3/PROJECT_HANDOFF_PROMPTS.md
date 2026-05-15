# Ternary Computer Architecture Project — Sequential Handoff Prompts for Next Agent

**Project Goal**: Build a complete toolchain for a balanced ternary (base-3, {-1,0,+1}) ISA, including hardware specification (ISA), assembler (ASM), virtual machine (VM), intermediate representation (IR) backend, abstract syntax tree (AST) + frontend, full compiler for a C-like language, and eventually a minimal OS (xv6-style) + FPGA/ASIC implementation using dual-rail encoding.

**Current Date**: 2026-05-15  
**Repo**: https://github.com/JonasComlita/dualrail (private — clone with appropriate credentials)  
**Related Public Repo**: https://github.com/JonasComlita/ternary (LongTriple 50-trit emulator for physics benchmarks — reference only for ternary math ideas)  
**Key Local Docs** (in attachments/ or to be committed):  
- `CVT_Rounding_Modes.docx` (extensive cross-architecture CVT research + Ternary proposal)  
- `Ternary_Logic_Gates.docx` (complete gate library + ISA opcode mapping)  
- `os_fundamentals.md` + `xv6.md` (OS path, memory model, interrupts, calling convention, etc.)

**Current State (as of handoff)**:
- **ISA**: Core defined in `ternary_isa.h` (R-type instructions, 27 registers r0-r26 + specials like r27 trap, r25 link, r26 SP; opcodes for gates: NEG, ABS, TMIN, TMAX, ADD, MUL, TCMP, TSEL/BRN/BRZ/BRP, CVT, etc.). Harvard architecture (separate IMEM/DMEM). Fixed 27-trit instructions (54 binary bits). Dual-rail encoding planned for FPGA ( -1=00, 0=01, +1=10, INVALID=11).
- **ASM**: `ternary_asm.h` — two-pass assembler, supports labels, pseudo-ops, emits binary.
- **VM**: Functional VM for execution, testing, and validation of ISA semantics. Supports traps via r27.
- **IR (in progress)**: `ternary_ir.h` — SSA-like Program builder with Value, Type (T1/T5/T10/.../T50, Lanes), register allocation (24 scalar regs?), instruction selection, lowering to assembly text via asm layer. Already handles basic arithmetic, load/store, constants, mul/add chains. Missing: full calling convention enforcement, stack frame/spilling, struct support.
- **Next Immediate Step**: Build AST + parser frontend to turn source code into IR Program objects → full compiler.
- **Research Completed**: Extensive comparison of conversion (CVT) opcodes and rounding across x86/Intel, ARM, RISC-V, IBM Power (see CVT_Rounding_Modes.docx). Ternary proposal: **Trit-Zone Rounding (TZR)** as native default (no midpoint case exists because 3^N is odd; zones divide remainder evenly into down/exact/up). Also full logic gate taxonomy mapped to ISA opcodes.
- **Language Target**: C subset (C89-level: structs, pointers, arrays, arithmetic, control flow, functions, inline asm) sufficient for kernel/OS code. Avoid full C++ features initially (no exceptions/RTTI in kernel mode).
- **Longer Term**: Interrupt/exception architecture, calling convention formalization, memory model (likely weak or TSO analog), atomic ops (ternary LR/SC or CAS with 3-outcome trit), minimal xv6-style OS, FPGA bring-up, self-hosting compiler.

**Important Notes for Next Agent**:
- All development in modern C++17/20. Use RAII, templates judiciously.
- Dual-rail encoding: Implement/validate in VM and planned FPGA (see Ternary_Logic_Gates.docx §7.1).
- Ternary advantages to exploit: No midpoint in rounding (TZR), symmetric around zero, natural 3-state for page tables/permissions (absent/RO/RW in 1 trit), negative addresses for kernel space, compact trit-zone logic.
- Test everything against the VM. Use the provided audit-style benchmarks or simple programs once compiler exists.
- Commit frequently to dualrail/main. Keep docs updated.
- When stuck on ternary-specifics, re-read the two .docx files thoroughly — they contain the architectural vision.

---

## Prompt 1: Initial Setup, Repo Familiarization, and Project State Audit

You are taking over the Ternary Computer Architecture Project (dualrail repo). Your first task is to fully orient yourself.

**Steps (do them in order, document findings in a new `STATUS.md` or update README):**

1. Clone the dualrail repo (use GitHub CLI or git with credentials for private repo):  
   `git clone https://github.com/JonasComlita/dualrail.git`  
   `cd dualrail`

2. Explore the current directory structure. List all files recursively. Identify presence/absence of:
   - `ternary_isa.h` (or include/ternary_isa.h)
   - `ternary_asm.h`
   - `ternary_vm/` or `vm.cpp` / `ternary_vm.h`
   - `ternary_ir.h` (the current work-in-progress IR backend)
   - `docs/` or attachments with the four key documents (CVT_Rounding_Modes, Ternary_Logic_Gates, os_fundamentals.md, xv6.md)
   - Any existing `ast/`, `parser/`, `compiler/`, `tests/`, `examples/`, `fpga/`, `os/`
   - CMakeLists.txt, Makefile, or build system
   - .github/ workflows if any

3. If the key .h files or docs are missing from the repo (they may be local-only currently), create the standard structure:
   ```
   dualrail/
   ├── include/
   │   ├── ternary_isa.h
   │   ├── ternary_asm.h
   │   ├── ternary_ir.h
   │   └── ternary_types.h
   ├── src/
   │   ├── asm/
   │   ├── vm/
   │   ├── ir/
   │   └── ast/          # <-- you will create this
   ├── tests/
   ├── examples/
   ├── docs/
   ├── fpga/             # future
   └── CMakeLists.txt
   ```
   Commit the structure + any existing code you find locally or reconstruct minimal stubs from context if needed. Push to main.

4. **Deep read the four key documents** (assume they are in docs/ or ask user to provide if not committed):
   - Thoroughly study `CVT_Rounding_Modes.docx`: Understand every architecture's CVT/FCVT/FCTIW family, MXCSR/FPCR/frm/FPSCR, rounding modes (RNE/RTZ/RDN/RUP/RMM), and the **Trit-Zone Rounding (TZR)** proposal for ternary (including the 9-zone model, generalization to N trits, no-midpoint proof, and proposed 2-trit mode field in CVT encoding). Note why TZR is superior (no special midpoint detection hardware).
   - Thoroughly study `Ternary_Logic_Gates.docx`: Master the full taxonomy (Unary, Lattice MIN/MAX/NMIN/NMAX, Arithmetic ADD/MUL/XSUM, Comparison CMP/EQ/TSEL, Łukasiewicz IMPL/CONSENSUS/NMUL). Map every gate to proposed ISA opcodes (NEG, TMIN, TMAX, ADD, MUL, TCMP, TSEL, BRN/BRZ/BRP, etc.). Understand dual-rail encoding and gate cost comparison (why native ternary wins big on TSEL/MUX).
   - Read `os_fundamentals.md` and `xv6.md` end-to-end: Internalize the missing pieces (interrupt controller + vector table + privilege levels, formal calling convention + ABI, memory model, atomic ops, reset/boot, floating-point exception flags). Understand the xv6 minimal vs intermediate vs complete spectrum and why a ternary xv6 port is the right first OS target. Note ternary-unique opportunities (sign of address = privilege, 1-trit permission states in page tables).

5. Audit current implementation status by reading/compiling the existing .h files (especially ternary_ir.h). Run any existing tests or the VM on simple programs. Identify exactly what ternary_ir.h currently supports (Value creation, basic ops like add/mul, lowering) and what it lacks (calling convention, stack frames, structs, full type system).

6. Research confirmation (quick, 30-60 min):
   - Confirm that AMD and Intel have **identical behavior** for CVTSS2SI, CVTTSS2SI, CVTSD2SI etc. (same MXCSR bits 14:13, same embedded rounding in AVX-512 {er} suffixes). Any numerical differences in practice come from compiler math libraries, x87 fallback, or alignment — **not** from the CVT opcodes or rounding hardware themselves.
   - Briefly note how ARM (FCVT* mnemonics encode rounding directly) and RISC-V (per-instruction 3-bit rm field or DYN from frm CSR) differ in control granularity from Intel's global MXCSR. This reinforces why per-instruction or explicit modes (as proposed for ternary CVT) are cleaner.
   - Summarize in STATUS.md: "Cross-architecture CVT research complete and documented in CVT_Rounding_Modes.docx. TZR is the key ternary-native innovation."

7. Create/update `STATUS.md` with:
   - Current completed components (ISA, ASM, VM, partial IR)
   - Immediate next milestone: AST + parser → working compiler for a C subset
   - Open decisions (list from os_fundamentals: interrupt model, calling convention details, memory model, atomics)
   - Build/test commands that work today

**Success Criteria for Prompt 1**: You can explain in your own words (in STATUS.md or a response) the full current state, why TZR matters, how the IR backend works, and what the AST must produce (IR Program objects). You have a clean repo structure and can build/run the existing VM/IR pieces.

---

## Prompt 2: Review IR Backend and Prepare for AST Integration

Now that you are oriented, deeply internalize the IR layer because the AST will feed directly into it.

**Steps**:

1. Read `ternary_ir.h` (and any related files) line-by-line. Understand:
   - How `Program` is built (param, constant, add, mul, load, store, etc.)
   - Type system (T1, T5, T10, T20, T40, T50, Lane types)
   - Value / SSA representation
   - Register allocation and spilling logic (or lack thereof)
   - Lowering to textual assembly (via ternary_asm)
   - Current limitations (no structs, limited calling convention, no prologue/epilogue yet)

2. Write a small test in C++ that uses the IR to generate a non-trivial function (e.g., a dot-product or polynomial evaluation over T50 values) and assembles/runs it in the VM. Verify correctness.

3. Document in `docs/IR_BACKEND.md`:
   - Exact interface the frontend (AST) must target: what methods on `Program` to call to emit a function, a struct access, a call, etc.
   - Proposed extensions needed in IR (you may implement simple ones): `beginFunction()`, `endFunction()`, `call()`, `loadField()`, `storeField()`, stack frame management, callee/caller-save handling based on the suggested convention (r1-r12 callee-saved, r13-r24 caller-saved, r25 link, r26 SP).

4. Decide and document the **initial language**:
   - Target: Strict C89 subset + minimal C99 (structs, pointers, arrays, int/trit types mapped to T* types, control flow, functions, `asm` inline for rare cases).
   - No templates, no exceptions, no RTTI, no operator overloading (or minimal), no stdlib initially.
   - Type mapping: `int`/`long` → appropriate T20/T50, pointers as T* addresses, `bool` as trit predicate or packed.
   - This subset is sufficient to later port a minimal xv6-style kernel.

5. Outline (in `docs/AST_DESIGN.md`) the AST node hierarchy you will build:
   - Expressions: Literal, Variable, BinaryOp (+ - * / % comparisons), Unary, Call, MemberAccess (for structs), ArrayAccess, TernaryConditional (?: or use TSEL)
   - Statements: Block, If, While/For, Return, ExprStmt, VarDecl, Assign
   - Top-level: FunctionDecl, StructDecl, Program
   - Special: InlineAsm node

**Success Criteria**: You have a clear mental model (and docs) of exactly how source code → AST → IR Program → assembly → VM execution will flow. You can articulate why the existing IR is already a strong backend (register alloc + selection done) and what the frontend must supply.

---

## Prompt 3: Design and Implement the Abstract Syntax Tree (AST)

Build the AST layer. This is the next concrete coding task.

**Requirements**:
- Create `include/ast.h` and `src/ast/` (or single `ast.cpp` initially for simplicity).
- Use modern C++: `std::variant` or inheritance + visitor pattern for nodes (visitor pattern is classic and clean for compiler work).
- Each node should have `location` info (for errors) and be visitable.
- Support pretty-printing (for debugging) and a `lowerToIR(Program& prog, SymbolTable& syms)` or similar method that emits into the IR.
- Or separate: AST nodes are pure data; a dedicated `IRLowerer` or `CodeGen` class walks the AST and calls Program methods.

**Implementation Order** (build incrementally, test at each step):

1. **Basic nodes**: Literal (trit/int/float constants mapped to T* types), Identifier, BinaryOp, UnaryOp.
2. **Control flow**: IfStmt, WhileStmt, BlockStmt, ReturnStmt.
3. **Declarations**: VarDecl, FunctionDecl (with params and body), StructDecl (fields with offsets — compute layout here or in type checker).
4. **Expressions**: CallExpr, MemberExpr (struct.field), AssignExpr, TernaryExpr (maps beautifully to TSEL).
5. **Types**: Simple Type enum or class (Trit, T5, T10, ..., Pointer, StructType, Array). Compute sizes/alignments in trits/words.
6. **Symbol Table**: Scope-aware map from name → (type, IR Value or stack offset, is_param, etc.). Handle shadowing.

**Key Ternary-Specific Touches**:
- Map C `int` etc. to best T* width or keep as T50 by default for safety (like LongTriple philosophy).
- Support trit literals: e.g. `T(-1)`, `T(0)`, `T(1)` or syntax extension, or just use -1/0/1 with type inference.
- Leverage TSEL for ternary conditionals where natural.

**Testing**:
- Write unit tests that build AST manually (no parser yet) and lower to IR, then run in VM.
- Example: A function that computes sign or absolute value using the gates, or a small loop.

**Deliverable**: Working AST data structures + lowering skeleton that can handle a simple function end-to-end (parse manually → lower → asm → VM). Update STATUS.md and commit.

**Success Criteria**: A non-trivial program can be represented in AST, lowered through IR, assembled, and executed correctly in the VM. Errors are caught early with source locations.

---

## Prompt 4: Build the Parser and Semantic Analysis (Frontend)

Now add the missing piece: turn source text into AST.

**Parser Strategy** (recommended for speed and control):
- Hand-written recursive descent parser (classic for C-like languages, ~3-5k LOC total for this subset).
- Or use a simple tool if desired, but hand-written is more educational and controllable for ternary extensions.
- One-pass or two-pass: parse → build AST, then separate semantic pass.

**Required Features for Parser**:
- Expressions with precedence ( Pratt or recursive descent with levels).
- Statements and blocks.
- Function definitions and calls.
- Struct definitions and member access ( `.` and `->` ).
- Variable declarations (with optional init).
- Control flow: if/else, while, for (simple), return.
- Basic types and pointers (`int*`, `T50` or custom `trit`/`t5` etc. — decide syntax).
- Inline assembly: `asm("...")` or `__asm__` block that passes through to IR.
- Error reporting with line/column.

**Semantic Analysis (in same or separate pass)**:
- Type checking and inference.
- Struct layout computation (field offsets in words/trits).
- Scope resolution and symbol table building.
- Implicit conversions (if any) and explicit casts.
- Function signature matching for calls.
- Detection of use-before-declare, etc.
- For kernel later: track which functions are `__kernel__` or naked.

**Integration**:
- Parser produces `std::unique_ptr<ProgramNode>` (root AST).
- Then `CodeGen::lower(astRoot, irProgram)` or `astRoot->lower(irProgram, symtab)`.
- The IR Program is then lowered to asm text → binary → load into VM.

**Testing Strategy**:
- Parser tests: feed source snippets, check AST structure or error messages.
- End-to-end: Write `.t3` or `.tc` source files in `examples/`, compile via your new `ternarycc` driver, run in VM, assert output.
- Example programs: factorial, gcd, vector dot product using T* types, simple struct Point { t5 x, y; }, sign/abs using TCMP.

**Deliverable**: A working `ternarycc` (or `compile` function) that takes source → AST → IR → asm → executable binary. At least 5-10 passing end-to-end tests. Document the language syntax in `docs/LANGUAGE.md` (grammar + examples).

**Success Criteria**: You (or a test script) can write C-like source, compile it, and have it run correctly on the VM. The compiler is now functional for simple programs.

---

## Prompt 5: Complete the Compiler, Add Features, Testing, and Documentation

Polish the compiler into a usable tool and prepare for larger software.

**Tasks**:

1. **Full Compiler Driver**:
   - Command-line tool: `ternarycc input.tc -o output.bin` (or .o for object, support multiple files later).
   - Support `-S` (emit asm), `-emit-ir`, verbose modes.
   - Integrate assembler and VM for "compile and run" mode or tests.

2. **Missing Language Features** (prioritize):
   - Pointers and address-of / dereference (maps to LOAD/STORE with offsets).
   - Arrays (fixed size or pointer + length).
   - More control flow (do-while, switch if simple, break/continue).
   - Function pointers or at least indirect calls if needed later.
   - Basic I/O or syscalls hooks (for OS later: trap to kernel via special instruction or r27).
   - Enums or constants.

3. **IR Enhancements** (implement as needed while building):
   - Proper function prologue/epilogue and stack frame (allocate locals on SP).
   - Full calling convention: argument passing (first N in regs, rest on stack), return value, save/restore live caller-saved regs around calls.
   - Struct passing/returning (by pointer or decompose — decide and document).
   - Better spilling and register pressure handling.

4. **Testing & Validation**:
   - Expand test suite: property-based or golden tests comparing ternary results vs. high-precision reference (use LongTriple ideas or mpfr if helpful).
   - Test rounding behavior using the TZR mode in CVT.
   - Fuzz simple programs or use the physics audit style once compiler is solid.
   - Ensure dual-rail invariants (no INVALID trit) are maintained in VM.

5. **Documentation**:
   - Update README.md with build instructions, example compilation + run.
   - `docs/COMPILER.md`: Architecture (frontend AST → IR backend → asm), limitations, how to add a new opcode or intrinsic.
   - `docs/LANGUAGE_SPEC.md`: Full grammar, type system, mapping to ternary types/gates.
   - Keep STATUS.md current.

6. **Polish**:
   - Error messages with source snippets.
   - Location tracking throughout.
   - Memory safety in compiler (unique_ptr everywhere).
   - Performance: the compiler itself should be fast (it's not compiling huge code yet).

**Success Criteria**: A complete, documented, tested C-subset compiler that produces correct ternary binaries from source. You can write and run non-trivial programs (loops, structs, functions calling each other). The project is now at "functional compiler and language" milestone.

---

## Prompt 6: Next Milestones — OS, FPGA, and Beyond (Future Handoffs)

After the compiler works, the project naturally progresses. These prompts are for subsequent agents or continuation.

**Immediate Follow-ups**:
- Formalize **Calling Convention + ABI** (document in new `docs/ABI.md`): exact register roles, stack alignment (trit/word), varargs, struct passing, syscall convention (how userspace calls kernel via trap).
- Design **Interrupt/Exception Architecture** (critical before FPGA): vector table, asynchronous interrupt sources (timer, UART, etc.), privilege levels (user/supervisor/machine analog — perhaps using sign of address or dedicated trit), exception return (ERET-like), how r27 integrates. Add to ISA if needed (new opcodes or CSRs analog = TPCR).
- Implement **Atomic Operations**: Design ternary LR/SC or CAS that returns a trit result (success / collision / value-mismatch). Essential for locks in multi-threaded OS.
- **Memory Model**: Decide and document (weak like ARM/RISC-V with explicit barriers, or stronger). Add fence instructions if needed.

**OS Milestone (xv6-style ternary port)**:
- Port or rewrite minimal xv6: process struct, round-robin scheduler (use ternary CMP for decisions), simple page tables (exploit 1-trit permissions), trap/syscall handling, basic FS (log-structured), UART/console driver, shell.
- Use the new compiler to build the kernel and userspace.
- Boot in VM first, then on FPGA.
- Exploit ternary: negative addresses = kernel, TSEL for three-way branches in scheduler, compact page table entries.

**Hardware Milestone**:
- FPGA implementation (Verilog/VHDL or Chisel/SpinalHDL) of the ternary core using dual-rail encoding.
- Synthesize, test on board (timer interrupts, etc.).
- Validate against VM bit-for-bit.

**Advanced**:
- Demand paging, network stack, multi-core (with memory model), self-hosting (compile the compiler on itself).
- Package manager, dynamic linking, full POSIX subset.
- Publish papers on TZR advantages, ternary OS properties, gate efficiency.

**General Guidance for All Future Work**:
- Always keep the VM as golden reference.
- Update the two foundational .docx (or convert to .md) when ISA or rounding/gates change.
- Measure everything: code density, gate count vs binary, rounding error accumulation (TZR should win), performance on physics kernels.
- Stay true to ternary-native design — don't just emulate binary.

---

**Final Notes**:
These prompts are designed to be copy-pasted sequentially into a new agent session. Each builds on the previous and leaves zero ambiguity about what "done" looks like. After Prompt 5, the project has a working language + compiler — the original request's immediate goal.

When you complete a prompt, update `STATUS.md`, commit with clear message, and prepare a short summary of what was achieved + any blockers for the next prompt.

The vision is sound, the research foundation (especially TZR and gates) is excellent, and the IR backend gives you a massive head start. Build the AST/frontend and the rest will follow naturally.

Good luck — this is going to be a landmark project. Ternary computing has been waiting for exactly this level of complete toolchain + OS demonstration.

— Handoff prepared by Grok (xAI), 2026-05-15
