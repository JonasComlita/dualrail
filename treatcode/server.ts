import express, { Request, Response } from "express";
import cors from "cors";
import fs from "fs";
import path from "path";
import { exec } from "child_process";

const app = express();
const PORT = process.env.PORT || 3000;

app.use(cors());
app.use(express.json());

// In production, Vite builds static files to 'dist'. Serve them.
const distPath = path.join(__dirname, "dist");
if (fs.existsSync(distPath)) {
  app.use(express.static(distPath));
}

// In-memory databases
interface LeaderboardEntry {
  id: number;
  problemId: string;
  name: string;
  engine: "native" | "bootstrap";
  cycles: number;
  date: string;
}

const leaderboard: LeaderboardEntry[] = [
  // T001
  { id: 1, problemId: "T001", name: "TritWizard", engine: "native", cycles: 3, date: "2026-05-21" },
  { id: 2, problemId: "T001", name: "balanced_0xff", engine: "bootstrap", cycles: 3, date: "2026-05-20" },
  // T002
  { id: 3, problemId: "T002", name: "TritWizard", engine: "native", cycles: 124500, date: "2026-05-21" },
  { id: 4, problemId: "T002", name: "balanced_0xff", engine: "bootstrap", cycles: 8520, date: "2026-05-20" },
  // T005
  { id: 5, problemId: "T005", name: "TritWizard", engine: "bootstrap", cycles: 1980, date: "2026-05-21" },
  { id: 6, problemId: "T005", name: "balanced_0xff", engine: "native", cycles: 38700, date: "2026-05-20" },
  // T056
  { id: 7, problemId: "T056", name: "balanced_0xff", engine: "bootstrap", cycles: 4200, date: "2026-05-21" },
  { id: 8, problemId: "T056", name: "TritWizard", engine: "native", cycles: 65400, date: "2026-05-20" }
];

interface TestCase {
  arg: any;
  expectedOutput?: string;
  expectedR13?: number;
}

interface Problem {
  id: string;
  title: string;
  difficulty: "easy" | "medium" | "hard";
  category: string;
  tags: string[];
  description: string;
  signature: string;
  template: string;
  testCases?: TestCase[];
  generateWrapper?: (code: string, arg: any) => string;
}

const problems: Problem[] = [
  {
    id: "T001",
    title: "Three-Way Sign Test",
    difficulty: "easy",
    category: "three-valued",
    tags: ["T1", "match"],
    description: `Given a \`t40\` value \`x\`, return its ternary sign — \`-1\` if negative, \`0\` if zero, \`+1\` if positive.

### Constraints
- Do not use \`if\` statements. The three-arm \`match\` construct is the intended solution mechanism. All three arms must be present.

### Examples
- \`sign_test(-42)\` → \`-1\`
- \`sign_test(0)\` → \`0\`
- \`sign_test(99)\` → \`+1\``,
    signature: "fn sign_test(x: t40) -> t40",
    template: `// T001 — Three-Way Sign Test
// Given a t40 value, return:
//   -1  if x is negative
//    0  if x is zero
//   +1  if x is positive
//
// Constraint: no if statements.
// Use the native three-arm match.

import ulib;

fn sign_test(x: t40) -> t40 {
    // your solution here
    
}`,
    testCases: [
      { arg: -42, expectedR13: -1 },
      { arg: 0, expectedR13: 0 },
      { arg: 99, expectedR13: 1 },
      { arg: -1, expectedR13: -1 },
      { arg: 1, expectedR13: 1 }
    ],
    generateWrapper: (code, arg) => `
${code}

fn main() -> t40 {
    var res: t40 = sign_test(${arg});
    return res;
}
`
  },
  {
    id: "T002",
    title: "Ternary FizzBuzz",
    difficulty: "easy",
    category: "three-valued",
    tags: ["T1", "match", "arithmetic"],
    description: `Given an integer \`N\`, print numbers from \`1\` to \`N\` (inclusive).
    
For each number \`i\`:
- If \`i\` is a multiple of 3, print \`"Trit"\` followed by a newline.
- Otherwise, print \`i\` followed by a newline (using \`sys_write_int\` and \`sys_newline\`).

### Constraints
- \`1 <= N <= 50\`

### Example Output for N = 5
\`\`\`text
1
2
Trit
4
5
\`\`\``,
    signature: "fn multiples_of_three(n: t40) -> t40",
    template: `// T002 — Ternary FizzBuzz
// Given an integer n, print numbers from 1 to n (inclusive).
// For each number i:
//   - If i is a multiple of 3, print "Trit" followed by a newline.
//   - Otherwise, print i followed by a newline.
//
// Use: print_char(c) or print_string(str) or sys_write_char(c) or sys_write_int(val) or sys_newline()

import ulib;

fn multiples_of_three(n: t40) -> t40 {
    var i: t40 = 1;
    while n - i >= 0 {
        // Your code here:
        
        i = i + 1;
    }
    return 0;
}`,
    testCases: [
      { arg: 5, expectedOutput: "1\n2\nTrit\n4\n5\n" },
      { arg: 12, expectedOutput: "1\n2\nTrit\n4\n5\nTrit\n7\n8\nTrit\n10\n11\nTrit\n" }
    ],
    generateWrapper: (code, arg) => `
${code}

fn main() -> t40 {
    multiples_of_three(${arg});
    return 0;
}
`
  },
  {
    id: "T003",
    title: "Three-State Machine",
    difficulty: "easy",
    category: "three-valued",
    tags: ["T1", "enum"],
    description: `Design a simple finite state machine using three-valued logic state values. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T003 — Three-State Machine
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T004",
    title: "Null Pointer Safety Chain",
    difficulty: "medium",
    category: "pointer-safety",
    tags: ["ptr<T,S>", "match"],
    description: `Safely traverse a chain of pointers, handling any null pointer states securely. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T004 — Null Pointer Safety Chain
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T005",
    title: "Validated Buffer Walk",
    difficulty: "medium",
    category: "pointer-safety",
    tags: ["ptr<T,S>", "borrow"],
    description: `Given a pointer to a null-terminated ASCII string, invert the characters of the string **in-place**.
    
You must reverse the string in the allocated buffer without using extra string memory allocations.

### Constraints
- String length up to 50 characters.
- Memory pointer is guaranteed to be \`valid\` and accessible.

### Example
- Input: \`"hello"\`
- Output: \`"olleh"\``,
    signature: "fn invert_string(str: borrow<ptr<t40, user, valid>>) -> t40",
    template: `// T005 — Validated Buffer Walk
// Given a pointer to a null-terminated ASCII string, invert the characters of the string in-place.
// You must reverse the string in the allocated buffer without using extra string memory allocations.

import ulib;

fn invert_string(str: borrow<ptr<t40, user, valid>>) -> t40 {
    // Write your in-place string reversal code here
    
    return 0;
}`,
    testCases: [
      { arg: "hello", expectedOutput: "olleh" },
      { arg: "Trit", expectedOutput: "tirT" },
      { arg: "abcdefg", expectedOutput: "gfedcba" }
    ],
    generateWrapper: (code, arg) => `
${code}

fn main() -> t40 {
    var s: own<ptr<t40, user, unknown>> = malloc_raw(${arg.length + 1});
    match s {
        valid(p) => {
            unsafe {
                ${arg.split("").map((c: string, idx: number) => `store(p + ${idx}, ${c.charCodeAt(0)});`).join("\n                ")}
                store(p + ${arg.length}, 0);
            }
            invert_string(p);
            print_string(p);
            free_raw(p);
        }
        unknown => {}
        null => {}
    }
    return 0;
}
`
  },
  {
    id: "T006",
    title: "Ownership Transfer Chain",
    difficulty: "hard",
    category: "pointer-safety",
    tags: ["own<T>", "move"],
    description: `Implement ownership transfer mechanics across multiple worker context queues. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T006 — Ownership Transfer Chain
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T007",
    title: "Balanced Ternary Addition",
    difficulty: "easy",
    category: "arithmetic",
    tags: ["T40", "carry"],
    description: `Implement carry-save balanced ternary addition. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T007 — Balanced Ternary Addition
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T008",
    title: "Sign-Free Absolute Value",
    difficulty: "easy",
    category: "arithmetic",
    tags: ["T40", "match"],
    description: `Compute the absolute value of a balanced ternary number without explicit branch testing. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T008 — Sign-Free Absolute Value
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T009",
    title: "Ternary Integer Square Root",
    difficulty: "medium",
    category: "arithmetic",
    tags: ["T40", "binary-search"],
    description: `Compute the integer square root using base-3 binary-search variants. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T009 — Ternary Integer Square Root
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T010",
    title: "Big Integer Limb Multiply",
    difficulty: "hard",
    category: "arithmetic",
    tags: ["T40", "montgomery"],
    description: `Implement large limb multiplication matching Montgomery reduction layouts. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T010 — Big Integer Limb Multiply
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T011",
    title: "TLDR/TSTR Atomic Counter",
    difficulty: "medium",
    category: "concurrent",
    tags: ["tldr", "tstr", "atomic"],
    description: `Implement a thread-safe atomic counter using Triton-27 TLDR and TSTR assembly primitives. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T011 — TLDR/TSTR Atomic Counter
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T012",
    title: "Lock-Free Stack",
    difficulty: "hard",
    category: "concurrent",
    tags: ["tldr", "tstr", "shared<T>"],
    description: `Build a lock-free stack using hardware-native CAS-like instructions. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T012 — Lock-Free Stack
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T013",
    title: "Split Buffer Claim Race",
    difficulty: "hard",
    category: "concurrent",
    tags: ["SplitBuf", "atomic"],
    description: `Coordinate buffer claims across parallel Triton threads under high contention. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T013 — Split Buffer Claim Race
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T014",
    title: "Ternary Search on Sorted Array",
    difficulty: "easy",
    category: "dsa",
    tags: ["T40", "search"],
    description: `Implement logarithmic search partitioning the search space into three equal parts. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T014 — Ternary Search on Sorted Array
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T015",
    title: "Three-Way Partition Quicksort",
    difficulty: "medium",
    category: "dsa",
    tags: ["vec_sort", "pivot"],
    description: `Implement Dutch National Flag partition logic for array sorting. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T015 — Three-Way Partition Quicksort
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T016",
    title: "TST String Interning",
    difficulty: "medium",
    category: "dsa",
    tags: ["TST", "string"],
    description: `Implement a Ternary Search Tree for space-efficient string interning lookup. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T016 — TST String Interning
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T017",
    title: "Ternary Heap Priority Queue",
    difficulty: "hard",
    category: "dsa",
    tags: ["heap", "T40"],
    description: `Implement a 3-ary heap priority queue with log3-bound insertion complexity. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T017 — Ternary Heap Priority Queue
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T018",
    title: "Montgomery Ladder Step",
    difficulty: "hard",
    category: "crypto",
    tags: ["montgomery", "T40"],
    description: `Coordinate constant-time ladder steps to protect against timing attacks. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T018 — Montgomery Ladder Step
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T019",
    title: "Constant-Time Comparison",
    difficulty: "medium",
    category: "crypto",
    tags: ["T1", "timing"],
    description: `Evaluate memory block equality with uniform instructions to prevent side-channel leaks. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T019 — Constant-Time Comparison
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T020",
    title: "Execute-Only Page Guard",
    difficulty: "hard",
    category: "crypto",
    tags: ["MMU", "PTE"],
    description: `Enforce execute-only page table permissions in Virtual Memory structures. (Compile Check Only Mode)`,
    signature: "fn main() -> t40",
    template: `// T020 — Execute-Only Page Guard
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`
  },
  {
    id: "T021",
    title: "Three-State Conway's Game",
    difficulty: "medium",
    category: "three-valued",
    tags: ["T1","cellular-automaton"],
    description: "Implement a Conway's Game of Life variant on a grid using three states: dead (-1), dormant (0), and alive (+1). Birth and survival rules use T1 comparisons on neighbor counts.",
    signature: "fn conway_step(grid: borrow<[t1]>, width: t40, height: t40, next_grid: borrow_mut<[t1]>) -> t40",
    template: "// T021 — Three-State Conway's Game\nimport ulib;\n\nfn conway_step(grid: borrow<[t1]>, width: t40, height: t40, next_grid: borrow_mut<[t1]>) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T022",
    title: "Ternary Run-Length Encoding",
    difficulty: "easy",
    category: "three-valued",
    tags: ["match","rle"],
    description: "Given a t40 array, encode it as a sequence of (value, count) pairs. Use a three-arm match on value equality to drive the state machine cleanly.",
    signature: "fn encode_rle(in_arr: borrow<[t40]>, out_val: borrow_mut<[t40]>, out_count: borrow_mut<[t40]>) -> t40",
    template: "// T022 — Ternary Run-Length Encoding\nimport ulib;\n\nfn encode_rle(in_arr: borrow<[t40]>, out_val: borrow_mut<[t40]>, out_count: borrow_mut<[t40]>) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T023",
    title: "Balanced Logic Evaluator",
    difficulty: "medium",
    category: "three-valued",
    tags: ["T1","expression-tree"],
    description: "Evaluate an expression tree where AND/OR/NOT operations act on T1 values. Note that T1 is not a standard bool: -1 is False, 0 is Unknown, +1 is True. E.g., (-1) AND (+1) is False, but (0) OR (+1) is True.",
    signature: "fn evaluate_expr(nodes: borrow<[t40]>, left: borrow<[t40]>, right: borrow<[t40]>, root: t40) -> t40",
    template: "// T023 — Balanced Logic Evaluator\nimport ulib;\n\nfn evaluate_expr(nodes: borrow<[t40]>, left: borrow<[t40]>, right: borrow<[t40]>, root: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T024",
    title: "Three-Way String Classifier",
    difficulty: "easy",
    category: "three-valued",
    tags: ["match","character"],
    description: "Given a character code (t40), return: neg (-1) if it's a control character, zero (0) if it's whitespace, or pos (+1) if it's a printable character. Use range comparisons returning T1 with one match block and no lookup tables.",
    signature: "fn classify_char(c: t40) -> t1",
    template: "// T024 — Three-Way String Classifier\nimport ulib;\n\nfn classify_char(c: t40) -> t1 {\n    // your solution here\n    return zero;\n}"
  },
  {
    id: "T025",
    title: "Ternary Carry Propagation",
    difficulty: "medium",
    category: "three-valued",
    tags: ["T1","carry-propagation"],
    description: "Given a t40 array of trit digits in {-1, 0, +1}, normalize them into a balanced ternary format with no outstanding carries. Perform carry-chain handling iteratively using TCMP.",
    signature: "fn propagate_carries(digits: borrow_mut<[t40]>) -> t40",
    template: "// T025 — Ternary Carry Propagation\nimport ulib;\n\nfn propagate_carries(digits: borrow_mut<[t40]>) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T026",
    title: "Nullable Field Chain",
    difficulty: "easy",
    category: "pointer-safety",
    tags: ["ptr","match"],
    description: "Safely traverse a nested field chain a.b.c where each field pointer is of type ptr<t40, user, unknown>. Ensure every pointer state is checked before dereferencing to prevent compilation failures.",
    signature: "fn access_chain(a: ptr<ptr<t40, user, unknown>, user, unknown>) -> t40",
    template: "// T026 — Nullable Field Chain\nimport ulib;\n\nfn access_chain(a: ptr<ptr<t40, user, unknown>, user, unknown>) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T027",
    title: "Pointer Chain Reversal",
    difficulty: "medium",
    category: "pointer-safety",
    tags: ["ptr","linked-list"],
    description: "Reverse a singly linked list in-place where next links are ptr<Node, user, unknown>. Ensure pointer states are validated and tracked correctly after updating each link.",
    signature: "fn reverse_list(head: ptr<t40, user, unknown>) -> ptr<t40, user, unknown>",
    template: "// T027 — Pointer Chain Reversal\nimport ulib;\n\nfn reverse_list(head: ptr<t40, user, unknown>) -> ptr<t40, user, unknown> {\n    // your solution here\n    return head;\n}"
  },
  {
    id: "T028",
    title: "Move Without Copy",
    difficulty: "medium",
    category: "pointer-safety",
    tags: ["own","move"],
    description: "Transfer ownership of a heap-allocated buffer through three successive function calls without cloning or copying. The compiler will check that the source binding is invalidated immediately after each transfer.",
    signature: "fn process_buffer(buf: own<ptr<t40, user, valid>>) -> own<ptr<t40, user, valid>>",
    template: "// T028 — Move Without Copy\nimport ulib;\n\nfn process_buffer(buf: own<ptr<t40, user, valid>>) -> own<ptr<t40, user, valid>> {\n    // your solution here\n    return buf;\n}"
  },
  {
    id: "T029",
    title: "Safe Arena Allocator",
    difficulty: "hard",
    category: "pointer-safety",
    tags: ["ptr","allocator"],
    description: "Implement a basic bump allocator over a fixed memory arena. Return ptr<t40, user, valid> on successful allocation, and ptr<t40, user, null> on exhaustion. Callers must validate pointer status before using the memory.",
    signature: "fn arena_alloc(arena: borrow_mut<ptr<t40, user, valid>>, size: t40, end: ptr<t40, user, valid>) -> ptr<t40, user, unknown>",
    template: "// T029 — Safe Arena Allocator\nimport ulib;\n\nfn arena_alloc(arena: borrow_mut<ptr<t40, user, valid>>, size: t40, end: ptr<t40, user, valid>) -> ptr<t40, user, unknown> {\n    // your solution here\n    return arena;\n}"
  },
  {
    id: "T030",
    title: "Ternary GCD",
    difficulty: "easy",
    category: "arithmetic",
    tags: ["TCMP","euclidean"],
    description: "Implement the Euclidean GCD algorithm using TCMP for remainder checks. Balanced ternary representation is highly efficient because remainder calculations can be negative without affecting loop termination.",
    signature: "fn ternary_gcd(a: t40, b: t40) -> t40",
    template: "// T030 — Ternary GCD\nimport ulib;\n\nfn ternary_gcd(a: t40, b: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T031",
    title: "Ternary Integer Logarithm",
    difficulty: "medium",
    category: "arithmetic",
    tags: ["TCMP","while-loop"],
    description: "Compute the floor of log₃(n) for n > 0. Implement a while loop driven by a T1 condition and terminate it using TCMP-based comparisons.",
    signature: "fn ternary_log3(n: t40) -> t40",
    template: "// T031 — Ternary Integer Logarithm\nimport ulib;\n\nfn ternary_log3(n: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T032",
    title: "Base-27 Formatter",
    difficulty: "easy",
    category: "arithmetic",
    tags: ["base-27","itoa"],
    description: "Format a t40 value as its base-27 representation with a 0z prefix. Requires extracting digits of base-27 (using base-3 groupings of 3 trits) and outputting characters.",
    signature: "fn format_base27(n: t40, out_str: borrow_mut<ptr<t40, user, valid>>) -> t40",
    template: "// T032 — Base-27 Formatter\nimport ulib;\n\nfn format_base27(n: t40, out_str: borrow_mut<ptr<t40, user, valid>>) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T033",
    title: "Fixed-Point Multiply",
    difficulty: "medium",
    category: "arithmetic",
    tags: ["Q13.13","multiply"],
    description: "Multiply two Q13.13 ternary fixed-point values (13 integer trits, 13 fractional trits packed in a t40 word). Handles sign propagation, wide multiplication, and shifting.",
    signature: "fn q_multiply(a: t40, b: t40) -> t40",
    template: "// T033 — Fixed-Point Multiply\nimport ulib;\n\nfn q_multiply(a: t40, b: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T034",
    title: "Multi-Precision Add",
    difficulty: "easy",
    category: "arithmetic",
    tags: ["bigint","carry"],
    description: "Add two big integers represented as arrays of t40 limbs, propagating carries sequentially. This serves as a building block for multi-precision cryptographic math.",
    signature: "fn bigint_add(a: borrow<[t40]>, b: borrow<[t40]>, out: borrow_mut<[t40]>) -> t40",
    template: "// T034 — Multi-Precision Add\nimport ulib;\n\nfn bigint_add(a: borrow<[t40]>, b: borrow<[t40]>, out: borrow_mut<[t40]>) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T035",
    title: "TLDR/TSTR Spinlock",
    difficulty: "medium",
    category: "concurrent",
    tags: ["tldr","tstr","spinlock"],
    description: "Implement lock() and unlock() using TLDR/TSTR atomic primitives. Lock state uses T1 values (neg = locked, pos = free). Avoid busy-spinning without a FENCE instruction.",
    signature: "fn spin_lock(lock_ptr: ptr<t1, shared, valid>) -> t40",
    template: "// T035 — TLDR/TSTR Spinlock\nimport ulib;\n\nfn spin_lock(lock_ptr: ptr<t1, shared, valid>) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T036",
    title: "Three-Phase Barrier",
    difficulty: "hard",
    category: "concurrent",
    tags: ["atomic","barrier"],
    description: "Synchronize N concurrent threads using atomic decrement (via TLDR/TSTR). Threads pass through approach, wait, and release phases, which map directly onto the three ternary state values.",
    signature: "fn barrier_wait(counter: ptr<t40, shared, valid>, num_threads: t40) -> t40",
    template: "// T036 — Three-Phase Barrier\nimport ulib;\n\nfn barrier_wait(counter: ptr<t40, shared, valid>, num_threads: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T037",
    title: "Epoch-Based Reclamation",
    difficulty: "hard",
    category: "concurrent",
    tags: ["epoch","memory-reclamation"],
    description: "Safely reclaim concurrent memory without garbage collection. Threads publish their epoch status in a shared<t40, ACQ_REL> slot. Reclaim buffers only after all threads have moved past the epoch.",
    signature: "fn try_reclaim(epochs: borrow<[t40]>, reclaim_epoch: t40) -> t40",
    template: "// T037 — Epoch-Based Reclamation\nimport ulib;\n\nfn try_reclaim(epochs: borrow<[t40]>, reclaim_epoch: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T038",
    title: "Three-Way Radix Sort",
    difficulty: "medium",
    category: "dsa",
    tags: ["msd-radix","sorting"],
    description: "Implement a most-significant-digit (MSD) radix sort that partitions elements by trits. Enables branchless, in-place sorting at the digit level into neg, zero, and pos buckets.",
    signature: "fn radix_sort(arr: borrow_mut<[t40]>, digit_index: t40) -> t40",
    template: "// T038 — Three-Way Radix Sort\nimport ulib;\n\nfn radix_sort(arr: borrow_mut<[t40]>, digit_index: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T039",
    title: "Ternary Huffman Coding",
    difficulty: "hard",
    category: "dsa",
    tags: ["huffman","tree"],
    description: "Build an optimal prefix tree with up to 3 children per node. Satisfies the ternary tree leaf property ((n-1) mod 2 = 0), padding with dummy leaf nodes when required.",
    signature: "fn build_huffman_tree(freqs: borrow<[t40]>) -> t40",
    template: "// T039 — Ternary Huffman Coding\nimport ulib;\n\nfn build_huffman_tree(freqs: borrow<[t40]>) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T040",
    title: "TST Delete",
    difficulty: "medium",
    category: "dsa",
    tags: ["TST","deletion"],
    description: "Delete a key from a Ternary Search Tree without corrupting prefix matches. Requires recursive safety checks and updating pointer states during node teardown.",
    signature: "fn tst_delete(root: ptr<t40, user, unknown>, key: borrow<ptr<t40, user, valid>>) -> ptr<t40, user, unknown>",
    template: "// T040 — TST Delete\nimport ulib;\n\nfn tst_delete(root: ptr<t40, user, unknown>, key: borrow<ptr<t40, user, valid>>) -> ptr<t40, user, unknown> {\n    // your solution here\n    return root;\n}"
  },
  {
    id: "T041",
    title: "Ternary Skip List",
    difficulty: "hard",
    category: "dsa",
    tags: ["skip-list","probabilistic"],
    description: "Implement a skip list where each node has a random height. Use TCMP on a random t40 word to determine the level selection probabilistically, achieving O(log₃ n) lookup.",
    signature: "fn skip_list_search(head: ptr<t40, user, valid>, target: t40) -> t40",
    template: "// T041 — Ternary Skip List\nimport ulib;\n\nfn skip_list_search(head: ptr<t40, user, valid>, target: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T042",
    title: "Interval Tree Query",
    difficulty: "medium",
    category: "dsa",
    tags: ["interval-tree","BST"],
    description: "Store intervals [lo, hi] and query for overlaps. Augment a BST with a max-endpoint field, driving search routing via three-way TCMP comparisons.",
    signature: "fn interval_search(root: ptr<t40, user, unknown>, q_lo: t40, q_hi: t40) -> t40",
    template: "// T042 — Interval Tree Query\nimport ulib;\n\nfn interval_search(root: ptr<t40, user, unknown>, q_lo: t40, q_hi: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T043",
    title: "Constant-Time Select",
    difficulty: "easy",
    category: "crypto",
    tags: ["TSEL","branch-free"],
    description: "Return a if condition is pos, b if neg, or c if zero, executing in constant time with no branches. Uses the TSEL hardware instruction directly.",
    signature: "fn ct_select(cond: t1, a: t40, b: t40, c: t40) -> t40",
    template: "// T043 — Constant-Time Select\nimport ulib;\n\nfn ct_select(cond: t1, a: t40, b: t40, c: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T044",
    title: "Hensel Lifting Step",
    difficulty: "hard",
    category: "crypto",
    tags: ["hensel-lifting","newton-step"],
    description: "Given N and x₀ such that N * x₀ ≡ 1 (mod 3), perform one Newton-Raphson step to double the correct digits. Serves as the core loop of Montgomery reduction parameter setup.",
    signature: "fn hensel_step(n: t40, x0: t40) -> t40",
    template: "// T044 — Hensel Lifting Step\nimport ulib;\n\nfn hensel_step(n: t40, x0: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T045",
    title: "Ternary Feistel Round",
    difficulty: "medium",
    category: "crypto",
    tags: ["feistel","sub-word"],
    description: "Perform one Feistel cipher round. Split a t40 word into two t20 halves, evaluate the round function on one half, and add it mod 3 (tritwise XOR) to the other, then swap halves.",
    signature: "fn feistel_round(word: t40, key: t40) -> t40",
    template: "// T045 — Ternary Feistel Round\nimport ulib;\n\nfn feistel_round(word: t40, key: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T046",
    title: "Branch-Free Clamp",
    difficulty: "easy",
    category: "isa-optimization",
    tags: ["TSEL","TCMP","optimization"],
    description: "Clamp a value x to the interval [lo, hi] without using conditional jump instructions (BRN, BRZ, BRP). Optimize cycle count by exploiting TCMP and TSEL.",
    signature: "fn clamp_branchfree(x: t40, lo: t40, hi: t40) -> t40",
    template: "// T046 — Branch-Free Clamp\nimport ulib;\n\nfn clamp_branchfree(x: t40, lo: t40, hi: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T047",
    title: "Sub-Word Pack / Unpack",
    difficulty: "easy",
    category: "isa-optimization",
    tags: ["trit-shift","masking"],
    description: "Pack three t9 values into a single t40 word and extract them. The reference solution uses 8 instructions; find an optimized formulation using 4.",
    signature: "fn pack_unpack(a: t40, b: t40, c: t40) -> t40",
    template: "// T047 — Sub-Word Pack / Unpack\nimport ulib;\n\nfn pack_unpack(a: t40, b: t40, c: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T048",
    title: "Register Pressure Reduction",
    difficulty: "medium",
    category: "isa-optimization",
    tags: ["register-allocation","stack-spill"],
    description: "Rewrite a complex calculation containing multiple intermediate results to reduce register pressure. Fit the logic within 24 registers with zero stack spills.",
    signature: "fn optimize_registers(a: t40, b: t40, c: t40, d: t40) -> t40",
    template: "// T048 — Register Pressure Reduction\nimport ulib;\n\nfn optimize_registers(a: t40, b: t40, c: t40, d: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T049",
    title: "Strength Reduction",
    difficulty: "medium",
    category: "isa-optimization",
    tags: ["strength-reduction","trit-shift"],
    description: "Replace multiplication operations with equivalent combinations of shifts and additions. E.g., multiply by 9 using base-3 shift operations to speed up execution.",
    signature: "fn multiply_fast(x: t40) -> t40",
    template: "// T049 — Strength Reduction\nimport ulib;\n\nfn multiply_fast(x: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T050",
    title: "Peephole Optimization",
    difficulty: "hard",
    category: "isa-optimization",
    tags: ["peephole","redundancy"],
    description: "Analyze a provided assembly sequence and eliminate redundancies such as consecutive identical loads/stores, identity math, or dead branch conditions.",
    signature: "fn peephole_opt(x: t40) -> t40",
    template: "// T050 — Peephole Optimization\nimport ulib;\n\nfn peephole_opt(x: t40) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T051",
    title: "Generic Dot Product",
    difficulty: "medium",
    category: "width-polymorphism",
    tags: ["VMAC","TritWidth","generic"],
    description: "Write a generic dot product function over arrays of width W. Must lower to hardware VMAC instructions when instantiated at T1, or standard multiply-accumulate otherwise.",
    signature: "fn dot<W: TritWidth>(a: borrow<[T<W>]>, b: borrow<[T<W>]>) -> t40",
    template: "// T051 — Generic Dot Product\nimport ulib;\n\nfn dot<W: TritWidth>(a: borrow<[T<W>]>, b: borrow<[T<W>]>) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T052",
    title: "Width-Parametric Clamp",
    difficulty: "easy",
    category: "width-polymorphism",
    tags: ["TritWidth","generic"],
    description: "Create a generic clamp function for elements of width W. Constrain parameters lo and hi to be the exact same width as the input value x.",
    signature: "fn clamp<W: TritWidth>(x: T<W>, lo: T<W>, hi: T<W>) -> T<W>",
    template: "// T052 — Width-Parametric Clamp\nimport ulib;\n\nfn clamp<W: TritWidth>(x: T<W>, lo: T<W>, hi: T<W>) -> T<W> {\n    // your solution here\n    return x;\n}"
  },
  {
    id: "T053",
    title: "Polymorphic Vec Map",
    difficulty: "medium",
    category: "width-polymorphism",
    tags: ["TritWidth","generic","function-pointer"],
    description: "Map a function f over an array of elements of width W. Note that TCL 1.0 does not support closures; f must be passed as a named function reference.",
    signature: "fn vec_map<W: TritWidth>(v: borrow<[T<W>]>, f: fn(T<W>) -> T<W>, out: borrow_mut<[T<W>]>)",
    template: "// T053 — Polymorphic Vec Map\nimport ulib;\n\nfn vec_map<W: TritWidth>(v: borrow<[T<W>]>, f: fn(T<W>) -> T<W>, out: borrow_mut<[T<W>]>) {\n    // your solution here\n    \n}"
  },
  {
    id: "T054",
    title: "Trinfuck Interpreter",
    difficulty: "medium",
    category: "trinfuck",
    tags: ["esolang","interpreter"],
    description: "Implement an interpreter for Trinfuck (balanced ternary Brainfuck) in TCL 1.0. Program execution details: tape cells span [-9841, +9841], instruction '+' increments, '-' decrements, and ternary branch operators are '[', '{', '}'.",
    signature: "fn interpret(program: borrow<[t40]>) -> t40",
    template: "// T054 — Trinfuck Interpreter\nimport ulib;\n\nfn interpret(program: borrow<[t40]>) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T055",
    title: "Trinfuck to TASM Compiler",
    difficulty: "hard",
    category: "trinfuck",
    tags: ["esolang","meta-programming","compiler"],
    description: "Write a TCL program that compiles Trinfuck code to TASM assembly instructions in memory. Map operations to 1-4 assembly instructions, translating branches to BRZ/BRN/BRP.",
    signature: "fn compile_trinfuck(program: borrow<[t40]>) -> t40",
    template: "// T055 — Trinfuck to TASM Compiler\nimport ulib;\n\nfn compile_trinfuck(program: borrow<[t40]>) -> t40 {\n    // your solution here\n    return 0;\n}"
  },
  {
    id: "T056",
    title: "Recursive Fibonacci",
    difficulty: "easy",
    category: "arithmetic",
    tags: ["T40", "recursion"],
    description: `Compute the Nth Fibonacci number recursively.

Fibonacci sequence:
- \`F(0) = 0\`
- \`F(1) = 1\`
- \`F(2) = 1\`
- \`F(3) = 2\`
- \`F(n) = F(n-1) + F(n-2)\`

### Constraints
- \`0 <= N <= 15\`
- Implement it **recursively** to verify proper register spill and call stack mechanics in the VM!`,
    signature: "fn fibonacci(n: t40) -> t40",
    template: `// T056 — Recursive Fibonacci
// Compute the Nth Fibonacci number recursively.
// F(0) = 0, F(1) = 1, F(n) = F(n-1) + F(n-2)
// Implement it recursively to verify call stack mechanics in the VM.

import ulib;

fn fibonacci(n: t40) -> t40 {
    // Write your recursive code here
    
    return 0;
}`,
    testCases: [
      { arg: 0, expectedR13: 0 },
      { arg: 1, expectedR13: 1 },
      { arg: 5, expectedR13: 5 },
      { arg: 10, expectedR13: 55 }
    ],
    generateWrapper: (code, arg) => `
${code}

fn main() -> t40 {
    var res: t40 = fibonacci(${arg});
    return res;
}
`
  }
];


// Helper to parse VM output
interface ParsedVmResult {
  cycles: number | null;
  status: string | null;
  r13: number | null;
  consoleOutput: string;
  registers: Record<string, number>;
}

function parseVmOutput(stdout: string): ParsedVmResult {
  const cyclesMatch = stdout.match(/Total CPU Cycles:\s*(\d+)/i);
  const cycles = cyclesMatch ? parseInt(cyclesMatch[1], 10) : null;

  const statusMatch = stdout.match(/Final CPU Status:\s*(\S+)/i);
  const status = statusMatch ? statusMatch[1] : null;

  const r13Match = stdout.match(/Return Register r13:\s*(-?\d+)/i);
  const r13 = r13Match ? parseInt(r13Match[1], 10) : null;

  // Extract console output
  let consoleOutput = "";
  const consoleHeaderIndex = stdout.indexOf("Console Output:");
  if (consoleHeaderIndex !== -1) {
    const afterHeader = stdout.substring(consoleHeaderIndex + "Console Output:".length);
    let startIndex = 0;
    while (startIndex < afterHeader.length && (afterHeader[startIndex] === "\r" || afterHeader[startIndex] === "\n")) {
      startIndex++;
    }
    let endIndex = afterHeader.length;
    const returnRegIndex = afterHeader.indexOf("Return Register");
    if (returnRegIndex !== -1) {
      endIndex = returnRegIndex;
    } else {
      const dividerIndex = afterHeader.indexOf("====");
      if (dividerIndex !== -1) {
        endIndex = dividerIndex;
      }
    }
    consoleOutput = afterHeader.substring(startIndex, endIndex);
    consoleOutput = consoleOutput.replace(/[\s\r\n]+$/, "");
  }

  // Extract registers
  const regMatches = stdout.matchAll(/^\s*r(\d+)(?:\s*\(sp\))?:\s*(-?\d+)/gm);
  const registers: Record<string, number> = {};
  for (const match of regMatches) {
    registers[`r${match[1]}`] = parseInt(match[2], 10);
  }

  return {
    cycles,
    status,
    r13,
    consoleOutput,
    registers
  };
}

// Clean up temporary files
function cleanupFiles(filePrefix: string) {
  const exts = [".trit", ".tasm", ".txe"];
  exts.forEach(ext => {
    const p = filePrefix + ext;
    if (fs.existsSync(p)) {
      try {
        fs.unlinkSync(p);
      } catch (e) {
        // Ignore error
      }
    }
  });
}

interface CompileResult {
  success: boolean;
  compileTimeCycles?: number;
  assembly?: string;
  cycles?: number | null;
  status?: string | null;
  r13?: number | null;
  consoleOutput?: string;
  registers?: Record<string, number>;
  compilerOutput: string;
  error?: string;
}

// Compiler execution engine
async function compileAndRunTrit(
  code: string,
  engine: "native" | "bootstrap",
  optLevel = "-O2"
): Promise<CompileResult> {
  const runId = `run_${Date.now()}_${Math.random().toString(36).substring(2, 7)}`;
  const rootDir = path.resolve(__dirname, "..");
  const tempPrefix = path.join(__dirname, runId);
  const tempTrit = `${tempPrefix}.trit`;
  const tempTasm = `${tempPrefix}.tasm`;
  const tempTxe = `${tempPrefix}.txe`;

  // Read mini stdlib
  const ulibMini = fs.readFileSync(path.join(rootDir, "ulib_mini.trit"), "utf-8");

  // Sanitize user code: strip import statements and replace "pub fn" with "fn"
  const sanitizedCode = code
    .replace(/^\s*import\s+\w+\s*;/gm, "// import statement removed")
    .replace(/\bpub\s+fn\b/g, "fn");

  // Combine ulib_mini and user wrapper code
  const fullCode = `${ulibMini}\n${sanitizedCode}`;
  fs.writeFileSync(tempTrit, fullCode, "utf-8");

  const relativeTrit = path.relative(rootDir, tempTrit);
  const relativeTasm = path.relative(rootDir, tempTasm);
  const relativeTxe = path.relative(rootDir, tempTxe);

  let compileTimeCycles = 0;
  let compilerOutput = "";
  let assemblyText = "";
  let runResult: ParsedVmResult | null = null;
  const getRunResult = () => runResult;
  let errorMsg = "";

  try {
    if (engine === "bootstrap") {
      // 1. Compile directly with tritc.exe
      const compileCmd = `build\\tritc.exe ${relativeTrit} -o ${relativeTxe} ${optLevel}`;
      await new Promise<void>((resolve, reject) => {
        exec(compileCmd, { cwd: rootDir, timeout: 30000 }, (err, stdout, stderr) => {
          compilerOutput += stdout + stderr;
          if (err) {
            reject(new Error(`Compilation Failed:\n${stdout}\n${stderr}`));
          } else {
            resolve();
          }
        });
      });

      // Also get the assembly for display (compile with -S)
      await new Promise<void>((resolve) => {
        const asmCmd = `build\\tritc.exe ${relativeTrit} -S ${optLevel}`;
        exec(asmCmd, { cwd: rootDir, timeout: 15000 }, (err, stdout, stderr) => {
          if (!err) {
            const generatedAsm = tempTrit.replace(".trit", ".tasm");
            if (fs.existsSync(generatedAsm)) {
              assemblyText = fs.readFileSync(generatedAsm, "utf-8");
              try {
                fs.unlinkSync(generatedAsm);
              } catch (e) {}
            }
          }
          resolve();
        });
      });
    } else {
      // 2. Compile via the Native VM Compiler loop
      const nativeCompileCmd = `build\\tritc.exe run treatcode\\compiler_driver.txe --imem 262144 --dmem 16777216 --seed-file ${relativeTrit} 1600000 --steps 200000000 --no-ansi`;

      let vmCompileStdout = "";
      await new Promise<void>((resolve, reject) => {
        exec(nativeCompileCmd, { cwd: rootDir, timeout: 90000 }, (err, stdout, stderr) => {
          vmCompileStdout = stdout;
          if (err) {
            reject(new Error(`Native Compilation VM Execution failed:\n${stdout}\n${stderr}`));
          } else {
            resolve();
          }
        });
      });

      const parsedCompiler = parseVmOutput(vmCompileStdout);
      compileTimeCycles = parsedCompiler.cycles || 0;
      compilerOutput = vmCompileStdout;

      // The console output contains the raw assembly code
      assemblyText = parsedCompiler.consoleOutput || "";

      if (!assemblyText.includes("_start") && !assemblyText.includes("RET")) {
        throw new Error(
          `Native compiler failed to generate valid assembly. Compiler Output:\n${parsedCompiler.consoleOutput}`
        );
      }

      // Write assembly to temp .tasm file
      fs.writeFileSync(tempTasm, assemblyText, "utf-8");

      // Assemble using tritc assembler
      const assembleCmd = `build\\tritc.exe ${relativeTasm} -o ${relativeTxe}`;
      await new Promise<void>((resolve, reject) => {
        exec(assembleCmd, { cwd: rootDir, timeout: 15000 }, (err, stdout, stderr) => {
          if (err) {
            reject(new Error(`Assembly of native compiler output failed:\n${stdout}\n${stderr}`));
          } else {
            resolve();
          }
        });
      });
    }

    // Run the assembled .txe binary inside VM
    const runCmd = `build\\tritc.exe run ${relativeTxe} --steps 1000000 --no-ansi --dump-registers`;
    await new Promise<void>((resolve, reject) => {
      exec(runCmd, { cwd: rootDir, timeout: 25000 }, (err, stdout, stderr) => {
        if (err) {
          reject(new Error(`VM Run Failed:\n${stdout}\n${stderr}`));
        } else {
          runResult = parseVmOutput(stdout);
          resolve();
        }
      });
    });
  } catch (err: any) {
    errorMsg = err.message;
  } finally {
    cleanupFiles(tempPrefix);
  }

  if (errorMsg) {
    return {
      success: false,
      error: errorMsg,
      compilerOutput
    };
  }

  return {
    success: true,
    compileTimeCycles,
    assembly: assemblyText,
    cycles: getRunResult()?.cycles,
    status: getRunResult()?.status,
    r13: getRunResult()?.r13,
    consoleOutput: getRunResult()?.consoleOutput,
    registers: getRunResult()?.registers || {},
    compilerOutput
  };
}

// REST Routes
app.get("/api/problems", (req: Request, res: Response) => {
  // Strip test cases before sending to client
  const clientProblems = problems.map(p => ({
    id: p.id,
    title: p.title,
    difficulty: p.difficulty,
    category: p.category,
    tags: p.tags,
    description: p.description,
    signature: p.signature,
    template: p.template
  }));
  res.json(clientProblems);
});

app.get("/api/leaderboard", (req: Request, res: Response) => {
  res.json(leaderboard);
});

app.post("/api/run", async (req: Request, res: Response) => {
  const { problemId, code, engine, optLevel } = req.body;
  const problem = problems.find(p => p.id === problemId);
  if (!problem) {
    return res.status(404).json({ error: "Problem not found" });
  }

  // Handle Compile Check Only Mode problems
  if (!problem.testCases || problem.testCases.length === 0) {
    const result = await compileAndRunTrit(code, engine, optLevel);
    if (!result.success) {
      return res.json(result);
    }
    return res.json({
      ...result,
      consoleOutput: "Compilation Successful! VM validation tests for this challenge are coming soon."
    });
  }

  // Get first test case for manual run
  const testCase = problem.testCases[0];
  const wrappedCode = problem.generateWrapper!(code, testCase.arg);

  const result = await compileAndRunTrit(wrappedCode, engine, optLevel);
  res.json(result);
});

app.post("/api/submit", async (req: Request, res: Response) => {
  const { problemId, code, engine, username, optLevel } = req.body;
  const problem = problems.find(p => p.id === problemId);
  if (!problem) {
    return res.status(404).json({ error: "Problem not found" });
  }

  if (!username || username.trim() === "") {
    return res.status(400).json({ error: "Username is required for submission" });
  }

  // Handle Compile Check Only Mode problems
  if (!problem.testCases || problem.testCases.length === 0) {
    const runResult = await compileAndRunTrit(code, engine, optLevel);
    if (!runResult.success) {
      return res.json({
        success: false,
        error: runResult.error,
        results: [
          {
            testCase: 1,
            passed: false,
            error: runResult.error,
            compilerOutput: runResult.compilerOutput
          }
        ]
      });
    }

    // Insert compiler cycle count into leaderboard
    const newEntry: LeaderboardEntry = {
      id: leaderboard.length + 1,
      problemId,
      name: username.substring(0, 20),
      engine,
      cycles: runResult.compileTimeCycles || 1, // just standard positive number
      date: new Date().toISOString().split("T")[0]
    };
    leaderboard.push(newEntry);
    leaderboard.sort((a, b) => a.cycles - b.cycles);

    return res.json({
      success: true,
      results: [
        {
          testCase: 1,
          passed: true,
          cycles: runResult.compileTimeCycles || 0,
          consoleOutput: "Compilation Successful! VM validation tests for this challenge are coming soon.",
          r13: 0,
          registers: runResult.registers || {}
        }
      ]
    });
  }

  const results = [];
  let allPassed = true;
  let totalCycles = 0;

  for (let i = 0; i < problem.testCases.length; i++) {
    const tc = problem.testCases[i];
    const wrappedCode = problem.generateWrapper!(code, tc.arg);
    const runResult = await compileAndRunTrit(wrappedCode, engine, optLevel);

    if (!runResult.success) {
      results.push({
        testCase: i + 1,
        passed: false,
        error: runResult.error,
        compilerOutput: runResult.compilerOutput
      });
      allPassed = false;
      continue;
    }

    let passed = false;
    if (tc.expectedOutput !== undefined) {
      const actual = (runResult.consoleOutput || "").replace(/\r\n/g, "\n").trim();
      const expected = tc.expectedOutput.replace(/\r\n/g, "\n").trim();
      passed = actual === expected;
    } else if (tc.expectedR13 !== undefined) {
      passed = runResult.r13 === tc.expectedR13;
    }

    results.push({
      testCase: i + 1,
      passed,
      cycles: runResult.cycles,
      consoleOutput: runResult.consoleOutput,
      r13: runResult.r13,
      registers: runResult.registers
    });

    if (!passed) allPassed = false;
    totalCycles += runResult.cycles || 0;
  }

  // If all tests passed, insert into leaderboard
  if (allPassed) {
    const avgCycles = Math.round(totalCycles / problem.testCases.length);
    const newEntry: LeaderboardEntry = {
      id: leaderboard.length + 1,
      problemId,
      name: username.substring(0, 20),
      engine,
      cycles: avgCycles,
      date: new Date().toISOString().split("T")[0]
    };
    leaderboard.push(newEntry);
    leaderboard.sort((a, b) => a.cycles - b.cycles);
  }

  res.json({
    success: allPassed,
    results
  });
});

// Fallback to serving the built frontend app for all other routes
app.get("*", (req: Request, res: Response) => {
  const indexHtmlPath = path.join(distPath, "index.html");
  if (fs.existsSync(indexHtmlPath)) {
    res.sendFile(indexHtmlPath);
  } else {
    res.status(404).send("API endpoint or frontend assets not found");
  }
});

app.listen(PORT, () => {
  console.log(`TreatCode Server running at http://localhost:${PORT}`);
});
