export interface Topic {
  id: string;
  title: string;
  content: string;
}

export interface Module {
  id: string;
  title: string;
  topics: Topic[];
}

export const MODULES: Module[] = [
  {
    id: "m1",
    title: "1. Binary-Backed Ternary",
    topics: [
      {
        id: "t1",
        title: "What Trit Emulates",
        content: [
          "### What Trit Emulates",
          "Trit is not pretending that a binary CPU has physical three-state gates. The implementation stores ternary values in ordinary binary memory, then keeps the ternary meaning explicit at every boundary.",
          "",
          "- A trit has the balanced value `-1`, `0`, or `+1`.",
          "- Numeric arithmetic uses ternary pack/unpack, native ternary integer and floating formats, and explicit overflow sentinels.",
          "- Lane, SIMD, GPU, and instruction transport use a separate two-bit-per-trit wire format.",
          "- The VM, assembler, compiler, and tests treat conversion between those families as a real contract, not as a display trick.",
          "",
          "The core source files for this layer are `ternary_scalar.h`, `ternary_native_ops.h`, `ternary_lanes.h`, `ternary_isa.h`, and `ternary_vm_state.h`.",
          "",
          "### Why Use Binary Storage At All?",
          "A binary host is the cheapest way to validate the ternary machine before hardware exists. The goal is exact emulation of ternary semantics: deterministic signed trits, ternary-width values, ternary instruction words, and ternary traps. Binary integers are only containers.",
          "",
          "### The Main Rule",
          "Never do normal binary integer arithmetic on a lane payload and call it ternary arithmetic. Numeric values and lane values are different representations. Cross with `toLane(...)`, `fromLane(...)`, `laneFromPositional(...)`, or `laneToPositional(...)`."
        ].join("\n"),
      },
      {
        id: "t2",
        title: "The Three Encodings",
        content: [
          "### The Three Encodings",
          "This project deliberately has more than one ternary encoding because each surface needs a different property.",
          "",
          "| Scheme | Used by | Binary shape | Trit meaning |",
          "|---|---|---|---|",
          "| Positional base-3 | Arithmetic, scalar values | One unsigned integer in `[0, 3^N)` | Raw digit `d` decodes as `d - 1` |",
          "| 2-bit lane | SIMD, GPU, vectors, instruction words | Two bits per trit | `00=-1`, `01=0`, `10=+1`, `11=invalid` |",
          "| Trit enum | HAL interface helpers | Small enum | Neutral, Positive, Negative, Invalid |",
          "",
          "### Positional Base-3",
          "This is the canonical numeric representation. `TernaryScalar<N>` packs N trits into the smallest unsigned storage type that can hold `3^N` states.",
          "",
          "```cpp",
          "stored = sum((trit[i] + 1) * 3^i)",
          "trit[i] = (stored / 3^i) % 3 - 1",
          "```",
          "",
          "That is why `T40` fits in a `uint64_t`: `3^40` is smaller than `2^64`. `T50` needs the project-local `UInt128` type.",
          "",
          "### Two-Bit Lane Encoding",
          "Lane values are for transport and parallel tritwise operations. Each trit gets two bits, so invalid bit pattern `11` can be detected and trapped. A 27-trit instruction word therefore fits in 54 bits of a `uint64_t`.",
          "",
          "### HAL Enum",
          "The HAL enum is an interface type. It is useful when hardware-facing code wants named trit states, but it is not the arithmetic storage format. Treat it as an API boundary."
        ].join("\n"),
      },
      {
        id: "t3",
        title: "Widths and Families",
        content: [
          "### Numeric Widths",
          "The implementation exposes numeric widths and matching lane widths. TCL uses the lowercase spellings shown here.",
          "",
          "| TCL type | IR family | Trits | Backing storage | Current role |",
          "|---|---|---:|---|---|",
          "| `t1` / `trit` | Numeric predicate | 1 | `uint8_t` | Three-way conditions |",
          "| `t5` | Small integer | 5 | `uint8_t` | Trap records and compact values |",
          "| `t10` | Ternary float | 10 | `uint16_t` | Short numeric format |",
          "| `t20` | Ternary float | 20 | `uint32_t` | Medium numeric format |",
          "| `t40` | Ternary float | 40 | `uint64_t` | Native scalar word |",
          "| `t50` | Ternary float | 50 | `UInt128` | Extended value / LongTriple |",
          "",
          "Lane types are spelled `l1`, `l5`, `l10`, `l20`, `l40`, and `l50`. They carry the same number of trits, but they use the two-bit transport encoding.",
          "",
          "### Integer vs Floating Formats",
          "`t1` and `t5` are small integer formats. `t10`, `t20`, `t40`, and `t50` use the native ternary floating format implemented in `ternary_native_ops.h`, with explicit mantissa and exponent trits.",
          "",
          "### Invalid Values",
          "Numeric formats reserve top-of-storage sentinel values for overflow and underflow. Lane formats reserve pair `0b11` as invalid. The VM converts invalid data into traps or sentinel values instead of silently accepting malformed ternary state."
        ].join("\n"),
      },
    ],
  },
  {
    id: "m2",
    title: "2. VM and ISA",
    topics: [
      {
        id: "t4",
        title: "VM State Model",
        content: [
          "### VM State Model",
          "The VM is a ternary machine model implemented in C++. `VMState` is data; `ternary_vm.h` owns execution. The important pieces are stable and regression-tested.",
          "",
          "- `pc`: word index into instruction memory.",
          "- `regfile`: 27 general-purpose registers, `r0` through `r26`.",
          "- `trap_reg`: separate `r27` fault record written by the VM.",
          "- `imem`: instruction memory containing 27-trit `TritWord27` words.",
          "- `dmem`: word-addressed `TernaryValue` memory with dense or sparse backing.",
          "- `csrs`: 47 control/status registers for traps, MMU, console, graphics, block devices, and syscalls.",
          "- `vregfile`: eight vector registers with a runtime vector length.",
          "",
          "### Harvard Split",
          "Instruction memory and data memory are separate. A data `STORE` cannot rewrite instruction memory. This makes the emulator closer to the intended architecture and keeps loader bugs easier to catch.",
          "",
          "### Word Addressing",
          "The VM does not expose byte loads and stores. DMEM addresses are word indices. If a program wants byte-like data, it stores character codes or packed sub-word data inside ternary words."
        ].join("\n"),
      },
      {
        id: "t5",
        title: "Instruction Words",
        content: [
          "### Instruction Words",
          "Every instruction is a 27-trit `TritWord27`. The host stores it as 54 meaningful bits plus padding inside a `uint64_t`.",
          "",
          "| Bit pair | Trit | Decode behavior |",
          "|---|---|---|",
          "| `00` | `-1` | valid |",
          "| `01` | `0` | valid |",
          "| `10` | `+1` | valid |",
          "| `11` | invalid | `TRAP_ILLEGAL_OP` |",
          "",
          "### Formats",
          "Trit 26 is the format discriminant.",
          "",
          "| `trit[26]` | Format | Main use |",
          "|---|---|---|",
          "| `+1` | R-type | Register operations, CSR, vector and extended forms |",
          "| `0` | I-type | Immediate moves, loads, stores, vector memory |",
          "| `-1` | B-type | Branches, jumps, calls |",
          "",
          "The opcode field is four trits wide and is decoded as an unsigned base-3 integer. Register fields are three trits wide, storing `register_index - 13`, which covers `r0` through `r26`.",
          "",
          "### Immediates",
          "Immediate fields are signed balanced-ternary values. `imm16` covers about plus/minus 21 million; branch `offset19` covers about plus/minus 581 million instruction words."
        ].join("\n"),
      },
      {
        id: "t6",
        title: "Registers and ABI",
        content: [
          "### Register Roles",
          "The VM has 27 general-purpose registers and one separate trap register.",
          "",
          "| Register | Role |",
          "|---|---|",
          "| `r0` | hardwired zero; writes are discarded |",
          "| `r1`-`r12` | callee-saved registers |",
          "| `r13` | argument 0 and return value |",
          "| `r14`-`r18` | additional register arguments in current compiler calls |",
          "| `r19`-`r24` | caller-saved scratch registers |",
          "| `r25` | link register written by `CALL` and read by `RET` |",
          "| `r26` | stack pointer, grows downward |",
          "| `r27` | VM trap register, not a normal arithmetic register |",
          "",
          "### Function Calls",
          "The current compiler codegen stages up to six normal function arguments in `r13` through `r18`; additional arguments are spilled to the stack. Return values come back in `r13`.",
          "",
          "### Syscalls",
          "Syscalls are a kernel ABI, not the same thing as normal function calls. The manifest says the service ID goes in CSR `syscall_id`, primary syscall args are in `r13` through `r16`, and returns are `r13` status, `r14` payload, and `r15` detail.",
          "",
          "Use `SYSCALL_MANIFEST.json` as the source of truth when adding or calling kernel services."
        ].join("\n"),
      },
      {
        id: "t7",
        title: "Three-Way Control",
        content: [
          "### Three-Way Control",
          "`t1` is not a binary boolean. It carries `-1`, `0`, or `+1`, and the ISA has direct support for that shape.",
          "",
          "| Instruction | Meaning |",
          "|---|---|",
          "| `TCMP` | produce sign of `a - b` as `-1`, `0`, or `+1` |",
          "| `BRN` | branch when a predicate is `-1` |",
          "| `BRZ` | branch when a predicate is `0` |",
          "| `BRP` | branch when a predicate is `+1` |",
          "| `TSEL` | choose negative, zero, or positive value without a branch |",
          "",
          "### Branch Pattern",
          "```asm",
          "tcmp r3, r1, r2",
          "brn  r3, less",
          "brz  r3, equal",
          "; fall through: greater",
          "```",
          "",
          "### Select Pattern",
          "```asm",
          "tsel r10, r3, r_neg, r_zero, r_pos",
          "```",
          "",
          "`TSEL` is tag-preserving. It copies the selected register value; it does not coerce widths, inspect payloads, or perform arithmetic."
        ].join("\n"),
      },
      {
        id: "t8",
        title: "Vectors and Lanes",
        content: [
          "### Vectors and Lanes",
          "The vector path keeps the same numeric-vs-lane split as the scalar path.",
          "",
          "- Numeric vector ops such as `vadd.t40` run ternary numeric arithmetic per lane.",
          "- Lane ops such as `tladd.l20`, `tlsub.l20`, `tland.l20`, and `tlor.l20` operate on two-bit packed trits.",
          "- `vcmp` produces lane predicates.",
          "- `vsel` is the vector form of three-way select.",
          "- `vdot.t1`, `vmac.t1`, and `vact.t1` are the T1 AI/accumulator path.",
          "",
          "### Acceleration Status",
          "The reference VM implements the vector and lane semantics. The host backend has scalar fallbacks and AVX2 dispatch for selected `TritLane20` operations. CUDA and SYCL wrappers exist behind build flags, with hardware validation tracked separately."
        ].join("\n"),
      },
    ],
  },
  {
    id: "m3",
    title: "3. TCL Language",
    topics: [
      {
        id: "t9",
        title: "Current Source Shape",
        content: [
          "### Current Source Shape",
          "TCL is the `.trit` language used by the kernel, apps, libraries, and native compiler components. The host compiler implementation lives in `tritc.cpp` and `ternary_compiler_*.h`; native TCL compiler pieces live in `tcl_*.trit`.",
          "",
          "The current parser accepts top-level `import`, `struct`, `const`, and `fn` declarations.",
          "",
          "```tcl",
          "import ulib;",
          "",
          "struct Point {",
          "    x: t40;",
          "    y: t40;",
          "}",
          "",
          "const LIMIT: t40 = 32;",
          "",
          "fn add(a: t40, b: t40) -> t40 {",
          "    return a + b;",
          "}",
          "```",
          "",
          "### Lexical Notes",
          "The current host lexer handles ASCII identifiers, decimal integer literals, line comments with `//`, and common punctuation. Do not rely on planned literal syntaxes unless a focused compiler test covers them."
        ].join("\n"),
      },
      {
        id: "t10",
        title: "Types That Compile Today",
        content: [
          "### Types That Compile Today",
          "Use lowercase type names in TCL source.",
          "",
          "| Type | Meaning |",
          "|---|---|",
          "| `t1` / `trit` | one-trit predicate or small value |",
          "| `t5`, `t10`, `t20`, `t40`, `t50` | numeric widths |",
          "| `l1`, `l5`, `l10`, `l20`, `l40`, `l50` | lane widths |",
          "| `ptr<T, region, state>` | pointer with optional region and state |",
          "| `own<T>` | owned value tracked by move analysis |",
          "| `borrow<T>` | immutable borrow wrapper |",
          "| `borrow_mut<T>` | mutable borrow wrapper |",
          "| `shared<T, ORDER>` | shared value with memory-order requirement |",
          "| `[T; N]` | fixed-size array |",
          "| `vec<T>` | vector value in the compiler type model |",
          "",
          "Regions are `stack`, `static`, `user`, and `kernel`. Pointer states are `valid`, `unknown`, and `null`. Memory orders are `RELAXED`, `ACQ_REL`, and `SEQ_CST`.",
          "",
          "### Important Correction",
          "`trit` is currently parsed as the one-trit type, equivalent to `t1`. It is not a synonym for `t40` in the current compiler."
        ].join("\n"),
      },
      {
        id: "t11",
        title: "Bindings and Control Flow",
        content: [
          "### Bindings",
          "`let` creates an immutable binding. `var` creates a mutable binding.",
          "",
          "```tcl",
          "fn count_to(n: t40) -> t40 {",
          "    var i: t40 = 0;",
          "    var sum: t40 = 0;",
          "    while n - i >= 0 {",
          "        sum = sum + i;",
          "        i = i + 1;",
          "    }",
          "    return sum;",
          "}",
          "```",
          "",
          "### If and While",
          "`if` and `while` conditions must evaluate to `t1`. The compiler lowers them as positive-only control flow: `+1` enters the then/body path; `0` and `-1` go to else or exit.",
          "",
          "```tcl",
          "fn abs_word(x: t40) -> t40 {",
          "    if x < 0 {",
          "        return -x;",
          "    }",
          "    return x;",
          "}",
          "```",
          "",
          "Comparisons return `t1`, so they are the natural condition expressions."
        ].join("\n"),
      },
      {
        id: "t12",
        title: "Match and TSEL",
        content: [
          "### Match",
          "`match` is the direct language shape for ternary branching. Numeric matches must cover `neg`, `zero`, and `pos` arms, or use `_` as a wildcard. Pointer matches must cover `null`, `unknown`, and `valid`.",
          "",
          "```tcl",
          "fn sign_code(x: t40) -> t40 {",
          "    match x {",
          "        neg => { return -1; }",
          "        zero => { return 0; }",
          "        pos => { return 1; }",
          "    }",
          "}",
          "```",
          "",
          "When the match subject is a numeric type wider than `t1`, the compiler compares it with zero first. If the subject is already `t1`, it branches directly.",
          "",
          "### TSEL Optimization",
          "Simple matches whose arms only return or assign pure scalar expressions can lower to `tsel` instead of three branch blocks.",
          "",
          "```tcl",
          "fn select_word(cond: t1, n: t40, z: t40, p: t40) -> t40 {",
          "    match cond {",
          "        neg => { return n; }",
          "        zero => { return z; }",
          "        pos => { return p; }",
          "    }",
          "}",
          "```"
        ].join("\n"),
      },
      {
        id: "t13",
        title: "Pointers and Ownership",
        content: [
          "### Pointer States",
          "Pointer state is part of the type. The compiler refuses to dereference a pointer unless its state is proven `valid`.",
          "",
          "```tcl",
          "fn read_or_zero(p: ptr<t40, user, unknown>) -> t40 {",
          "    match p {",
          "        null => { return 0; }",
          "        unknown => { return 0; }",
          "        valid(q) => { return *q; }",
          "    }",
          "}",
          "```",
          "",
          "The `valid(q)` arm introduces `q` as a valid pointer binding. That binding does not exist in the other arms.",
          "",
          "### Ownership Tracking",
          "`own<T>` values are move-tracked by type inference. Reusing a moved owned value is diagnosed as a compiler error. `borrow<T>` and `borrow_mut<T>` are represented in the type model so compiler passes can distinguish borrowed access from ownership transfer.",
          "",
          "This is implementation work, not just a planned syntax: the host compiler has type and move checks for these wrappers."
        ].join("\n"),
      },
      {
        id: "t14",
        title: "Unsafe and Atomics",
        content: [
          "### Unsafe Blocks",
          "Raw hardware-like operations require `unsafe`. The compiler still type-checks inside the block; `unsafe` only marks the operations whose safety must be reasoned about manually.",
          "",
          "Available unsafe intrinsics include:",
          "",
          "- `load(addr)` and `store(addr, value)` for raw word memory.",
          "- `csr_read(id)` and `csr_write(id, value)` for CSR access.",
          "- `tldr(addr, order)` and `tstr(addr, desired, expected, order)` for reserved load and conditional store.",
          "- `fence(order)` for memory ordering.",
          "",
          "```tcl",
          "fn write_console_char(c: t40) -> t40 {",
          "    unsafe {",
          "        csr_write(22, c);",
          "    }",
          "    return 0;",
          "}",
          "```",
          "",
          "### Shared Values",
          "`shared<T, ORDER>` values can be accessed with `atomic_load` and `atomic_store`. The compiler checks that the requested order is at least as strong as the declared order."
        ].join("\n"),
      },
      {
        id: "t15",
        title: "Width Parameters",
        content: [
          "### Width Parameters",
          "The host compiler parses generic functions with width parameters constrained by `TritWidth`. Calls are monomorphized to concrete numeric widths.",
          "",
          "```tcl",
          "fn choose<W: TritWidth>(cond: t1, n: T<W>, z: T<W>, p: T<W>) -> T<W> {",
          "    match cond {",
          "        neg => { return n; }",
          "        zero => { return z; }",
          "        pos => { return p; }",
          "    }",
          "}",
          "```",
          "",
          "Width parameters are not runtime dynamic dispatch. The compiler specializes a copy for each concrete width it can infer from call sites."
        ].join("\n"),
      },
    ],
  },
  {
    id: "m4",
    title: "4. Compiler Pipeline",
    topics: [
      {
        id: "t16",
        title: "Frontend to AST",
        content: [
          "### Frontend to AST",
          "The host compiler pipeline is split across `ternary_compiler_lexer.h`, `ternary_compiler_parser.h`, `ternary_compiler_ast.h`, `ternary_compiler_types.h`, and `ternary_compiler_codegen.h`.",
          "",
          "```text",
          ".trit source",
          "  -> lexer tokens",
          "  -> recursive-descent parser",
          "  -> ModuleAst",
          "  -> type inference and checks",
          "  -> structural IR",
          "  -> optimization",
          "  -> register allocation",
          "  -> TASM text",
          "  -> assembler/linker image",
          "```",
          "",
          "The native TCL compiler components in `tcl_lexer.trit`, `tcl_parser.trit`, `tcl_ir.trit`, `tcl_backend.trit`, and `tcl_asm.trit` mirror this architecture for the in-OS compiler path.",
          "",
          "### Constant Folding",
          "The AST layer evaluates simple constant expressions and folds them before lowering. This is why array lengths and module constants must be compile-time constant expressions."
        ].join("\n"),
      },
      {
        id: "t17",
        title: "Type Inference and Checks",
        content: [
          "### Type Inference and Checks",
          "The current type model includes numeric widths, lanes, vectors, pointers, shared values, structs, arrays, functions, `own`, `borrow`, and `borrow_mut`.",
          "",
          "Implemented checks include:",
          "",
          "- Function call argument count and type matching.",
          "- Return type matching.",
          "- No implicit narrowing when assigning or returning wider values into narrower annotations.",
          "- `if` and `while` conditions must be `t1`.",
          "- Numeric matches must cover `neg`, `zero`, and `pos`.",
          "- Pointer matches must cover `null`, `unknown`, and `valid`.",
          "- Dereference requires a proven valid pointer.",
          "- Moved `own<T>` values cannot be used again.",
          "- Raw CSR, load/store, atomic, and fence intrinsics require `unsafe`.",
          "",
          "The compiler is intentionally stricter than old examples that treated any word as a loosely typed integer."
        ].join("\n"),
      },
      {
        id: "t18",
        title: "Lowering and Allocation",
        content: [
          "### Lowering",
          "Codegen emits textual TASM and structural IR metadata together. This keeps the assembler-facing output simple while giving tests a way to inspect compiler behavior.",
          "",
          "Important lowering choices:",
          "",
          "- Numeric comparisons lower through `tcmp` plus `tsel` or branches.",
          "- `if` branches on negative or zero to the else path, leaving positive as fallthrough.",
          "- `while` exits on negative or zero and loops on positive.",
          "- Pointer match maps `null`, `unknown`, and `valid` onto the same three-way branch shape.",
          "- Tuple swaps can lower to `swap` when both values are local and compatible.",
          "",
          "### Register Allocation",
          "The allocator colors scalar values into `r1` through `r24`, preferring callee-saved registers for values live across calls. If register pressure is too high and spilling is enabled, values receive stack spill slots.",
          "",
          "The function prologue allocates a static frame, saves `lr`, saves callee-saved registers that were used, and stores register arguments into local slots."
        ].join("\n"),
      },
      {
        id: "t19",
        title: "Assembler and Images",
        content: [
          "### Assembler",
          "`tcl_asm.trit` and `ternary_asm.h` parse TASM, resolve labels, encode instructions as `TritWord27`, and produce executable instruction arrays.",
          "",
          "Assembler details that matter to users:",
          "",
          "- Width suffixes such as `.t1`, `.t20`, `.t40`, `.t50`, `.l1`, and `.l20` select numeric or lane mode.",
          "- Branch labels resolve to signed 19-trit PC-relative offsets.",
          "- Loads and stores use word offsets, not byte offsets.",
          "- `cvt.src.dst` is the explicit numeric/lane conversion spelling.",
          "",
          "### Images",
          "Linked programs are packaged with an executable header and then bundled into `.tboot` or `.tdisk` images by the OS image builder. Use `IMAGE_FORMAT_MANIFEST.json` as the format contract."
        ].join("\n"),
      },
    ],
  },
  {
    id: "m5",
    title: "5. OS and Runtime",
    topics: [
      {
        id: "t20",
        title: "Kernel Surface",
        content: [
          "### Kernel Surface",
          "The kernel is written in TCL in `kernel.trit`, with supporting layer files under `kernel/`. It dispatches syscalls, manages process state, serves the VFS, and connects apps to the host runner.",
          "",
          "Current status from `ROADMAP_STATUS.json`:",
          "",
          "- Core VM and ISA are verified.",
          "- Compiler/runtime are verified.",
          "- Kernel, VFS, and process lifecycle are in progress but production-tested.",
          "- Desktop host runtime is in progress and covered by smoke/release tests.",
          "",
          "### Syscall Contract",
          "`SYSCALL_MANIFEST.json` lists the service IDs, groups, and ABI sources. When adding a syscall, update the kernel dispatch, compiler runtime IDs, codegen wrapper mapping, app SDK, manifest, and focused tests."
        ].join("\n"),
      },
      {
        id: "t21",
        title: "Apps and SDK",
        content: [
          "### Apps and SDK",
          "Bundled user apps live under `apps/`. They compile against `apps/os_sdk.trit`, `apps/libwidget.trit`, and shared user libraries such as `ulib.trit`.",
          "",
          "Important app files include:",
          "",
          "- `apps/shell.trit`: command shell.",
          "- `apps/desktop.trit`: desktop surface.",
          "- `apps/tcc.trit`: in-OS compiler client.",
          "- `apps/file_manager.trit`, `apps/text_editor.trit`, `apps/settings.trit`, and `apps/task_manager.trit`: GUI/user utilities.",
          "- `APP_MANIFEST.json`: bundled app paths, stack hints, and guest metadata.",
          "",
          "When adding an app, update `build_tos_image.cpp`, `APP_MANIFEST.json`, and focused app/process tests. Then rebuild and inspect the release image."
        ].join("\n"),
      },
      {
        id: "t22",
        title: "Tools and Knowledge",
        content: [
          "### Tools and Knowledge",
          "The repo now has an agent-operable tool layer. Prefer the stable entry point over ad-hoc scripts.",
          "",
          "```powershell",
          "python tools/trit_tool.py doctor",
          "python tools/trit_tool.py test smoke",
          "python tools/trit_tool.py knowledge status",
          "python tools/trit_tool.py knowledge graph --no-archive",
          "python tools/trit_tool.py export-diagnostics",
          "```",
          "",
          "### Obsidian Vault",
          "`docs/` is an Obsidian-friendly vault. It contains quick references, architecture notes, ABI docs, execution engine docs, compiler notes, OS docs, and the `trit-stack.canvas` architecture map.",
          "",
          "### Graphify",
          "Graphify output is advisory. The source files, manifests, tests, and implementation headers remain authoritative. Use the graph to navigate symbols and call relationships, then verify against source."
        ].join("\n"),
      },
    ],
  },
  {
    id: "m6",
    title: "6. Practical Workflow",
    topics: [
      {
        id: "t23",
        title: "Reading the Source",
        content: [
          "### Reading the Source",
          "New users usually get oriented fastest by reading in this order:",
          "",
          "1. `docs/00_Quick_Ref/trit_encoding.md` for representation boundaries.",
          "2. `ternary_scalar.h` and `ternary_native_ops.h` for numeric semantics.",
          "3. `ternary_lanes.h` and `ternary_backend.h` for packed trit transport.",
          "4. `ternary_isa.h` for instruction word layout and opcode definitions.",
          "5. `ternary_vm_state.h` and `ternary_vm.h` for execution state and opcode behavior.",
          "6. `ternary_compiler_parser.h` and `ternary_compiler_codegen.h` for current TCL syntax and lowering.",
          "7. `SYSCALL_MANIFEST.json`, `APP_MANIFEST.json`, and `IMAGE_FORMAT_MANIFEST.json` for OS contracts.",
          "",
          "If a website page, Obsidian note, or roadmap paragraph disagrees with implementation and tests, treat source and manifests as the truth."
        ].join("\n"),
      },
      {
        id: "t24",
        title: "Verifying Changes",
        content: [
          "### Verifying Changes",
          "Use focused suites first, then production before making broad health claims.",
          "",
          "```powershell",
          "tools/trit-doctor.ps1",
          "tools/trit-test.ps1 smoke",
          "python tools/trit_tool.py knowledge status",
          "tools/trit-test.ps1 production",
          "```",
          "",
          "Useful CMake targets:",
          "",
          "- `test_host_runtime` for host runner behavior.",
          "- `test_tcl_asm` for assembler coverage.",
          "- `test_phase7_compiler` for compiler/runtime behavior.",
          "- `test_ternary_lanes` and `test_native_ops` for representation and arithmetic.",
          "- `build_tos_image`, `stage_tos_release`, and `smoke_tos_release` for release images.",
          "",
          "If a failure persists, export diagnostics and inspect `build/diagnostics/latest/agent_diagnostics.json`."
        ].join("\n"),
      },
      {
        id: "t25",
        title: "What Is Still In Progress",
        content: [
          "### What Is Still In Progress",
          "The project is no longer just hypothetical, but some areas are intentionally still marked open.",
          "",
          "Current known gaps include:",
          "",
          "- Real syscall trace export behind `syscall_trace.jsonl`.",
          "- Deterministic replay traces for `tools/trit-replay.ps1`.",
          "- Guest `/bin/doctor`, `/bin/test`, `/bin/sysinfo`, `/bin/log`, and richer diagnostics commands.",
          "- Fuzz harnesses for malformed image files and bad syscall pointers.",
          "- More crash and power-loss scenarios for VFS and WAL recovery.",
          "- Broader Graphify/Trit extraction beyond the current direct AST symbols and call edges.",
          "",
          "These are tracked in `KNOWN_GAPS.md` and `ROADMAP_STATUS.json`. Keep this website honest: documented as implemented should mean covered by source and tests."
        ].join("\n"),
      },
    ],
  },
];

export interface Block {
  type: "p" | "h3" | "h4" | "code" | "ul" | "ol" | "table";
  content: string;
  lang?: string;
  items?: string[];
  rows?: string[][];
  headers?: string[];
}

export function parseMarkdown(text: string): Block[] {
  const lines = text.split("\n");
  const blocks: Block[] = [];
  let currentBlock: Block | null = null;

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];

    if (line.trim().startsWith("```")) {
      if (currentBlock && currentBlock.type === "code") {
        blocks.push(currentBlock);
        currentBlock = null;
      } else {
        if (currentBlock) {
          blocks.push(currentBlock);
        }
        const lang = line.trim().substring(3).trim();
        currentBlock = { type: "code", content: "", lang };
      }
      continue;
    }

    if (currentBlock && currentBlock.type === "code") {
      currentBlock.content += (currentBlock.content ? "\n" : "") + line;
      continue;
    }

    if (line.trim().startsWith("|")) {
      if (!currentBlock || currentBlock.type !== "table") {
        if (currentBlock) {
          blocks.push(currentBlock);
        }
        currentBlock = { type: "table", content: "", rows: [], headers: [] };
      }

      const parts = line
        .split("|")
        .map((p) => p.trim())
        .filter((_, idx, arr) => idx > 0 && idx < arr.length - 1);
      if (parts.every((p) => p.startsWith("-") || p === "")) {
        continue;
      }
      if (!currentBlock.headers || currentBlock.headers.length === 0) {
        currentBlock.headers = parts;
      } else {
        currentBlock.rows = currentBlock.rows || [];
        currentBlock.rows.push(parts);
      }
      continue;
    } else if (currentBlock && currentBlock.type === "table") {
      blocks.push(currentBlock);
      currentBlock = null;
    }

    if (line.trim().startsWith("### ")) {
      if (currentBlock) {
        blocks.push(currentBlock);
      }
      blocks.push({ type: "h3", content: line.trim().substring(4).trim() });
      currentBlock = null;
      continue;
    }

    if (line.trim().startsWith("#### ")) {
      if (currentBlock) {
        blocks.push(currentBlock);
      }
      blocks.push({ type: "h4", content: line.trim().substring(5).trim() });
      currentBlock = null;
      continue;
    }

    if (line.trim().startsWith("- ") || line.trim().startsWith("* ")) {
      const itemContent = line.trim().substring(2).trim();
      if (currentBlock && currentBlock.type === "ul") {
        currentBlock.items = currentBlock.items || [];
        currentBlock.items.push(itemContent);
      } else {
        if (currentBlock) {
          blocks.push(currentBlock);
        }
        currentBlock = { type: "ul", content: "", items: [itemContent] };
      }
      continue;
    } else if (currentBlock && currentBlock.type === "ul") {
      blocks.push(currentBlock);
      currentBlock = null;
    }

    const olMatch = line.trim().match(/^(\d+)\.\s+(.*)$/);
    if (olMatch) {
      const itemContent = olMatch[2].trim();
      if (currentBlock && currentBlock.type === "ol") {
        currentBlock.items = currentBlock.items || [];
        currentBlock.items.push(itemContent);
      } else {
        if (currentBlock) {
          blocks.push(currentBlock);
        }
        currentBlock = { type: "ol", content: "", items: [itemContent] };
      }
      continue;
    } else if (currentBlock && currentBlock.type === "ol") {
      blocks.push(currentBlock);
      currentBlock = null;
    }

    if (line.trim() === "") {
      if (currentBlock) {
        blocks.push(currentBlock);
        currentBlock = null;
      }
    } else if (currentBlock && currentBlock.type === "p") {
      currentBlock.content += " " + line.trim();
    } else {
      if (currentBlock) {
        blocks.push(currentBlock);
      }
      currentBlock = { type: "p", content: line.trim() };
    }
  }

  if (currentBlock) {
    blocks.push(currentBlock);
  }

  return blocks;
}
