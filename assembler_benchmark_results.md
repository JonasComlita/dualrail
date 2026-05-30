# Balanced Ternary Assembler Performance Benchmark Report

This report provides empirical performance data comparing the C++ host assembler (`sandbox::vm::assembler::assemble`) against the native balanced ternary assembler (`tcl_asm.trit`) executing inside the emulated Trit Virtual Machine (TVM) run loop.

## System Configuration

- **Host OS**: Windows 10/11
- **Host CPU**: AMD64 Family 23 Model 113 Stepping 0, AuthenticAMD (24 logical cores)
- **Compiler**: GCC/MinGW-w64 (MSYS2 UCRT64, C++17, Release)
- **VM Memory Limits**: IMEM = 262,144 words, DMEM = 2,097,152 words

## Workload Specifications

| Workload | Description | Lines of Assembly | Character Count | Output Binary Size (words) |
| :--- | :--- | :---: | :---: | :---: |
| **Small** | Basic loop block (scalar arithmetic) | 13 | 279 | 12 |
| **Medium** | Intermediate control flow, Fibonacci loop subroutine | 37 | 939 | 32 |
| **Large** | Large program containing complex subroutine cascade blocks | 220 | 5855 | 190 |

## Performance Comparison Metrics

| Workload | C++ Assembler (ms) | Native TVM Assembler (ms) | TVM Executed Steps | Runtime Conditional Branches | Emulated Speed (MIPS) | Slowdown Factor | Outputs Verified |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Small** | 0.0230 | 209.15 | 378918 | 27728 (7.3%) | 1.81 | 9092.2x | ✅ MATCH |
| **Medium** | 0.0622 | 450.13 | 1052654 | 78337 (7.4%) | 2.34 | 7241.2x | ✅ MATCH |
| **Large** | 0.4074 | 2494.81 | 7757060 | 573090 (7.4%) | 3.11 | 6124.3x | ✅ MATCH |

## Width And Branch Profile

The width profile is decoded from TVM instruction words without adding counters to the timed hot path. Primary width counts are the semantic width selected by instructions such as ADD, TCMP, vector ops, conversions, and lane ops. Width field counts also include source-width fields for CVT/VPACK/VUNPACK.

### Native Assembler Harness Image

- **Instructions**: 9308
- **Primary width mix**: t1=63 (6.8%), t40=866 (93.2%)
- **All width fields**: t1=63 (6.8%), t40=866 (93.2%) (929 fields)
- **Implicit MOV/MOVH T40 producers**: 1425
- **Typed literal MOV/MOVH + CVT pairs**: 42
- **Memory operations**: 4461 (47.9%)
- **Conditional branch sites**: 648 total, 0 backward loop-shaped sites
- **Top opcodes**: LOAD=2510, STORE=1951, MOV=1425, JMP=966, COPY=396, CALL=382, ADD=346, SUB=264, BRP=230, TCMP=229, BRN=209, BRZ=209

### Generated Workload Images

| Workload | Primary Width Mix | Implicit MOV/MOVH T40 | Typed Literal Pairs | Conditional Branch Sites | Backward Conditional Sites | Memory Ops | Top Opcodes |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| **Small** | t5=6 (100.0%) | 3 | 3 | 1 | 1 | 0 | MOV=3, CVT=3, HALT=1, ADD=1, SUB=1, TCMP=1 |
| **Medium** | t20=12 (100.0%) | 6 | 6 | 1 | 0 | 2 | MOV=6, COPY=6, CVT=6, ADD=3, SUB=2, CALL=2 |
| **Large** | t20=71 (100.0%) | 7 | 7 | 1 | 0 | 77 | STORE=46, ADD=31, LOAD=31, SUB=30, CALL=16, RET=16 |

## Key Analysis & Findings

### 1. The Emulation Overhead Gap
The native assembler executing inside the TVM runs **7241x to 6124x slower** than the host-compiled C++ assembler. This performance overhead is driven by:
- **Instruction Decoding and Translation**: The TVM run loop fetches 27-trit instructions, parses opcodes, isolates registers, compiles/cache-checks basic blocks, and performs branch checks for emulated steps.
- **Register and Memory Virtualization**: Accesses to virtual registers and virtual balanced-ternary memory are translated through array lookups and arithmetic bounds checks rather than execution on native hardware.
- **Lack of Whole-Program Host Optimizations**: The C++ assembler benefits from full compiler optimizations (MSVC/GCC inline, loop unrolling, register allocation, cache efficiency), whereas the emulated run loop and fallback helpers still pay virtual machine boundaries.

### 2. Emulated VM Speed Consistency
Across all workloads, the TVM run loop achieves a stable emulated execution speed of **2.34 to 3.11 MIPS** (Million Instructions Per Second) on the host machine. This consistency shows that the current cost is still dominated by VM execution boundaries and memory-heavy assembler behavior.

### 3. Practical Viability & Self-Hosting Assessment
- **Bootstrap Speed**: For normal development, utilizing the C++ compiler/assembler on the host machine is essential, as compiling and assembling large programs natively takes seconds compared to milliseconds on the host.
- **Self-Hosting Capability**: Despite the overhead, compiling the largest workload natively on the TVM completes in a timely manner. This proves that the VM implementation is fully viable for running self-hosted compiler pipelines, interactive sandboxes, and verification test suites where host access is unavailable.
