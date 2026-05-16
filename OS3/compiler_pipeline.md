## Compiler pipeline (low-level → high-level)

1. **Executable / Deployment** — final runnable program on the target platform (conforms to ABI).  
2. **Runtime / Standard library / Garbage collector** — startup, memory management, and platform services used by the program.  
3. **ABI / Object format (ELF/PE/Mach-O) & Calling conventions** — binary-level contract that generated code, linker, OS, and runtimes follow.  
4. **Linker** — combines object files and libraries into executables or shared libraries; resolves symbols.  
5. **Object files / Symbol metadata / DWARF** — machine code chunks plus debug/symbol information.  
6. **Assembler** — converts assembly into object files.  
7. **Assembly** — human-readable target instruction text emitted by codegen.  
8. **Compiler (code generator)** — lowers final IR to target-specific instructions; orchestrates backend tasks.  
9. **Optimizer / Analyses** — transforms and optimizes IR(s).  
10. **Multiple IRs (MIR/SSA/etc.)** — lowering passes produce one or more intermediate representations for analysis and optimization.  
11. **Parser → AST** — builds the Abstract Syntax Tree representing program structure.  
12. **Lexer / Tokenizer** — turns source text into tokens.  
13. **High-level language** — source code (e.g., Python, C, Rust).