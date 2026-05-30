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
    title: "1. Foundations of Balanced Ternary",
    topics: [
      {
        id: "t1",
        title: "Trytes, Words, and Widths",
        content: `### 1. Foundations of Balanced Ternary
Balanced ternary is a base-3 numeral system that uses three digits (trits):
- \`-1\` (represented as \`-\` or \`neg\`)
- \`0\` (represented as \`0\` or \`zero\`)
- \`+1\` (represented as \`+\` or \`pos\`)

Unlike binary, balanced ternary natively represents negative numbers without a sign bit or two's complement.

### Data Types and Trit Widths
The Trit Virtual Machine (TVM) and the TCL compiler define four main hardware width sizes:
1. **\`t1\` (Single Trit)**: Represents a single trit in \`{-1, 0, +1}\`. Primarily used for boolean-like conditions, ternary logic operations, and state cells.
2. **\`t9\` (Tryte)**: Formed of 9 trits. Represents integer ranges from \`-4,920\` to \`+4,920\` ($(3^9-1)/2$).
3. **\`t20\` (Half-Word)**: Formed of 20 trits. Ranges from \`-1,743,392,200\` to \`+1,743,392,200\`. Used for compact pointers and instruction offsets.
4. **\`t40\` (Word)**: The standard machine word of 40 trits. Ranges from \`-6,078,131,311,940,900\` to \`+6,078,131,311,940,900\`. This is the default integer size for general computations and pointers.

### Memory Alignment and Structs
- **Trit-Packing**: In TCL, arrays and structs are byte-aligned (or tryte-aligned in memory). A \`t1\` array is packed compactly (9 trits per tryte address).
- **Words**: Access to \`t40\` values is typically aligned to multiples of 40 trits in data memory (\`dmem\`) for optimal read/write cycles.`,
      },
      {
        id: "t2",
        title: "Balanced Ternary Arithmetic",
        content: `### Arithmetic Operators in Balanced Ternary
Arithmetic in balanced ternary has clean carry and overflow properties.

#### Addition Carry Rules:
- \`+1 + +1 = -1\` (Carry: \`+1\`)
- \`-1 + -1 = +1\` (Carry: \`-1\`)
- \`+1 + -1 = 0\` (Carry: \`0\`)

#### Multiplications:
Multiplication table is extremely elegant and requires no carry:
- \`+1 * +1 = +1\`
- \`+1 * -1 = -1\`
- \`-1 * -1 = +1\`
- Any multiplication by \`0\` yields \`0\`.

### Hardware Comparison (TCMP)
Ternary comparison is performed via the \`TCMP\` instruction, which subtracts two operands and yields a \`t1\` value directly:
- **\`-1\`** if $A < B$
- **\`0\`** if $A = B$
- **\`+1\`** if $A > B$

### Q13.13 Fixed-Point Math
Ternary fixed-point math represents fractional values in a standard \`t40\` word by assigning 13 trits to the fractional part, 13 trits to the integer part, and remaining trits for padding/sign.
Multiplication of two Q13.13 numbers requires wide multiplication and a base-3 shift of 13 trits:
\`\`\`tcl
fn q_multiply(a: t40, b: t40) -> t40 {
    // Wide multiply (implied in VMAC) followed by shift right by 13 trits
    var prod: t40 = a * b;
    return prod >> 13;
}
\`\`\``,
      },
      {
        id: "t3",
        title: "Ternary Logic & Logic Gates",
        content: `### Three-Valued Logic
Ternary logic generalizes Boolean algebra. Conditions evaluate to \`t1\`, containing \`-1\` (False), \`0\` (Unknown), or \`+1\` (True).

#### Truth Tables:
* **AND (Min)**: Returns the smaller trit.
  - \`True AND Unknown\` $\rightarrow$ \`Unknown\` (\`0\`)
  - \`False AND Unknown\` $\rightarrow$ \`False\` (\`-1\`)
* **OR (Max)**: Returns the larger trit.
  - \`True OR Unknown\` $\rightarrow$ \`True\` (\`+1\`)
  - \`False OR Unknown\` $\rightarrow$ \`Unknown\` (\`0\`)
* **NOT (Negation/Inversion)**:
  - \`NOT True\` $\rightarrow$ \`False\`
  - \`NOT False\` $\rightarrow$ \`True\`
  - \`NOT Unknown\` $\rightarrow$ \`Unknown\` (\`0\`)

### Constant-Time Selection (TSEL)
Branch-free execution is critical in cryptographic and optimized code. The \`TSEL\` (Ternary Select) instruction allows selecting values based on a \`t1\` condition without branches:
\`\`\`tasm
// TASM: TSEL Rd, Rcond, Rpos, Rneg, Rzero
// Rd = (Rcond == +1) ? Rpos : (Rcond == -1) ? Rneg : Rzero
TSEL r3, r1, r4, r5, r6
\`\`\`
In TCL, this is exposed via match statements or native helper functions:
\`\`\`tcl
var val: t40 = ct_select(cond, a, b, c);
\`\`\``,
      },
    ],
  },
  {
    id: "m2",
    title: "2. The Hardware & Virtual Machine",
    topics: [
      {
        id: "t4",
        title: "The Ternary VM (TVM) Execution Model",
        content: `### The TVM Execution Cycle
The Ternary Virtual Machine (TVM) executes compiled \`.txe\` binary instruction words sequentially.

#### The Core Loop:
1. **Fetch**: Read the 27-trit instruction word from Instruction Memory (\`imem\`) at the address pointed by the Program Counter (\`PC\`).
2. **Decode**: Extract the opcode (4 trits), register fields, and optional immediate value (up to 19 trits).
3. **Execute**: Execute the hardware operation, mutating the register file or memory.
4. **Increment**: Update \`PC\` (relative to branches or function returns).

### Emulation and Cycle Telemetry
During VM runs, the execution engine tracks CPU cycles:
- **Instruction Cost**: Basic register operations (add, sub, shift) cost 1 cycle. Memory loads and stores cost 3 cycles. Branch/jump instructions cost 2 cycles.
- **VM Telemetry**: Telemetry reports instruction density, register mutations, and peak memory usage, allowing cycle-accurate performance analysis.`,
      },
      {
        id: "t5",
        title: "Virtual Memory Layout",
        content: `### Instruction and Data Memory
The TVM separates execution code from data (Harvard Architecture design mapped into a unified address space):

1. **Instruction Memory (\`imem\`)**:
   - Contains instruction words. 
   - Non-writable during standard execution.
   - Typically sized to 262,144 words.

2. **Data Memory (\`dmem\`)**:
   - Read-write memory containing variables, buffers, heap allocations, and stack frames.
   - Size typically defaults to 16,777,216 words.

### Stack and Heap Segments
- **The Stack**: Grows downwards from the high memory boundary towards lower addresses. The Stack Pointer (\`sp\`) track active frames.
- **The Heap**: Grows upwards from the end of the static data segment. Allocations are managed via \`malloc_raw\` and \`free_raw\` in standard library.`,
      },
      {
        id: "t6",
        title: "The Register File and Processor State",
        content: `### Register Allocation
The TVM defines 27 general-purpose registers labeled \`r0\` to \`r26\`:

| Name | Alias | Role |
|---|---|---|
| \`r0\` | \`zero\` | Hardwired to zero (0). Writes are ignored. |
| \`r1\` - \`r12\` | \`t0\` - \`t11\` | Temporary registers (caller-saved). |
| \`r13\` | \`rv\` | Return Value register. Standard for function outputs. |
| \`r14\` - \`r24\` | \`s0\` - \`s10\` | Saved registers (callee-saved). |
| \`r25\` | \`lr\` | Link Register. Stores the return PC for function calls. |
| \`r26\` | \`sp\` | Stack Pointer. Tracks the top of the execution stack. |

### Register Telemetry
In the TreatCode interface, the **Registers** tab displays the contents of all 27 registers. The UI automatically highlights registers that changed in the last executed step and supports switching display representations between:
- **Decimal**: Standard base-10 integers.
- **Balanced Ternary**: Base-3 string representations of trytes (e.g. \`0+0--+-\`).`,
      },
      {
        id: "t7",
        title: "Hardware Acceleration & Atomics",
        content: `### Vector Instruction Support (VMAC)
The TVM implements Vector Multiply-Accumulate (\`VMAC\`) instructions that accelerate dot product operations natively in hardware:
- Computes $Y = Y + A \times B$ across arrays of trits or words.
- Maps directly to ternary neural network inference tasks.

### Concurrency and Atomics
TVM provides three hardware instructions to manage concurrent execution safely:
1. **\`TLDR\` (Ternary Load-Reserved)**: Loads a value from memory and registers an active reservation on that address.
2. **\`TSTR\` (Ternary Store-Conditional)**: Attempts to store a value to a reserved address. Succeeds and yields \`+1\` if the reservation is still valid; yields \`-1\` if another thread mutated the memory in the interim.
3. **\`FENCE\` (Memory Fence)**: Enforces memory ordering across instructions, preventing reads and writes from reordering around the fence boundary.`,
      },
    ],
  },
  {
    id: "m3",
    title: "3. ABI & System Interface",
    topics: [
      {
        id: "t8",
        title: "The Application Binary Interface (ABI)",
        content: `### Function Calling Conventions
The TCL compiler adheres to a strict binary interface layout to enable interoperability:

- **Arguments passing**: The first 6 parameters of a function are passed in registers \`r1\` through \`r6\`. Additional parameters are pushed onto the stack.
- **Return Values**: A single return value is placed in register \`r13\` (\`rv\`). Wide return structures are returned by passing a pointer to a pre-allocated buffer as the first parameter.
- **Saved Registers**: Registers \`r14\` to \`r24\` must be preserved across function calls. If a function mutates them, it must spill them to the stack on entry and restore them on exit.

### Stack Frame Layout
On function entry, the compiler establishes the stack frame:
\`\`\`tasm
// Function prologue
sub sp, sp, 3   // Allocate 3 stack slots
store sp, lr, 0 // Save link register
store sp, s0, 1 // Save callee-saved s0
\`\`\`
Before return, the epilogue restores registers and jumps back:
\`\`\`tasm
// Function epilogue
load lr, sp, 0  // Restore link register
load s0, sp, 1  // Restore s0
add sp, sp, 3   // Deallocate stack
ret             // Return to caller
\`\`\``,
      },
      {
        id: "t9",
        title: "Kernel Traps, CSRs, and System Calls",
        content: `### Privilege Levels and Traps
The TVM supports User Mode (restrictive memory access) and Kernel Mode (unrestricted hardware control). Traps occur on illegal operations, page faults, or syscall calls.

### Control and Status Registers (CSRs)
System state is managed through special registers:
- **\`TVEC\` (Trap Vector)**: Address of the kernel trap handler.
- **\`EPC\` (Exception Program Counter)**: Stores the PC where the trap or interrupt occurred.
- **\`CAUSE\`**: Details the reason for the trap (e.g., divide by zero, page violation, syscall trigger).

### System Calls
Syscalls are triggered using the \`SYSCALL\` instruction with an immediate code.
- **Syscall 22 (\`sys_write_char\`)**: Renders a character to the console. The character code is passed in register \`r1\`.
- Standard output utilities in \`ulib.trit\` (\`print_char\`, \`print_string\`) wrap Syscall 22 to output legible ASCII logs instead of raw decimal values.`,
      },
      {
        id: "t10",
        title: "Memory Management & Page Tables",
        content: `### Page-Based Virtual Memory
The VM memory access maps virtual pages to physical pages via page tables:
- **Page Size**: Typically 243 words ($3^5$).
- **Page Table Entry (PTE)**: Stores translation mapping, validation trits, and permissions:
  - **Writable (W)**: Trit value determining if the page is modifiable.
  - **Readable (R)**: Trit value determining if data can be read.
  - **Executable (X)**: Trit value governing instruction fetches.

### Page Guard Protections
By enforcing strict PTE permissions, the kernel isolates memory:
- **Execute-Only (XO)**: Pages containing library code can be marked executable but not readable/writable (\`X = +1, R = -1, W = -1\`). Prevents user code from scanning system code to extract sensitive execution layouts.`,
      },
    ],
  },
  {
    id: "m4",
    title: "4. TCL Language Specification",
    topics: [
      {
        id: "t11",
        title: "Matches, Branches, and Control Flow",
        content: `### Control Flow in TCL 1.0
TCL 1.0 does not use standard boolean values for branching. Instead, control flow is structured around ternary logic and pattern matching.

### The Match Statement
The \`match\` construct is the primary mechanism for conditional branching. It requires all three arms to handle the ternary outcomes:
\`\`\`tcl
match x {
    neg => {
        // executed if x < 0
    }
    zero => {
        // executed if x == 0
    }
    pos => {
        // executed if x > 0
    }
}
\`\`\`

### While Loops and Conditions
Loops use a condition that evaluates to a ternary status (\`t1\`). A loop continues as long as the condition evaluates to \`pos\` (\`+1\`).
If the condition evaluates to \`neg\` or \`zero\`, the VM branches out of the loop:
\`\`\`tcl
var i: t40 = 0;
while len - i > 0 { // condition evaluates to pos while i < len
    // loop body
    i = i + 1;
}
\`\`\``,
      },
      {
        id: "t12",
        title: "Ownership and Borrowing Semantics",
        content: `### Linear Typing and Ownership
TCL enforces strict memory safety without a garbage collector through an ownership model:

1. **Ownership (\`own<T>\`)**: 
   - Indicates exclusive ownership of a resource (e.g. a heap-allocated buffer).
   - Moving the variable (e.g., passing it to a function) invalidates the original variable. Re-using it will raise a compiler error.
   - When an \`own<T>\` goes out of scope, the compiler automatically inserts a cleanup/drop routine.

2. **Immutable Borrowing (\`borrow<T>\`)**:
   - Creates a read-only reference.
   - Multiple active borrows can coexist, but the underlying data cannot be mutated.

3. **Mutable Borrowing (\`borrow_mut<T>\`)**:
   - Creates an exclusive read-write reference.
   - No other borrows (mutable or immutable) can exist at the same time.

### Compiler Lifetime Enforcement
The compiler's borrow-checker verifies at compile-time that:
- References never outlive the data they point to.
- Aliasing is strictly controlled (either one mutable borrow or many immutable borrows, never both).`,
      },
      {
        id: "t13",
        title: "Safe Pointer States",
        content: `### Pointer Typings in TCL
TCL separates raw pointers from references by tracking safety states at compile time:
\`\`\`tcl
ptr<T, Space, State>
\`\`\`
- **\`Space\`**: Memory region (\`user\`, \`kernel\`, \`shared\`).
- **\`State\`**: Safety categorization (\`valid\`, \`null\`, \`unknown\`).

### Pointer Matching
You cannot dereference a pointer in the \`unknown\` state. You must first match its address state using a pointer match block:
\`\`\`tcl
match ptr {
    null => {
        // safe recovery path
    }
    valid => {
        // dereferencing is allowed here!
        var val: t40 = load(ptr);
    }
    unknown => {
        // fallback or error handling
    }
}
\`\`\`
This pattern prevents null-pointer dereferences and enforces safety at the compiler level.`,
      },
      {
        id: "t14",
        title: "Width Polymorphism",
        content: `### Parametric Widths
Width polymorphism allows writing functions that operate generically over different trit widths (e.g., \`t1\`, \`t9\`, \`t20\`, \`t40\`).

### Syntax:
\`\`\`tcl
fn clamp<W: TritWidth>(x: T<W>, lo: T<W>, hi: T<W>) -> T<W> {
    match tcmp(x, lo) {
        neg => { return lo; }
        zero => { return x; }
        pos => {
            match tcmp(x, hi) {
                pos => { return hi; }
                zero => { return x; }
                neg => { return x; }
            }
        }
    }
}
\`\`\`

### Monomorphization
The compiler doesn't run generic instructions at runtime. Instead, during compilation, it performs **monomorphization**:
- It locates all concrete invocations of the generic function (e.g. \`clamp<t40>\`, \`clamp<t9>\`).
- It generates specific, optimized code copies for each width.
- This ensures maximum performance with no runtime generic dispatch overhead.`,
      },
    ],
  },
  {
    id: "m5",
    title: "5. Inside the Compiler Pipeline",
    topics: [
      {
        id: "t15",
        title: "Lexer and Parser Specifications",
        content: `### Lexical Analysis
The Lexer (\`tcl_lexer.trit\`) processes raw TCL source code character-by-character and outputs a stream of tokens (\`tcl_token.trit\`). 
- **Ternary Whitespace**: The parser ignores comments and standard whitespace.
- **Literals**: Identifies base-10 digits, ternary string literals, and identifiers.

### Syntactic Parsing
The Parser (\`tcl_parser.trit\`) consumes tokens and builds the Abstract Syntax Tree (AST) (\`tcl_ast.trit\`).
- **Recursive Descent**: It uses a recursive descent parsing model to validate blocks, functions, matches, variable declarations, and expressions.
- **Syntax Diagnostics**: Generates precise error markers on syntax violations.`,
      },
      {
        id: "t16",
        title: "Type Inference and Validation Passes",
        content: `### Semantic Validation
Once the AST is built, the compiler executes validation passes:

1. **Type Inference (\`tcl_infer.trit\` / \`tcl_type.trit\`)**:
   - Resolves variable types, function signatures, and constraints.
   - Validates width boundaries for arithmetic and assignments.
2. **Borrow Checking**:
   - Analyzes lifetimes and scope.
   - Enforces ownership move rules on memory allocations.
3. **Pointer State Propagation**:
   - Conducts flow-sensitive analysis of pointer variables, ensuring no pointer is dereferenced unless its state is proven \`valid\` in the current basic block.`,
      },
      {
        id: "t17",
        title: "TCL Intermediate Representation (IR)",
        content: `### The IR CFG Layout
After typechecking, the AST is lowered into a Control Flow Graph (CFG) of Intermediate Representation (IR) blocks (\`tcl_ir.trit\`).

- **Basic Blocks**: Contain linear sequences of operations with no branching. Branches and matches can only occur at block terminators.
- **SSA Form**: The compiler utilizes Single Static Assignment (SSA) form in the IR, making dataflow optimizations and variable lifetimes explicit.
- **Lowering Loops**: High-level loops (\`while\`) are lowered into conditional blocks that branch back to the loop header or forward to the exit block based on a ternary condition.`,
      },
      {
        id: "t18",
        title: "TASM Code Generation",
        content: `### Lowering IR to Assembly
The Backend (\`tcl_backend.trit\` / \`tcl_backend_codegen.trit\`) translates optimized IR into Trit Assembly (TASM) instructions:

- **Instruction Selection**: Maps high-level operations (adds, matching, pointer lookups) to discrete assembly commands.
- **Spill Handling**: If the code utilizes more variables than the active register allocation limit (24 temporary/saved registers), the code generator allocates stack offset frame slots and emits \`store\`/\`load\` spill sequences.
- **Label Generation**: Inserts symbolic labels for branch targets and function entrance points.`,
      },
      {
        id: "t19",
        title: "Optimization Passes",
        content: `### Maximizing Efficiency
The compiler executes optimization runs to minimize instruction counts and cycle times:

1. **Register Allocation (\`tcl_backend_alloc.trit\`)**:
   - Performs lifetime analysis on variables.
   - Maps variables to active registers, maximizing temporary usage and avoiding costly stack spills.
2. **Peephole Optimization**:
   - Scans output assembly for redundant load-after-stores, jumps-to-jumps, or identity calculations (e.g. adding 0), replacing them with shorter sequences.
3. **Strength Reduction**:
   - Replaces computationally heavy operations with faster alternatives (e.g., substituting multiplications by multiples of 3 with base-3 shift operations).`,
      },
    ],
  },
  {
    id: "m6",
    title: "6. Toolchain & Developer Workflows",
    topics: [
      {
        id: "t20",
        title: "The Assembler",
        content: `### Assembling TASM text into TXE binaries
The Assembler (\`tcl_asm.trit\` / \`ternary_asm.h\`) parses text assembly instructions and generates binary output:

- **Symbol Resolution**: Operates in two passes. The first pass aggregates all label declarations and maps them to program counter indexes. The second pass replaces all label usages with absolute or PC-relative offset values.
- **Opcode Packing**: Groups instruction opcodes, registers, and immediates into semantic 27-trit words, which are written as a consecutive executable binary array (\`.txe\`).
- **Data Sections**: Resolves static variables, constants, and pre-allocated arrays into structural data segments.`,
      },
      {
        id: "t21",
        title: "Bootstrapping the Compiler",
        content: `### The Self-Hosting Sequence
To ensure compiler independence, the system utilizes a bootstrapping pipeline:

\`\`\`
  [TCL Source Code]
         │
         ▼ (Compiled by Legacy C++ compiler on host)
  [Native Executable (E1)]
         │
         ▼ (Executes in VM, compiling the same TCL Source)
  [Self-Hosted Executable (E2)]
\`\`\`

- **Phase E1**: The bootstrap compiler (written in C++) compiles the native TCL compiler source files into a temporary binary.
- **Phase E2**: This temporary binary is executed *inside the VM* to compile the compiler source files again.
- **Verification**: If the binary generated in E1 and the binary generated in E2 match down to the bit level, self-hosting is complete and the C++ compiler source code can be safely decommissioned.`,
      },
      {
        id: "t22",
        title: "Benchmarking and Profile-Guided Cycles",
        content: `### Writing High-Performance Ternary Code
Optimizing programs on the TVM is scored based on **CPU Cycle Count**. 

#### Cycle Minimization Guidelines:
1. **Reduce Stack Spills**: Restrict the number of active variables in hot loops to under 24 to avoid memory-store cycles.
2. **Avoid Branches**: Use \`TSEL\` or match structures that resolve branch-free instead of nested comparisons.
3. **Array Access**: Keep pointers in registers and increment them sequentially (leveraging pointer arithmetic offsets) rather than recalculating array addresses inside loops.
4. **Leverage shifts**: Replace division and multiplication by constants with left/right shifts (\`<<\` and \`>>\`) which execute in 1 cycle.`,
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

    // 1. Code Block Handling
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

    // 2. Table Handling
    if (line.trim().startsWith("|")) {
      if (!currentBlock || currentBlock.type !== "table") {
        if (currentBlock) {
          blocks.push(currentBlock);
        }
        currentBlock = { type: "table", content: "", rows: [], headers: [] };
      }
      
      const parts = line.split("|").map(p => p.trim()).filter((_, idx, arr) => idx > 0 && idx < arr.length - 1);
      if (parts.every(p => p.startsWith("-") || p === "")) {
        continue;
      }
      if (!currentBlock.headers || currentBlock.headers.length === 0) {
        currentBlock.headers = parts;
      } else {
        currentBlock.rows = currentBlock.rows || [];
        currentBlock.rows.push(parts);
      }
      continue;
    } else {
      if (currentBlock && currentBlock.type === "table") {
        blocks.push(currentBlock);
        currentBlock = null;
      }
    }

    // 3. Headers
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

    // 4. Unordered Lists
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
    } else {
      if (currentBlock && currentBlock.type === "ul") {
        blocks.push(currentBlock);
        currentBlock = null;
      }
    }

    // 5. Ordered Lists
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
    } else {
      if (currentBlock && currentBlock.type === "ol") {
        blocks.push(currentBlock);
        currentBlock = null;
      }
    }

    // 6. Paragraphs
    if (line.trim() === "") {
      if (currentBlock) {
        blocks.push(currentBlock);
        currentBlock = null;
      }
    } else {
      if (currentBlock && currentBlock.type === "p") {
        currentBlock.content += " " + line.trim();
      } else {
        if (currentBlock) {
          blocks.push(currentBlock);
        }
        currentBlock = { type: "p", content: line.trim() };
      }
    }
  }

  if (currentBlock) {
    blocks.push(currentBlock);
  }

  return blocks;
}

