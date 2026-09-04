import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptRoot = path.dirname(fileURLToPath(import.meta.url));
const appRoot = path.resolve(scriptRoot, "..");
const repoRoot = path.resolve(appRoot, "..");
const contentRoot = path.join(appRoot, "src", "content", "learn");
const routeRoot = path.join(appRoot, "learn");
const registryPath = path.join(appRoot, "public", "api", "v1", "stack_nodes.json");
const testsRegistryPath = path.join(appRoot, "public", "api", "v1", "tests.json");
const benchmarksRegistryPath = path.join(appRoot, "public", "api", "v1", "benchmarks.json");
const gapsRegistryPath = path.join(appRoot, "public", "api", "v1", "gaps.json");
const snapshotPath = path.join(appRoot, "public", "api", "v1", "snapshot.json");

const readJson = (filePath) => JSON.parse(fs.readFileSync(filePath, "utf8"));
const writeJson = (filePath, value) => {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  fs.writeFileSync(filePath, `${JSON.stringify(value, null, 2)}\n`, "utf8");
};
const repoRelative = (filePath) => path.relative(repoRoot, filePath).replaceAll(path.sep, "/");
const unique = (items) => [...new Set(items)];

const writeSource = process.argv.includes("--write-source");

function readCanonicalPage(filePath) {
  const filename = path.basename(filePath);
  const source = fs.readFileSync(filePath, "utf8");
  const match = source.match(/^---\r?\n([\s\S]*?)\r?\n---\r?\n([\s\S]*)$/);
  if (!match) throw new Error(`Learning source ${repoRelative(filePath)} is missing JSON front matter`);
  let metadata;
  try {
    metadata = JSON.parse(match[1]);
  } catch (error) {
    throw new Error(`Learning source ${repoRelative(filePath)} has invalid JSON front matter: ${String(error)}`);
  }
  if (!metadata || typeof metadata.id !== "string" || !metadata.id) {
    throw new Error(`Learning source ${repoRelative(filePath)} has no lesson id`);
  }
  return { ...metadata, content: match[2].trim(), filename };
}

function loadCanonicalPages() {
  if (!fs.existsSync(contentRoot)) throw new Error(`Learning source directory is missing: ${repoRelative(contentRoot)}`);
  const pages = new Map();
  for (const entry of fs.readdirSync(contentRoot, { withFileTypes: true })) {
    if (!entry.isFile() || !entry.name.endsWith(".md")) continue;
    const filePath = path.join(contentRoot, entry.name);
    const page = readCanonicalPage(filePath);
    if (pages.has(page.id)) throw new Error(`Duplicate learning source id ${page.id}`);
    pages.set(page.id, page);
  }
  return pages;
}

// These are the editorial contracts for the current registry. The generator
// refuses to publish if the registry contains a phase without a corresponding
// contract, which makes the matrix a derived inventory instead of a sample.
const PHASE_DEFINITIONS = [
  {
    slug: "authority",
    focus: "source of truth, reproducible builds, tests, evidence, and safe reading of project claims",
    entry: "a repository claim, a manifest row, or a test result",
    exit: "a bounded, commit-addressed statement that another learner can reproduce",
    concept: "Systems work is easier to trust when a claim names the artifact that owns it, the transformation that produced it, and the test that can falsify it. A README can explain intent, but a manifest, source span, generated index, or test result carries a different kind of authority. Reproducibility is the habit of recording enough context that a second reader can repeat the same check rather than relying on memory or an attractive demo.",
    implementation: "TreatCode keeps these distinctions in AGENTS.md, TEST_MANIFEST.json, ROADMAP_STATUS.json, and tools/trit_tool.py. The public snapshot also carries a repository, commit, generation time, and source label. The stack registry points each phase at source references, test IDs, benchmarks, and gaps. A green command is evidence for the command's contract; it is not permission to claim that an unrelated future feature exists.",
    planned: "Benchmark baselines remain a recorded gap, so a lesson may teach how to compare a baseline without inventing a performance number. The safe label for unavailable evidence is not_available or missing, and the Learn route carries that label through to the Stack Explorer.",
    worked: "Suppose a learner reads that a release image can boot. The defensible trail starts with IMAGE_FORMAT_MANIFEST.json for the format, build_tos_image.cpp for construction, a release test for the observed behavior, and a snapshot commit for freshness. If one link is absent, the conclusion becomes partial rather than complete. The same method applies to a compiler claim, a kernel claim, and a benchmark claim.",
    investigation: "Run knowledge status, inspect the relevant plan entry, and compare the phase row with its source and test IDs. Then deliberately change the query to a nonexistent record and observe that the search reports no result instead of silently choosing a nearby file. This is a small but important lesson in negative evidence: not finding a record is different from proving that an implementation is absent.",
    misconception: "A passing test does not certify the entire dependency stack. It certifies the assertions and inputs named by that test at a particular source state. A source link without a test is a design pointer, while a test name without a readable source or result is an incomplete trail.",
    action: "Write a three-column note for one stack phase: claim, authoritative artifact, falsifying check. Include the snapshot commit and mark every planned or unavailable item explicitly. Your note is finished only when a reader who did not watch your work can follow the same path.",
    terms: ["source of truth", "reproducible build", "evidence record", "coverage matrix", "provenance"],
    termNotes: {
      "source of truth": "the artifact whose value controls a contract; an explanatory note can point to it but cannot replace it",
      "reproducible build": "a build procedure whose inputs, toolchain, and source state are fixed well enough to compare outputs",
      "evidence record": "a durable result that names what was checked, where it came from, and whether it passed",
      "coverage matrix": "a generated mapping from inventory records to the lessons and checks that expose them",
      "provenance": "the repository, commit, path, and role that allow a claim to be traced back to its origin",
    },
    lessons: [
      { id: "authority-claims", title: "Authority and safe claims", kind: "mental-model", interaction: "choice" },
      { id: "authority-build-evidence", title: "Build an evidence trail", kind: "build-trace", interaction: "source" },
    ],
  },
  {
    slug: "representation",
    focus: "trits, ternary encodings, balanced and unbalanced choices, gates, dual-rail realization, timing, and the hardware boundary",
    entry: "a logical trit or a packed word crossing a representation boundary",
    exit: "a value whose numeric, lane, and physical meanings are not confused",
    concept: "A representation is a promise about meaning and layout at the same time. Balanced ternary uses digits -1, 0, and +1 for arithmetic; an unbalanced encoding may use 0, 1, and 2 for storage or transport. A gate is a relation over states, while a circuit realization chooses signals, timing, and error behavior. Keeping these layers separate lets a compiler use a convenient host container without pretending that two host bits are themselves a physical ternary device.",
    implementation: "Trit headers define scalar and lane operations, while the logic-level documentation describes dual-rail realization. The packed lane contract uses two bits per trit with 00 for -1, 01 for 0, 10 for +1, and 11 invalid. Numeric widths such as T40 describe positional arithmetic; lane widths such as l40 describe packed tritwise transport. Native operation and lane tests exercise conversions and boundary behavior.",
    planned: "The repository records the hardware-facing contract and simulation, but a software header is not a manufactured silicon timing proof. Any physical gate, fabrication target, or accelerator claim must remain tied to the evidence status in the registry.",
    worked: "Encode the balanced value sequence [-1, 0, +1] as lanes. The transport payload is 00 01 10, but the numeric sum is -1 + 0 + 1 = 0. Treating 000110 as a binary integer and adding one to it would answer a different question. The correct workflow decodes each lane, applies the trit operation, and re-encodes the result.",
    investigation: "Open ternary_scalar.h beside ternary_lanes.h and trace one conversion in the lane tests. Record which function interprets a word numerically and which function preserves lane positions. Then inspect the hardware contract page for timing language; that is where an abstract truth table stops and a signal-level claim begins.",
    misconception: "Balanced ternary is not the same thing as a two-bit lane encoding. The former describes numeric digit values; the latter is a transport convention with an invalid code. A trit enum can name a state without being safe to pass to an arithmetic routine.",
    action: "Make a table for five values containing the balanced digit, numeric value, lane bits, and physical signal pair. Mark invalid lane code 11 and explain why it must not silently become +1. Use the table to predict a conversion test before running it.",
    terms: ["trit", "balanced ternary", "lane representation", "dual rail", "invalid sentinel"],
    termNotes: {
      trit: "one logical ternary digit with numeric states -1, 0, and +1 in the balanced model",
      "balanced ternary": "a positional number system whose digit set is negative, neutral, and positive",
      "lane representation": "a position-preserving packed encoding for tritwise or vector operations, not scalar arithmetic",
      "dual rail": "a physical or logical pair of signals used to distinguish ternary states and detect an invalid pair",
      "invalid sentinel": "a deliberately reserved encoding that reports malformed state instead of being treated as data",
    },
    lessons: [
      { id: "representation-boundaries", title: "Representation boundaries", kind: "mental-model", interaction: "choice" },
      { id: "hardware-gates", title: "From gates to hardware evidence", kind: "build-trace", interaction: "trace" },
    ],
  },
  {
    slug: "isa-binary",
    focus: "instruction shape, opcodes, register state, privilege, CSRs, trap causes and entry, calling convention, and ABI boundaries",
    entry: "an encoded instruction and the architectural state before execution",
    exit: "a decoded operation whose effects and privilege checks are explicit",
    concept: "An instruction set architecture is the boundary between software expectations and machine state. It defines not only arithmetic opcodes but also registers, program-counter movement, privilege, control/status registers, traps, and the calling convention used at an ABI boundary. Encoding is a separate concern: a decoder must know how bits or trits become fields before an executor can interpret those fields.",
    implementation: "The Trit ISA and architecture manifests describe the instruction word and ABI. The VM and compiler share register and CSR names, and tests exercise encoding, decode, privilege, and trap behavior. Syscall arguments and returns are documented as register and CSR conventions; wrappers are useful because they preserve those conventions for user code.",
    planned: "A complete hardware privilege implementation may be broader than the current host model. The lesson therefore separates architectural rules from the exact runtime surface that is currently tested.",
    worked: "For a call that returns one value, imagine the caller placing an argument in r13, setting the call target, and observing the return in r13. If the same numeric payload is placed in a CSR field, that is not automatically an argument: the field's role comes from the ABI. A malformed opcode should produce a trap record and a defined PC or cause update rather than accidentally executing a nearby instruction.",
    investigation: "Compare ternary_isa.h, ARCHITECTURE_MANIFEST.json, and the encoding documentation. Pick one opcode, write down its fields, then locate the test that rejects an invalid field. Follow the trap cause into the VM or kernel entry point and note where privilege changes are checked.",
    misconception: "An ABI is not just a function signature, and a register name is not self-explanatory. The ABI includes where arguments, returns, service IDs, and failure details live. A bitwise copy that preserves a payload can still violate the ABI if it crosses the wrong field.",
    action: "Draw the before/decode/after state for one instruction: PC, opcode, source registers, destination register, and trap cause. Repeat it for an illegal encoding. Explain which parts are ISA guarantees and which parts are implementation evidence.",
    terms: ["opcode", "register file", "privilege", "CSR", "ABI"],
    termNotes: {
      opcode: "the operation selector in an encoded instruction; it is not the whole instruction word",
      "register file": "the architecturally visible set of named registers and their state",
      privilege: "the execution mode that controls which instructions, memory, and services are permitted",
      CSR: "a control and status register carrying machine, trap, MMU, device, or syscall state",
      ABI: "the stable calling and data-layout contract between independently compiled layers",
    },
    lessons: [
      { id: "isa-vm-execution", title: "ISA and VM execution", kind: "mental-model", interaction: "choice" },
      { id: "isa-trap-abi-trace", title: "Decode, traps, and ABI evidence", kind: "build-trace", interaction: "source" },
    ],
  },
  {
    slug: "numeric-operations",
    focus: "numeric versus lane representation, arithmetic semantics, vector and SIMD behavior, and accelerator limits",
    entry: "scalar values, packed lanes, or a vector operation request",
    exit: "an arithmetic result with width, representation, and overflow semantics recorded",
    concept: "Numeric computation asks what a value means; lane computation asks how corresponding positions interact. SIMD can accelerate independent lanes, but it does not erase the distinction between a packed transport word and a scalar number. Width, sign, carry, saturation, and invalid-code policy all need an explicit contract before a fast path can be trusted.",
    implementation: "The scalar and native-operations headers define positional arithmetic and ternary operators. SIMD and lane headers provide packed transformations, while workload tests compare expected behavior. Accelerator support is optional and is validated only when its toolchain is configured; the CPU path remains the reference for correctness.",
    planned: "Some accelerator and external workload assets are represented as capabilities or gaps rather than shipped execution. A benchmark result without the target profile, representation, and correctness gate is not a product claim.",
    worked: "Take two lanes encoded as [-1, +1, 0] and [0, +1, -1]. A lane-wise minimum yields [-1, +1, -1]. Interpreting both packed payloads as base-three scalars and minimizing the aggregate numbers would discard the lane positions and produce a different result. The exercise is to name the operation before choosing the implementation.",
    investigation: "Run the native-ops and lane tests, then read the SIMD helper that maps a lane code to a ternary value. Check the benchmark record for which backend and width it measures. If an accelerator is unavailable, record that limitation and compare the CPU oracle instead of substituting a fabricated timing.",
    misconception: "More bits per host word do not automatically mean more numeric range. A 40-trit scalar width and a 40-lane packed width can occupy different layouts and obey different operators. SIMD is an execution strategy, not a new mathematical meaning.",
    action: "Classify six operations as scalar, lane-wise, reduction, or accelerator-specific. For two of them, write the expected result by hand and identify the exact test or oracle that should falsify your answer.",
    terms: ["numeric representation", "lane", "SIMD", "saturation", "accelerator"],
    termNotes: {
      "numeric representation": "the positional interpretation used for scalar arithmetic and width-aware values",
      lane: "one independent position in a packed vector; it is not a digit position in a scalar sum",
      SIMD: "single-instruction, multiple-data execution over independent lanes",
      saturation: "an overflow policy that clamps a result to a permitted endpoint instead of wrapping",
      accelerator: "an optional execution device whose speed claim still depends on the same correctness contract",
    },
    lessons: [
      { id: "tritwise-operations", title: "Tritwise operations", kind: "mental-model", interaction: "choice" },
      { id: "simd-accelerator-evidence", title: "SIMD and accelerator evidence", kind: "build-trace", interaction: "code" },
    ],
  },
  {
    slug: "vm-state",
    focus: "PC and register state, memory and addressing, stack behavior, fetch state, vectors, faults, and architectural invariants",
    entry: "a VM snapshot and a fetch address",
    exit: "a next state that preserves the VM invariants or reports a defined fault",
    concept: "A virtual machine is a state transition system. Its meaning lives in the relation between program counter, registers, memory, stack, vector tables, and fault state. A useful state model makes illegal addresses, misaligned words, and register-width errors observable. It also gives tests a stable vocabulary: instead of saying that a program 'seems to work', a test can assert the exact next PC and memory cell.",
    implementation: "ternary_vm_state.h and the execution-engine documentation describe architectural state and memory model. Multiwidth VM tests cover state at different widths, and architecture tests exercise invariants. The memory model distinguishes address calculations, stored words, and faults so a host pointer is not confused with a guest address.",
    planned: "A full MMU and hardware fault model is taught later as kernel work. The VM lesson uses the state that is implemented and labels any hardware-only assumption as a planned boundary.",
    worked: "Start with PC=12, r13=2, and a stack pointer at 100. A load from guest address 100 reads the guest memory cell at 100; it does not read host byte 100. After a two-trit instruction, the PC advances according to the instruction width unless a branch or fault changes it. A read outside the mapped range must produce a fault state that the caller can inspect.",
    investigation: "Read the VM state struct and trace a multiwidth test from initialization through fetch. Record the state fields that are compared after execution. Then introduce an invalid address in a scratch thought experiment and identify which invariant should fail first.",
    misconception: "The host process's pointer and the guest machine's address are different namespaces. The VM can store a guest address in a host integer, but that does not grant the guest access to host memory. Likewise, the stack pointer is a convention inside the guest state, not the host call stack.",
    action: "Build a four-row state table for fetch, decode, execute, and fault. Include PC, one register, one memory location, and the fault/vector field. Explain which values are architectural and which are implementation details of the host interpreter.",
    terms: ["program counter", "guest address", "architectural state", "stack pointer", "fault vector"],
    termNotes: {
      "program counter": "the architectural address of the next instruction or defined trap target",
      "guest address": "an address interpreted by the VM or guest kernel rather than by the host process directly",
      "architectural state": "the state visible to an instruction or ABI contract and therefore testable at boundaries",
      "stack pointer": "a register or convention naming the current guest stack region",
      "fault vector": "the state or table entry that directs a defined fault or trap response",
    },
    lessons: [
      { id: "vm-state-memory", title: "VM state and memory", kind: "mental-model", interaction: "trace" },
      { id: "vm-memory-trace", title: "Trace a memory transition", kind: "build-trace", interaction: "code" },
    ],
  },
  {
    slug: "vm-execution",
    focus: "fetch, decode, execute, dispatch, tracing and JIT tradeoffs, scheduling, multicore state, and synchronization",
    entry: "a valid VM state and an execution backend choice",
    exit: "a deterministic or explicitly scheduled sequence of state transitions with measured tradeoffs",
    concept: "Execution is a loop around state transitions, but the loop's implementation matters. An interpreter is easy to inspect, a dispatch table reduces branching overhead, a trace JIT specializes hot paths, and multicore execution introduces ownership and synchronization. None of those strategies may change the ISA result. A performance path is credible only when it can be compared with a correctness oracle and a stated workload.",
    implementation: "ternary_vm.h owns the execution surface, while the trace/JIT documentation records decoded traces and native backends. VM tests exercise widths and execution, and the benchmark target records backend comparisons. The deterministic replay gap is kept visible because scheduling and native timing need stronger evidence than a single run.",
    planned: "Deterministic replay and some multicore scheduling guarantees remain gaps. The course teaches how to describe those limits and how to keep synchronization claims narrower than the current evidence.",
    worked: "For a three-instruction loop, the interpreter fetches and decodes every iteration. A trace recorder may capture the hot path after the branch is stable and compile a specialized representation. If the input changes the branch outcome, the JIT must leave the trace or deoptimize; it cannot reuse a trace whose guard no longer holds. Two cores additionally need an ownership rule for the shared memory cell.",
    investigation: "Compare the VM's dispatch entry point with the decoded trace document and the execution-backend benchmark. Note which fields are recorded for a trace and which outputs are used for correctness. Run the width test before reading the timing so a faster but wrong backend cannot pass the lesson.",
    misconception: "A JIT is not a second ISA and a multicore run is not automatically deterministic. JIT code is an implementation of the same architectural transitions. Multicore ordering requires a synchronization contract; a reproducible seed does not by itself prove a reproducible schedule.",
    action: "Annotate a loop with fetch, decode, dispatch, guard, and exit points. Predict where a trace should be invalidated, then name the benchmark and correctness result you would need to trust the optimization.",
    terms: ["interpreter", "dispatch", "trace JIT", "multicore", "synchronization"],
    termNotes: {
      interpreter: "an execution backend that performs architectural transitions directly from decoded instructions",
      dispatch: "the mechanism that selects the implementation for an opcode or decoded operation",
      "trace JIT": "a just-in-time compiler that specializes a frequently observed linear path with guards",
      multicore: "execution with more than one concurrently progressing machine or worker state",
      synchronization: "the ordering or ownership protocol that makes shared state updates safe and understandable",
    },
    lessons: [
      { id: "vm-dispatch-multicore", title: "Interpreter, dispatch, and multicore state", kind: "mental-model", interaction: "trace" },
      { id: "vm-jit-benchmark", title: "Trace JIT and benchmark evidence", kind: "build-trace", interaction: "source" },
    ],
  },
  {
    slug: "assembler",
    focus: "assembly syntax, encoding and decoding, symbols, relocations, object boundaries, and binary compatibility",
    entry: "textual assembly or an object fragment with symbols",
    exit: "a binary artifact whose encoding, relocation, and compatibility rules are explicit",
    concept: "An assembler is a boundary translator, not a spell checker. It maps names and syntax to fields, emits bytes or trits, and records unresolved addresses so a linker can finish the job. A disassembler provides the inverse view, but inverse does not mean lossless: comments, aliases, and symbol names may not survive. Binary compatibility depends on the contract for widths, endianness or packing, relocation records, and error reporting.",
    implementation: "ternary_asm.h, the assembly syntax documentation, and the TCL assembly tests define the current parser and encoder boundary. The next assembler goldens are explicitly referenced work, so a learner can inspect the planned target without calling it shipped. TASCII-81 and symbolic encoding gaps stay separate from core instruction encoding.",
    planned: "Some symbolic encodings and relocation ergonomics are still represented as next tests or gaps. The current assembler contract is narrower than a complete general-purpose object format.",
    worked: "An instruction that calls symbol start cannot know the final address until sections are placed. The assembler emits an opcode plus a relocation entry naming start and the field to patch. The linker resolves start or reports an undefined symbol. If a disassembler prints the patched numeric address without the symbol table, the binary is still executable but the source-level explanation is poorer.",
    investigation: "Take one short TASM fixture and follow it through parse, encode, and test expectation. Locate the relocation or symbol record in the source headers. Then inspect a golden test that fails on an incompatible encoding; classify the failure as syntax, encoding, relocation, or contract drift.",
    misconception: "Assembly text is not the binary contract, and a successful parse does not prove linkability. A symbol may be syntactically valid but unresolved. Conversely, a binary may decode while violating the version or width contract that another loader expects.",
    action: "Create a tiny object map with two sections, one symbol, and one relocation. Predict the unresolved error before linking and identify the source and test record that should witness it.",
    terms: ["assembler", "disassembler", "relocation", "symbol", "binary contract"],
    termNotes: {
      assembler: "the translator from textual assembly and symbolic fields to encoded machine artifacts",
      disassembler: "a decoder that presents an encoded artifact as readable instruction text",
      relocation: "a recorded patch site whose final value depends on linking or loading",
      symbol: "a named address or definition used to connect separately assembled units",
      "binary contract": "the versioned agreement about layout, fields, widths, and compatibility of an artifact",
    },
    lessons: [
      { id: "assembler-binary-contract", title: "Assembler and binary contracts", kind: "mental-model", interaction: "choice" },
      { id: "assembler-relocation-lab", title: "Inspect encoding and relocation", kind: "build-trace", interaction: "source" },
    ],
  },
  {
    slug: "low-level-ir",
    focus: "IR purpose, lowering, object and link stages, symbol resolution, and link failures",
    entry: "typed or parsed operations above machine encoding",
    exit: "a lower-level representation with explicit symbols and link diagnostics",
    concept: "An intermediate representation gives a compiler a stable place to reason before committing to a binary layout. It can expose types, control flow, calling convention, and ownership more clearly than assembly, while still being close enough to lower into instructions. The linking boundary is where independently produced pieces meet; a good IR and object model make a missing symbol or incompatible relocation visible rather than hiding it in generated bytes.",
    implementation: "ternary_ir.h and the compiler-infrastructure documentation describe the current IR shape. The IR tests are the direct evidence for construction and invariants. The linker boundary is intentionally read alongside assembler contracts: lowering can be correct while a later symbol or object error still prevents a usable program.",
    planned: "Some richer optimization and debug metadata may be future work. The current lesson treats IR fields present in source and tests as implemented, and names absent stages as planned rather than implying a full production linker.",
    worked: "Lower an addition function into a block with two input values, an add operation, and a return. The IR names the values before registers exist. Register allocation later maps them to concrete locations, while linking resolves the function symbol. If the return type and call-site ABI disagree, the failure belongs at the interface even if the add operation itself is valid.",
    investigation: "Read the IR struct definitions and one test that verifies a node or operand. Trace where a symbol is carried into generated assembly. Then construct two failure cases: an unknown symbol and a known symbol with the wrong relocation kind. Explain what evidence would distinguish them.",
    misconception: "IR is not an executable substitute for a linked image. It is a compiler contract whose usefulness comes from preserving meaning between stages. A pretty IR dump cannot prove that code generation, object emission, and link resolution all succeed.",
    action: "Write the same three-operation function as source, IR, and a symbolic assembly sketch. Circle the first point where an address becomes concrete and name the test that should cover that transition.",
    terms: ["intermediate representation", "lowering", "basic block", "linker", "symbol resolution"],
    termNotes: {
      "intermediate representation": "a compiler-owned data model that preserves program meaning between front end and machine stages",
      lowering: "the transformation from a richer representation to one closer to the target instruction contract",
      "basic block": "a straight-line sequence with one entry and controlled exits used to model control flow",
      linker: "the stage that combines objects, resolves symbols, and applies relocations",
      "symbol resolution": "matching a use of a named definition to a concrete section or address",
    },
    lessons: [
      { id: "low-level-ir-linking", title: "IR and the linking boundary", kind: "mental-model", interaction: "trace" },
      { id: "link-failure-investigation", title: "Investigate a link failure", kind: "build-trace", interaction: "source" },
    ],
  },
  {
    slug: "tcl-language",
    focus: "syntax, types, control flow, memory and ownership, pointers, errors, and explicit language constraints",
    entry: "a .trit source file and the TCL language contract",
    exit: "a source program whose types, ownership, and failure behavior are explainable",
    concept: "A systems language makes resource and representation choices visible. Syntax is only the surface: types constrain operations, control flow determines which states are reachable, ownership describes who may use memory, and error rules explain how failure crosses a function boundary. A learner should be able to tell which behavior belongs to the language specification and which is a compiler or runtime extension.",
    implementation: "TCL_Spec_1.0.md, the language documentation, and the native lexer/parser define the current front-end contract. Compiler smoke inputs and phase-seven tests provide validation. The first program uses a typed t40 result and an ordinary function body so the learner sees a real source shape rather than a UI-only snippet.",
    planned: "The self-hosted compiler and richer library surface are separate phases. Where pointer safety or ownership analysis is incomplete, the lesson records the constraint and points to the exact test or gap.",
    worked: "Consider a function that returns 1 + 2 as t40. The return type tells the compiler which numeric width to check; the expression is not merely a host-language integer. If a pointer is passed to a buffer helper, the lifetime and bounds rule must be described separately from the function's syntax. A parser success therefore answers only one question in the pipeline.",
    investigation: "Read the TCL specification section for functions and compare it with tcl_parser.trit. Open the compiler smoke input and identify the syntax that is actually exercised. Then mark one language promise that is tested and one promise that is documented but still constrained by a planned runtime feature.",
    misconception: "A valid-looking TCL snippet is not proof that the compiler accepts every related program. The specification, parser, type checker, runtime, and target ABI each own different failure modes. Likewise, a pointer value is not a permission grant to arbitrary memory.",
    action: "Annotate a small TCL function with syntax, type, ownership, runtime, and ABI notes. Change one line to make a type or ownership error and predict the stage that should reject it before running the compiler test.",
    terms: ["TCL", "type contract", "ownership", "pointer", "source error"],
    termNotes: {
      TCL: "the repository's .trit source language and its documented syntax, types, and control-flow contract",
      "type contract": "the set of value and operation rules the compiler must enforce for a source program",
      ownership: "the rule for which layer is responsible for the lifetime and mutation of a resource",
      pointer: "a typed reference into an address space whose validity still depends on bounds and ownership",
      "source error": "a diagnosable language-level failure reported before a program becomes a runnable artifact",
    },
    lessons: [
      { id: "tcl-language-contract", title: "The TCL language contract", kind: "mental-model", interaction: "choice" },
      { id: "first-tcl-program", title: "First TCL program", kind: "build-trace", interaction: "code" },
    ],
  },
  {
    slug: "host-compiler",
    focus: "lexing, parsing, AST, type checking, IR, optimization, allocation, code generation, object files, and linking",
    entry: "TCL source and compiler configuration",
    exit: "an object or linked artifact plus diagnostics that identify every major stage",
    concept: "A compiler is a sequence of contracts, not one magical translation step. Lexing recognizes tokens, parsing builds structure, type checking proves local meaning, IR makes transformations inspectable, optimization changes representation under invariants, allocation chooses locations, code generation emits target operations, and object/link stages make cross-file addresses concrete. Diagnostics should make the first broken contract visible.",
    implementation: "ternary_compiler.h, ternary_compiler_codegen.h, and tritc.cpp cover the host compiler driver and output path. Compiler, ABI, allocator, and corpus tests supply evidence at multiple stages. The plan labels syscall tracing as a gap so a missing diagnostic surface is not mistaken for a successful compiler trace.",
    planned: "Some end-to-end tracing and optimization observability remain limited. The route therefore exposes the exact source and test records and gives a bounded exercise that can report either a compiler success or a real failure.",
    worked: "For first_value returning 1 + 2, lexing produces identifiers and literals, parsing creates a function node, typing checks t40, lowering creates an add and return, allocation chooses registers or temporaries, code generation emits instructions, and linking resolves runtime symbols. If t40 is misspelled, the error should occur before allocation; if a runtime symbol is missing, it belongs later.",
    investigation: "Run the frontend smoke or phase-seven compiler test and read its input beside the compiler entry point. Search for the AST or IR handoff, then inspect the codegen wrapper that maps an operation to an instruction. Capture one success and one failure stage in your notes.",
    misconception: "Optimization is not allowed to change observable semantics, and a compiler binary existing on disk is not evidence that a particular source program passed every stage. A successful parse does not imply type correctness or a linked object.",
    action: "Make a pipeline ledger with one input and one output for each compiler stage. Add a failure injection at lexing, typing, and linking, and state which evidence file would prove the failure is caught.",
    terms: ["lexer", "AST", "type checker", "code generation", "object file"],
    termNotes: {
      lexer: "the compiler stage that turns characters into tokens under the language's lexical rules",
      AST: "an abstract syntax tree that records parsed structure without committing to machine layout",
      "type checker": "the stage that proves source operations satisfy the declared type contract",
      "code generation": "the stage that selects target instructions and ABI details for lowered operations",
      "object file": "a relocatable compilation result that still carries symbols or patches for linking",
    },
    lessons: [
      { id: "compiler-pipeline", title: "Compiler pipeline", kind: "mental-model", interaction: "trace" },
      { id: "compiler-object-trace", title: "Trace source to object and link", kind: "build-trace", interaction: "code" },
    ],
  },
  {
    slug: "self-hosted-compiler",
    focus: "bootstrap stages, trust boundaries, self-hosting constraints, compiler tests, and reproducibility",
    entry: "a host compiler, TCL compiler sources, and a bootstrap plan",
    exit: "a staged compiler claim with independent checks for each trust boundary",
    concept: "Self-hosting means a compiler can eventually build itself, but the route there is a chain of trust. A host compiler may build an early compiler, that compiler may build a later compiler, and the outputs should be compared or tested so a bootstrap stage does not silently introduce a different language or ABI. The important question is not whether a compiler invokes itself, but what is trusted at each stage and how the result is checked.",
    implementation: "tcl_frontend.trit, tcl_backend.trit, and the TreatCode compiler driver describe the repository's self-hosting direction. Native library and compiler pipeline tests validate pieces of the route. The course keeps bootstrap output separate from a claim that the entire toolchain is independently reproducible.",
    planned: "A fully self-hosted and independently bootstrapped release remains a stronger goal than the current evidence. The gap is explicit so learners can distinguish native-hosted compilation from a trusted bootstrap chain.",
    worked: "Stage A uses the host compiler to build a small TCL compiler. Stage B runs that compiler to build the same source again. A meaningful comparison records compiler version, source commit, input files, and output hashes; a visual 'it ran twice' is not enough. If Stage B emits a different object, the difference becomes a reproducibility investigation rather than a silent success.",
    investigation: "Read the frontend and backend entry points and locate the tests that exercise native ulib or compiler pipeline behavior. Map each stage to its input compiler and output artifact. Then list one threat to the bootstrap trust boundary, such as an untracked host dependency.",
    misconception: "A compiler written in TCL is not automatically self-hosted. Self-hosting requires a working build path, a bootstrap strategy, and evidence that the resulting compiler respects the same contract. Reusing a host compiler behind the scenes can be useful without satisfying that stronger claim.",
    action: "Draw a three-stage bootstrap graph and attach a falsifiable check to every edge. Include the source commit, expected output identity, and the failure label you would publish if the comparison diverges.",
    terms: ["bootstrap", "self-hosting", "trust boundary", "compiler seed", "reproducibility witness"],
    termNotes: {
      bootstrap: "a staged process that uses an earlier trusted tool to construct a later toolchain stage",
      "self-hosting": "the property that a language compiler can be built using programs in its own language",
      "trust boundary": "the point where an assumption or external tool must be accepted or independently checked",
      "compiler seed": "the initial compiler implementation used to start a bootstrap sequence",
      "reproducibility witness": "an output identity, hash, or test result that lets stages be compared rather than asserted",
    },
    lessons: [
      { id: "self-hosting-bootstrap", title: "Bootstrap and self-hosting", kind: "mental-model", interaction: "trace" },
      { id: "self-hosted-reproducibility", title: "Verify a bootstrap boundary", kind: "build-trace", interaction: "source" },
    ],
  },
  {
    slug: "runtime-sdk",
    focus: "runtime services, libraries, streams, handles, SDK wrappers, ABI stability, and error propagation",
    entry: "a compiled program that needs services beyond arithmetic",
    exit: "a library or SDK call whose handle, ABI, and failure path are visible",
    concept: "The runtime is the first layer that makes an operating environment feel useful. It turns low-level calls into conventions for streams, memory, handles, errors, and libraries. An SDK is not decoration: it is an ABI-preserving boundary that lets application code use services without manually rebuilding register and CSR conventions. The boundary is successful only when success and failure are both representable.",
    implementation: "ulib.trit, apps/os_sdk.trit, and apps/libwidget.trit implement the current library and SDK surfaces. Native app and runtime tests exercise wrappers, while the syscall manifest remains the ABI authority. The lesson traces a call from a user wrapper to a service ID and back through status, payload, and detail values.",
    planned: "Some higher-level services and richer tracing are still gaps. A wrapper can be implemented while a backing device or persistence feature remains planned; the status label must follow the evidence.",
    worked: "A file-open wrapper may load a service ID into CSR syscall_id, pass a path handle or pointer in an argument register, and return a status plus handle. If the path is invalid, the status must distinguish failure from a valid handle. A caller that ignores the status can turn a service error into an unrelated memory fault.",
    investigation: "Read the SDK wrapper beside SYSCALL_MANIFEST.json and the focused OS tests. Record the service ID, argument registers, return registers, and error propagation. Follow one widget or stream helper into its underlying runtime call and mark the layer where the contract changes.",
    misconception: "A library function is not automatically a kernel syscall, and a handle is not a raw pointer. The SDK may validate, translate, or preserve ownership before crossing the kernel boundary. Returning a zero-like value is not sufficient if the ABI requires a separate status.",
    action: "Create an ABI table for one successful and one failing runtime call. Include input ownership, service ID, registers, output handle, and the exact test that validates the wrapper.",
    terms: ["runtime", "library", "stream", "handle", "SDK ABI"],
    termNotes: {
      runtime: "the services and conventions available to a running program above the kernel boundary",
      library: "reusable code whose exported behavior is governed by a source and binary interface",
      stream: "an ordered service interface for reading or writing data without exposing storage layout directly",
      handle: "an opaque capability or identifier whose validity is checked by its owning subsystem",
      "SDK ABI": "the application-facing form of the runtime and syscall conventions that must remain stable",
    },
    lessons: [
      { id: "runtime-sdk-abi", title: "Runtime, libraries, and SDK ABI", kind: "mental-model", interaction: "choice" },
      { id: "runtime-streams-errors", title: "Trace a stream and error path", kind: "build-trace", interaction: "code" },
    ],
  },
  {
    slug: "boot-trap",
    focus: "reset state, image loading, boot stages, trap entry, first user program, and failure diagnosis",
    entry: "reset state and a bootable image",
    exit: "a first executable with a diagnosable path from reset to service call",
    concept: "Boot is a sequence of ownership transfers. Reset establishes a small architectural state, a loader finds and verifies an image, assembly establishes runtime and trap entry, and the kernel eventually admits the first executable. Each handoff narrows the unknowns. A boot failure is easier to diagnose when the evidence says whether reset, image parsing, instruction decode, trap entry, or process launch failed.",
    implementation: "bootloader.tasm, native_kernel_boot.tasm, native_kernel_trap_stub.tasm, and the first-silicon bring-up fixture cover the current boot boundary. Architecture, bring-up, and OS tests provide evidence. The image format manifest is read with the loader so a valid file and a valid boot sequence are not conflated.",
    planned: "Some hardware reset and physical device behavior remains simulated or host-provided. The route labels those assumptions and teaches a failure taxonomy instead of promising a universal board bring-up.",
    worked: "At reset, the PC and privilege state are known. The loader reads a header, checks the format version and section bounds, and places code at the expected guest address. The boot stub installs a trap target, then jumps to kernel initialization. If the first user program faults, the correct question is whether the fault happened before process state, during a syscall, or after a user instruction.",
    investigation: "Read the bootloader and trap stub together, then open the bring-up and OS tests. Build a timeline with reset, image check, load, trap vector, kernel entry, process creation, and first instruction. For one negative test, record the evidence that distinguishes a malformed image from a malformed instruction.",
    misconception: "Boot is not just copying bytes and jumping to an address. Privilege, image version, memory layout, and trap entry all participate. A file that has the right extension but the wrong header is not bootable evidence.",
    action: "Design a boot checklist with a stop condition and artifact for each stage. Use it to classify three hypothetical failures and identify which source and test record a learner should open next.",
    terms: ["reset state", "bootloader", "trap entry", "image header", "first executable"],
    termNotes: {
      "reset state": "the architecturally defined register, PC, privilege, and memory state at the start of execution",
      bootloader: "the early program that validates and places a boot image before transferring control",
      "trap entry": "the defined transition from an executing context to a trap handler and cause record",
      "image header": "the format metadata used to validate sections, versions, sizes, and load addresses",
      "first executable": "the first user-visible program admitted after boot and kernel initialization",
    },
    lessons: [
      { id: "boot-and-traps", title: "Boot and traps", kind: "mental-model", interaction: "trace" },
      { id: "first-executable-boot", title: "Diagnose the first executable", kind: "build-trace", interaction: "source" },
    ],
  },
  {
    slug: "kernel-foundations",
    focus: "hardware abstraction, allocation, address translation, TLBs, process and context state, scheduling, traps, IPC, and isolation",
    entry: "a booted machine state and a request from a process",
    exit: "an isolated kernel transition with ownership, scheduling, and fault evidence",
    concept: "A kernel turns shared hardware into controlled abstractions. The HAL gives higher layers stable operations, memory management names ownership and address spaces, the MMU/TLB translates accesses, process state records context, scheduling chooses who runs, traps regain control, and IPC moves data under an isolation rule. These are separate mechanisms that meet at carefully defined boundaries.",
    implementation: "kernel/hal.trit, kernel/process.trit, and kernel.trit define the current foundation. Layer-one, kernel, OS-platform, phase-D, process-handoff, and scaling tests provide evidence. The public registry also records crash-recovery and fuzz-harness gaps, which are not hidden by the presence of basic process code.",
    planned: "Crash recovery, fuzz harnesses, and some hardware isolation claims remain incomplete. The lesson treats the current allocator, process, and trap tests as implemented slices and labels the broader production guarantee as planned where appropriate.",
    worked: "A user load first names a virtual address. The MMU checks the page mapping or TLB entry, the kernel enforces permissions, and the process resumes or takes a page fault. A context switch saves the old PC and registers, selects another runnable process, and restores its state. IPC then copies or shares data under an explicit capability rule rather than exposing the entire address space.",
    investigation: "Follow a process-handoff test into process state and kernel dispatch. Compare a successful syscall with an invalid pointer case. Identify the HAL call, address-translation decision, saved context, and returned status for each path.",
    misconception: "A process is not just a function call and a TLB is not the page table. The TLB caches translations; the MMU and kernel policy decide whether an access is permitted. Switching stacks without switching address-space or privilege state would not provide isolation.",
    action: "Trace one page fault through hardware abstraction, translation, trap, scheduler, and process resumption. Write the invariant that prevents a faulting process from silently reading another process's page.",
    terms: ["HAL", "address space", "MMU", "TLB", "IPC"],
    termNotes: {
      HAL: "a hardware-abstraction boundary that gives kernel code stable operations over platform details",
      "address space": "the set of guest addresses and permissions visible to one process or execution context",
      MMU: "the memory-management mechanism that translates and checks virtual memory accesses",
      TLB: "a cache of recent address translations; it is not the authoritative mapping policy",
      IPC: "inter-process communication with an explicit data-transfer and isolation contract",
    },
    lessons: [
      { id: "kernel-syscalls", title: "Kernel foundations and syscalls", kind: "mental-model", interaction: "choice" },
      { id: "kernel-memory-process", title: "Trace memory, traps, and IPC", kind: "build-trace", interaction: "code" },
    ],
  },
  {
    slug: "storage-vfs",
    focus: "block I/O, caching, write-ahead logging, recovery, relational state, filesystem and VFS operations, and consistency",
    entry: "a logical storage request and a block or volume boundary",
    exit: "a durable or recoverable operation with stated consistency and failure behavior",
    concept: "Storage is a consistency problem across volatile memory, durable blocks, metadata, and interruption. BIO moves requests, a buffer pool avoids repeated I/O, a WAL records intent before a state change, relational state organizes metadata, and a VFS gives callers a stable naming and operation surface. Recovery is part of correctness because a power loss can occur between any two writes.",
    implementation: "kernel/bio.trit, kernel/vfs.trit, and ternary_redo_wal.h implement the current storage surfaces. Production-layer, migration, WAL, and next VFS persistence/recovery tests provide the evidence trail. Image formats are related but distinct: a release image packages an environment, while a WAL protects updates inside a storage system.",
    planned: "Crash recovery and encrypted volumes remain recorded gaps. A passing WAL fixture demonstrates the tested recovery contract, not every filesystem failure mode or production disk guarantee.",
    worked: "To update a directory entry and its inode, append a redo record describing the intended new values, flush the log according to its contract, then update the durable structures. On restart, recovery replays complete records or discards incomplete ones. If the log is written after the metadata, a torn update can leave a name pointing at the wrong object.",
    investigation: "Read the WAL header and the VFS operation side by side. Trace a create or rename test through buffer lookup, log append, commit, and recovery. Record which test distinguishes a committed record from an interrupted one and which gap remains outside the fixture.",
    misconception: "A cache is not durability and a filesystem path is not a block address. WAL ordering protects a recovery story, but it does not replace a volume format or encryption policy. Returning success before the specified durable point changes the contract.",
    action: "Write a two-write failure table for a rename. For each interruption point, state what recovery should see, what record proves it, and whether the current gap list covers the case.",
    terms: ["BIO", "buffer pool", "WAL", "VFS", "recovery point"],
    termNotes: {
      BIO: "a block-I/O request boundary between a storage caller and the device or volume layer",
      "buffer pool": "a cache of blocks whose residency and dirty state must be managed explicitly",
      WAL: "a write-ahead log that records an update before durable state is changed so recovery can reason about order",
      VFS: "a virtual filesystem interface that presents stable operations above concrete storage formats",
      "recovery point": "the durable boundary after which a specified operation must survive interruption",
    },
    lessons: [
      { id: "storage-and-images", title: "Storage and images", kind: "mental-model", interaction: "choice" },
      { id: "storage-recovery-vfs", title: "Trace WAL recovery and VFS", kind: "build-trace", interaction: "code" },
    ],
  },
  {
    slug: "devices-services",
    focus: "device discovery and lifecycle, drivers, interrupts and DMA boundaries, networking, graphics, services, and current gaps",
    entry: "a device or service request at the kernel boundary",
    exit: "a lifecycle and failure story that does not overclaim hardware support",
    concept: "Devices are asynchronous, stateful participants. Discovery names what exists, initialization establishes ownership and capabilities, drivers translate requests, interrupts report events, and DMA crosses a memory boundary that needs permission and lifetime rules. Networking, graphics, and services build on those primitives but have different timing and failure behavior. A useful device lesson explains the unavailable case as carefully as the happy path.",
    implementation: "kernel/net.trit, apps/service_stub.trit, and ternary_os.h show the current networking and service-facing boundary. OS-platform, drivers/HAL, and graphics/GUI targets provide evidence or explicit future work. Syscall tracing is recorded as a gap, so service observability remains distinct from service existence.",
    planned: "Physical drivers, full graphics integration, and syscall tracing may be planned or referenced rather than shipped. The public registry carries these labels to the lesson so a stub cannot be mistaken for a production device.",
    worked: "A network send enters through an API, becomes a kernel request, and is queued for a driver. The driver owns a descriptor and may receive an interrupt when the device completes. If DMA writes into a buffer after the caller releases it, the bug is a lifetime violation even if the packet contents look correct. A service stub can demonstrate the message shape without proving hardware delivery.",
    investigation: "Open the network source and service stub, then compare their assumptions with the drivers and GUI next-test records. For one request, list discovery, ownership, queue, event, and completion. Mark the exact point where the current implementation becomes a planned hardware boundary.",
    misconception: "An API that accepts a device request is not proof that a physical driver exists. Interrupts are not polling, DMA is not a free memory copy, and a graphics surface without frame evidence is not a display guarantee.",
    action: "Create a device lifecycle state diagram from absent to discovered, initialized, busy, completed, and failed. Attach one source, one test, and one gap or capability to every nontrivial transition.",
    terms: ["driver", "interrupt", "DMA", "network service", "device lifecycle"],
    termNotes: {
      driver: "code that translates a kernel service contract into a particular device protocol",
      interrupt: "an asynchronous event that requests controlled attention from the current execution flow",
      DMA: "device-managed memory transfer whose buffer ownership and permissions must be explicit",
      "network service": "a kernel or runtime boundary that exposes packet or connection behavior to callers",
      "device lifecycle": "the discover, own, initialize, operate, drain, and fail sequence for a device",
    },
    lessons: [
      { id: "devices-services", title: "Devices and services", kind: "mental-model", interaction: "choice" },
      { id: "network-graphics-boundary", title: "Trace networking, graphics, and driver gaps", kind: "build-trace", interaction: "source" },
    ],
  },
  {
    slug: "host-runtime",
    focus: "host and guest boundaries, native runtime integration, portability, virtualization assumptions, and observability",
    entry: "a guest operation executed by a host process",
    exit: "a host integration claim that names the boundary, portability assumptions, and observability",
    concept: "A hosted OS or VM borrows resources from a host operating system. The host owns threads, files, windows, clocks, and process isolation; the guest owns its architectural model and guest resources. Integration is valuable because it makes the system runnable, but it can also hide assumptions. A sound model names which state is guest state, which timing is host timing, and which failures are translation failures.",
    implementation: "ternary_host_runtime.h, run_tos_sdl.cpp, and ternary_consumer_shell.h implement the current native integration and consumer shell boundaries. Host-runtime and productization tests provide evidence. Deterministic replay and framebuffer PNG are recorded gaps, so observability claims are narrower than a full emulator trace.",
    planned: "Portability across hosts, deterministic replay, and artifact-level framebuffer capture need additional evidence. A host run can be current while a portable or replayable run remains planned.",
    worked: "A guest write to a virtual display becomes a host buffer update, and the host window toolkit presents it later on its own event loop. The guest may report a deterministic frame index, but host presentation time can vary. If a host file operation fails, the error must be translated into a guest status without leaking a host pointer or path assumption.",
    investigation: "Read the host runtime adapter beside the host-runtime test and shell productization test. Annotate every crossing: thread, file, window, clock, and memory. Then check which gap describes missing replay or screenshot evidence.",
    misconception: "Running on a host is not the same as running on bare hardware, and a host timestamp is not a guest architectural clock. Portability requires more than compiling the same source; it requires the boundary assumptions to be tested.",
    action: "Make a host/guest ownership table for one frame and one file request. Include lifecycle, error translation, and the observable evidence available at each side.",
    terms: ["host", "guest", "native integration", "virtualization", "observability"],
    termNotes: {
      host: "the operating system and process environment that supplies resources to the guest runtime",
      guest: "the Trit architectural and operating-system state being executed or emulated",
      "native integration": "the adapter that connects guest behavior to host threads, files, windows, or devices",
      virtualization: "an execution model that presents an isolated or translated machine interface above a host",
      observability: "the records and signals that let a learner inspect state, timing, failure, and provenance",
    },
    lessons: [
      { id: "host-runtime-integration", title: "Host and guest runtime boundaries", kind: "mental-model", interaction: "choice" },
      { id: "host-observability-portability", title: "Inspect native integration and portability", kind: "build-trace", interaction: "source" },
    ],
  },
  {
    slug: "user-space-apps",
    focus: "processes, shell and GUI composition, app SDKs, bundled apps, permissions, and user-facing failure paths",
    entry: "runtime services and an application manifest",
    exit: "a user-facing program whose process, permissions, and failure paths are testable",
    concept: "User space is where the stack becomes a product for a person. A shell composes commands and services, a GUI composes events and surfaces, and applications use SDKs rather than reaching through arbitrary kernel state. Permissions and process boundaries determine what a program may do. A polished interface still needs a failure path for missing files, denied services, and partial capabilities.",
    implementation: "apps/desktop.trit, apps/shell.trit, and APP_MANIFEST.json define the bundled application route. App, native-app, consumer-shell, and next desktop tests provide evidence. The app manifest is a contract for image registration and guest paths, not merely a list used to populate a card.",
    planned: "Golden app snapshots and some consumer assets remain gaps. A bundled app can be compiled and registered while its visual or full-device behavior remains explicitly unverified.",
    worked: "A shell command starts a process through the runtime, the app receives only its permitted handles, and the GUI event loop delivers an input. If an app requests a missing capability, the user-facing result should be a clear error or disabled action. The shell's process handoff is part of the feature because a binary that cannot be launched is not a useful bundle.",
    investigation: "Read the shell and desktop sources with APP_MANIFEST.json. Trace one app from source to SDK call, process creation, image registration, and focused test. Then choose a failure such as a missing guest path and identify where it should be caught.",
    misconception: "A GUI label is not an application capability, and a manifest entry is not proof of launch. User space depends on process, permission, runtime, image, and test contracts that can fail independently.",
    action: "Write a launch trace with five checkpoints: manifest lookup, process creation, permission check, first service call, and user-visible error. Attach exact evidence to each checkpoint.",
    terms: ["user space", "shell", "GUI", "application manifest", "permission"],
    termNotes: {
      "user space": "the less-privileged process environment that uses kernel and runtime services through contracts",
      shell: "a user-facing command or process-composition surface that launches and connects programs",
      GUI: "a graphical event and rendering surface layered over runtime and device services",
      "application manifest": "the release record for bundled app identity, guest path, and packaging metadata",
      permission: "a policy decision that grants or denies an operation independently of its requested syntax",
    },
    lessons: [
      { id: "applications-and-validation", title: "Applications and validation", kind: "mental-model", interaction: "choice" },
      { id: "shell-gui-apps", title: "Trace shell, GUI, and app launch", kind: "build-trace", interaction: "code" },
    ],
  },
  {
    slug: "images-release",
    focus: "image formats, loaders, manifests, packaging, release channels, compatibility, and recovery",
    entry: "compiled artifacts, app metadata, and a release format contract",
    exit: "a package or image that can be inspected, loaded, and recovered under a stated version policy",
    concept: "Packaging turns separately built pieces into a distributable promise. An image format defines headers, sections, offsets, sizes, and version compatibility. A loader checks those invariants before executing. A manifest explains what is included, while a release channel says which audience and compatibility policy applies. Recovery matters because a failed update should not leave an ambiguous artifact or half-installed state.",
    implementation: "IMAGE_FORMAT_MANIFEST.json, build_tos_image.cpp, and tools/trit-inspect-image.ps1 define the current image route. Host-runtime, migration, and full-system release tests validate parts of loading and compatibility. The generated image remains distinct from the source manifest and from the public snapshot.",
    planned: "Some external assets and recovery scenarios are limited by availability. The course labels a format contract as implemented only where its parser or test evidence exists, not because a file was produced once.",
    worked: "A loader reads a versioned header, validates section bounds against the file size, checks the declared architecture, and maps code and data into guest regions. The app manifest then supplies guest paths. If a section extends beyond the file, the loader must reject it before writing memory. A migration path may accept an older version only with an explicit conversion record.",
    investigation: "Read the image format manifest with the builder and inspection tool. Trace a staged release test through header validation and app registration. Compare a compatible image with a malformed or future-version image and state the expected error boundary.",
    misconception: "A successful build is not a compatible release, and a file extension is not a loader contract. Packaging can preserve invalid metadata unless inspection and load tests reject it. A migration tool is not permission to silently reinterpret every future format.",
    action: "Design a release manifest table with version, sections, guest paths, checksums, and rollback point. Add one invalid row and explain exactly which validator should catch it.",
    terms: ["image format", "loader", "packaging manifest", "release channel", "compatibility"],
    termNotes: {
      "image format": "the versioned binary layout and metadata contract for a bootable or loadable artifact",
      loader: "the component that validates and places an image before transferring execution to it",
      "packaging manifest": "the record of included artifacts, guest paths, versions, and release metadata",
      "release channel": "a distribution boundary with a declared audience, update policy, and compatibility expectation",
      compatibility: "the property that a consumer can safely interpret an artifact under the declared contract",
    },
    lessons: [
      { id: "images-packaging-distribution", title: "Images, packaging, and distribution", kind: "mental-model", interaction: "choice" },
      { id: "release-loading-recovery", title: "Inspect loading and release recovery", kind: "build-trace", interaction: "source" },
    ],
  },
  {
    slug: "specialized-products",
    focus: "libraries and tooling, product boundaries, extension points, and how contributors use the platform",
    entry: "a platform extension request or specialized workload",
    exit: "a product boundary with ownership, evidence, and an honest capability label",
    concept: "Specialized libraries and products are where a general platform meets a demanding workload. Cryptographic arithmetic, transformer runtimes, benchmark tooling, and TreatCode UI each need an extension point and a correctness boundary. A product is not just a library file: it includes the inputs it supports, the performance profile it claims, the tests that protect it, and the gaps that limit distribution.",
    implementation: "ternary_montgomery.h, ternary_transformer_runtime.h, and treatcode/src/App.tsx represent current specialized library and product surfaces. Numeric workload and benchmark tests provide evidence, while BitNet and Doom assets are recorded as gaps where full external inputs are unavailable.",
    planned: "Full external workload assets and some product integrations remain planned. A local benchmark or partial library implementation must carry its scope and cannot be promoted to a complete product claim.",
    worked: "A Montgomery multiplication helper can be correct for a fixed width and still be unsuitable for an unsupported modulus or accelerator layout. A transformer runtime can expose a tensor operation while its external model assets remain unavailable. The product boundary says which shapes, widths, and artifacts are accepted and what happens outside them.",
    investigation: "Read one specialized header, its workload test, and its benchmark record. Identify the input contract, output correctness check, and performance measurement. Then open the linked gap and describe what new evidence would be needed before widening the product claim.",
    misconception: "A benchmark name is not a product specification, and a library header is not proof of every width or workload. Specialized code needs a bounded capability envelope and a contributor workflow that preserves evidence.",
    action: "Write a one-page extension proposal for a new specialized library. Include API boundary, supported representation, correctness test, benchmark profile, source ownership, and explicit unsupported cases.",
    terms: ["extension point", "specialized library", "workload profile", "product boundary", "capability envelope"],
    termNotes: {
      "extension point": "a documented place where a new library, backend, or product can connect without bypassing contracts",
      "specialized library": "a reusable implementation optimized or tailored for a constrained domain or representation",
      "workload profile": "the input sizes, shapes, distributions, and target conditions used to interpret a result",
      "product boundary": "the owned surface that defines inputs, outputs, support, evidence, and failure behavior",
      "capability envelope": "the explicit set of cases a feature supports, separate from cases merely imaginable",
    },
    lessons: [
      { id: "specialized-libraries-products", title: "Specialized libraries and products", kind: "mental-model", interaction: "choice" },
      { id: "developer-extension-workflow", title: "Investigate an extension boundary", kind: "build-trace", interaction: "code" },
    ],
  },
  {
    slug: "closure-evidence",
    focus: "threat model, isolation, fuzzing, benchmark interpretation, performance tradeoffs, and end-to-end validation",
    entry: "a release candidate and a set of correctness, security, and performance claims",
    exit: "a full-system conclusion with scoped evidence and explicit remaining gaps",
    concept: "Closure is not a final screenshot; it is the discipline of checking the stack as a connected system. Security asks what can go wrong and across which boundary. Fuzzing searches malformed or surprising inputs. Benchmarks quantify a workload under a profile and require correctness first. Full-system validation then ties boot, kernel, storage, devices, user space, packaging, and observability into one acceptance story.",
    implementation: "tests_next/manifests/coverage_matrix.json and acceptance_gates.json define future-facing closure structure, while KNOWN_GAPS.md records unresolved work. Production-layer, hardening, agent-tooling, and full-system release tests provide current evidence; Doom and BitNet benchmark records remain bounded by their assets and baselines.",
    planned: "Fuzz harnesses, benchmark baselines, crash recovery, and external assets are not all complete. The correct closure result can therefore be 'verified slice with gaps' rather than a universal green claim.",
    worked: "A release test boots an image, launches a process, writes a file, sends a service request, and records the result. A security check asks whether an untrusted pointer or malformed image can cross each boundary. A benchmark repeats the same workload with a correctness oracle and reports p50 or another declared statistic. If the workload asset is missing, the result is unavailable, not zero.",
    investigation: "Read the acceptance gates and coverage matrix, then compare their rows with production test commands and known gaps. Trace one full-system failure backwards from its user-visible symptom to the first violated contract. Record which parts are current, planned, experimental, or unavailable.",
    misconception: "A passing smoke test is not full-system closure, and a faster run is not a better implementation if correctness changed. Fuzzing finds examples but does not prove all inputs safe; a benchmark without a baseline and workload profile cannot support a general performance claim.",
    action: "Create a closure report for one end-to-end scenario. Include threat, oracle, workload, timing statistic, evidence paths, and an explicit residual-gap section. Do not use the word complete unless every required gate has a named result.",
    terms: ["threat model", "fuzzing", "benchmark baseline", "performance tradeoff", "system closure"],
    termNotes: {
      "threat model": "a scoped description of actors, assets, trust boundaries, and failure consequences",
      fuzzing: "automated exploration of malformed or varied inputs to find crashes, invariant breaks, or unexpected states",
      "benchmark baseline": "a versioned comparison point that makes a performance result interpretable",
      "performance tradeoff": "a measured change in one cost or benefit that may affect another contract or resource",
      "system closure": "an evidence-backed statement that connected end-to-end gates pass within a declared scope",
    },
    lessons: [
      { id: "security-fuzzing-performance-closure", title: "Security, fuzzing, and performance closure", kind: "mental-model", interaction: "choice" },
      { id: "full-system-validation", title: "Build a full-system validation trail", kind: "build-trace", interaction: "code" },
    ],
  },
];

const LEGACY_SOURCE_LABELS = new Map([
  ["TCL_Spec_1.0.md", "TCL 1.0 language specification"],
  ["SYSCALL_MANIFEST.json", "Syscall ABI manifest"],
  ["TEST_MANIFEST.json", "Authoritative test manifest"],
]);

function collectFiles(root) {
  if (!fs.existsSync(root)) return [];
  const result = [];
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const filePath = path.join(root, entry.name);
    if (entry.isDirectory()) result.push(...collectFiles(filePath));
    else result.push(filePath);
  }
  return result;
}

const testFiles = collectFiles(path.join(repoRoot, "tests"))
  .concat(collectFiles(path.join(repoRoot, "tests_next")))
  .concat(collectFiles(path.join(repoRoot, "tools", "tests")));

function testFileForId(testId) {
  const stem = testId.replace(/^tc:test:/, "");
  const target = stem.replaceAll("-", "_");
  const candidates = testFiles.filter((filePath) => path.basename(filePath, path.extname(filePath)) === target);
  return candidates.length ? repoRelative(candidates[0]) : null;
}

function sourceReferences(node) {
  const paths = (node.source_refs || []).map((ref) => ref.path).filter(Boolean);
  const safePaths = paths.filter((value) => fs.existsSync(path.join(repoRoot, value)));
  const finalPaths = safePaths.length ? safePaths : ["TEST_MANIFEST.json"];
  return unique(finalPaths).map((sourcePath) => ({
    path: sourcePath,
    label: LEGACY_SOURCE_LABELS.get(sourcePath) || path.basename(sourcePath),
    kind: sourcePath.endsWith(".json") ? "manifest" : sourcePath.startsWith("docs/") ? "documentation" : "source",
  }));
}

function evidenceReferences(node) {
  const refs = [{ path: "TEST_MANIFEST.json", label: "Authoritative test manifest", kind: "manifest" }];
  for (const testId of node.test_ids || []) {
    const testPath = testFileForId(testId);
    if (testPath) refs.push({ path: testPath, label: `${testId} test source`, kind: "test" });
  }
  return unique(refs.map((item) => item.path)).map((referencePath) => refs.find((item) => item.path === referencePath));
}

function linkForPath(pathValue) {
  return `https://github.com/JonasComlita/dualrail/blob/main/${pathValue}`;
}

function buildInteractive(phase, lesson, node, commit) {
  const firstSource = sourceReferences(node)[0]?.path || "TEST_MANIFEST.json";
  const firstTest = (node.test_ids || [])[0] || "test-agent-tooling";
  const codeStarter = "import ulib;\n\nfn first_value() -> t40 {\n    return 1 + 2;\n}";
  if (lesson.interaction === "choice") {
    const prompt = phase.slug === "representation"
      ? "Which value is the numeric source of truth for scalar arithmetic?"
      : phase.slug === "storage-vfs"
        ? "Which boundary protects a storage update after interruption?"
        : phase.slug === "closure-evidence"
          ? "What must come before a performance claim?"
          : `Which statement keeps the ${phase.slug} boundary honest?`;
    const options = phase.slug === "representation"
      ? ["Two bits per lane", "The positional numeric value", "A host pointer"]
      : phase.slug === "storage-vfs"
        ? ["A write-ahead log before durable metadata", "A UI label", "A benchmark name"]
        : phase.slug === "closure-evidence"
          ? ["A correctness oracle and workload profile", "A larger title", "An unavailable asset"]
          : ["The exact source and validation contract", "A nearby concept with no evidence", "A UI-only state change"];
    return { kind: "choice", title: phase.slug === "representation" ? "Boundary check" : phase.slug === "isa-binary" ? "Decode check" : phase.slug === "images-release" ? "Release check" : "Concept check", prompt, options, answer: 1, explanation: `The repository boundary is defined by the current source and validation evidence for ${node.name}. Planned and unavailable work remains labelled.` };
  }
  if (lesson.interaction === "trace") {
    const labels = ["Input contract", "State or artifact check", "Boundary transition", "Observable result"];
    return {
      kind: "trace",
      title: `${node.name} trace`,
      prompt: `Advance through the ${phase.slug} transition and inspect the state at each boundary.`,
      steps: labels.map((label, index) => ({
        label,
        state: index === 0 ? phase.entry : index === 1 ? phase.worked.split(".")[0] : index === 2 ? phase.exit : `Evidence: ${firstTest}`,
        explanation: index === 0 ? `Start with ${phase.entry}.` : index === 1 ? phase.investigation.split(".")[0] + "." : index === 2 ? `The transition leaves the ${phase.slug} boundary only when its invariant holds.` : `Use ${firstTest} to validate the observed result.`,
      })),
    };
  }
  if (lesson.interaction === "source") {
    return {
      kind: "source",
      title: "Source investigation",
      prompt: `Find the exact implementation boundary for ${node.name}.`,
      sourcePath: firstSource,
      sourceHref: linkForPath(firstSource, commit),
      expectedIncludes: [path.basename(firstSource)],
      explanation: `Open the source and compare it with the ${firstTest} validation record. This exercise reports the repository boundary instead of pretending that a link is a lesson.`,
    };
  }
  return {
    kind: "code",
    title: phase.slug === "tcl-language" ? "Try the first function" : "Run a bounded compiler check",
    starter: codeStarter,
    expectedIncludes: ["fn first_value", "return 1 + 2"],
    exercise_id: lesson.id,
    runner: "treatcode-learning-compiler",
    explanation: `Submit the bounded TCL example to the repository-backed learning runner. It may pass or report a real compiler failure; either result is evidence about the ${node.name} exercise, not a local text-only success.`,
  };
}

function markdownBody({ phase, lesson, node, page, commit }) {
  const firstSource = page.sources[0];
  const testNames = page.test_ids.length ? page.test_ids.join(", ") : "the authoritative test manifest";
  const sourceLinks = page.sources.map((source) => `- [${source.label}](${linkForPath(source.path, commit)}) — ${source.path}`).join("\n");
  const evidenceLinks = page.evidence.map((evidence) => `- [${evidence.label}](${linkForPath(evidence.path, commit)}) — ${evidence.path}`).join("\n");
  const nextLink = page.next ? `[Continue to the next lesson](/learn/eecs/${page.next})` : "the course closure report";
  const modeSentence = lesson.kind === "mental-model"
    ? `This is the mental-model lesson for ${node.name}. Begin with the general systems idea, then compare it with the current Trit boundary before you touch a tool.`
    : `This is the build, trace, and evidence lesson for ${node.name}. The goal is to inspect a real repository transition and report both what the current tests prove and what they do not prove.`;
  return `## Objectives

- Explain ${phase.focus} in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

${page.prerequisites.length ? `Read ${page.prerequisites.map((id) => `[${id}](/learn/eecs/${id})`).join(", ")} first. Those lessons introduce the state and vocabulary that this page assumes.` : `No earlier Learn page is required. The Stack Explorer phase still records repository dependencies, so use the source and phase links if a term is unfamiliar.`} ${modeSentence} A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

${phase.concept} In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. ${lesson.kind === "mental-model" ? `The useful question here is not only “what does Trit call this?” but “what invariant would another computer system need at the same boundary?”` : `The useful question here is whether the repository's files and tests actually carry that invariant through a build or execution step.`}

## Explanation

${phase.implementation} The phase enters through ${phase.entry} and leaves through ${phase.exit}. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

${lesson.kind === "mental-model" ? phase.worked : phase.investigation} The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

${phase.implementation} The current registry row is ${node.id}, with implementation coverage marked ${node.coverage?.implemented?.status || "recorded"} and tested coverage marked ${node.coverage?.tested?.status || "recorded"}. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

${phase.planned} Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

${phase.worked} For this lesson, write the example as a sequence: first identify ${phase.entry}; next apply the ${phase.slug} rule; then inspect ${phase.exit}; finally compare the result with the named validation record. ${lesson.kind === "build-trace" ? `The repository investigation should begin at ${firstSource.path} and cross-check the test IDs ${testNames}.` : `The conceptual check should explain why the result would be wrong if a neighboring representation or layer were substituted.`} This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

${phase.misconception} Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

${phase.action} Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

${sourceLinks}

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/${node.slug}) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

${evidenceLinks}

The test IDs attached to this lesson are ${testNames}. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. ${node.benchmark_ids?.length ? `This phase also has benchmark records ${node.benchmark_ids.join(", ")}; interpret them only with their workload and baseline.` : "This phase has no benchmark record in the current registry."}

## Next step

${nextLink}. The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
`;
}

function pageMeta(phase, lesson, node, pageIds, commit) {
  const phaseIndex = node.ordinal;
  const lessonIndex = phase.lessons.indexOf(lesson);
  const prior = lessonIndex === 0 ? (phaseIndex > 0 ? pageIds[phaseIndex - 1]?.[1] : null) : pageIds[phaseIndex]?.[0];
  const prerequisites = phase.slug === "representation" && lessonIndex === 0 ? [] : prior ? [prior] : [];
  const next = lessonIndex === 0 ? pageIds[phaseIndex]?.[1] : pageIds[phaseIndex + 1]?.[0] || null;
  const sources = sourceReferences(node);
  const evidence = evidenceReferences(node);
  const testIds = unique(node.test_ids || []);
  return {
    schema: "trit.treatcode_learning_page.v2",
    id: lesson.id,
    title: lesson.title,
    module: `Phase ${String(node.ordinal).padStart(2, "0")} · ${node.name}`,
    level: node.ordinal <= 8 ? "beginner" : node.ordinal <= 17 ? "programmer" : "eecs",
    order: node.ordinal * 2 + lessonIndex,
    summary: `${lesson.kind === "mental-model" ? "Build the mental model" : "Trace the repository evidence"} for ${node.name}.`,
    phase_id: node.id,
    phase_slug: node.slug,
    phase_name: node.name,
    lesson_kind: lesson.kind,
    implementation_status: node.coverage?.implemented?.status || "recorded",
    canonical_terms: phase.terms,
    objectives: [
      `Explain ${phase.focus}.`,
      `Distinguish the general concept from the current Trit implementation and planned gaps.`,
      `Use a worked example and repository evidence to validate one claim.`,
    ],
    prerequisites,
    sources,
    evidence,
    test_ids: testIds,
    benchmark_ids: unique(node.benchmark_ids || []),
    gap_ids: unique(node.gap_ids || []),
    stack_links: {
      phase: `/stack/${node.slug}`,
      source: sources[0] ? linkForPath(sources[0].path, commit) : null,
      tests: `/stack?focus=${encodeURIComponent(testIds[0] || "")}`,
    },
    next,
    interactive: buildInteractive(phase, lesson, node, commit),
  };
}

function htmlEscape(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function inlineHtml(value) {
  let result = htmlEscape(value);
  result = result.replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_match, label, href) => {
    const safeHref = /^(https?:|\/|#)/.test(href) ? href : "#";
    return `<a href="${htmlEscape(safeHref)}">${label}</a>`;
  });
  result = result.replace(/`([^`]+)`/g, "<code>$1</code>");
  result = result.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
  return result;
}

function markdownHtml(markdown) {
  const lines = markdown.split(/\r?\n/);
  const output = [];
  let inCode = false;
  let code = [];
  let list = null;
  const closeList = () => {
    if (list) output.push(`</${list}>`);
    list = null;
  };
  for (const line of lines) {
    const trimmed = line.trim();
    if (trimmed.startsWith("```")) {
      if (inCode) {
        output.push(`<pre><code>${htmlEscape(code.join("\n"))}</code></pre>`);
        code = [];
        inCode = false;
      } else {
        closeList();
        inCode = true;
      }
      continue;
    }
    if (inCode) {
      code.push(line);
      continue;
    }
    if (!trimmed) {
      closeList();
      continue;
    }
    const heading = trimmed.match(/^(#{2,4})\s+(.+)$/);
    if (heading) {
      closeList();
      const level = Math.min(5, heading[1].length + 1);
      output.push(`<h${level}>${inlineHtml(heading[2])}</h${level}>`);
      continue;
    }
    const item = trimmed.match(/^[-*]\s+(.+)$/) || trimmed.match(/^\d+\.\s+(.+)$/);
    if (item) {
      const desiredList = trimmed.match(/^\d+\./) ? "ol" : "ul";
      if (list !== desiredList) {
        closeList();
        list = desiredList;
        output.push(`<${list}>`);
      }
      output.push(`<li>${inlineHtml(item[1])}</li>`);
      continue;
    }
    closeList();
    output.push(`<p>${inlineHtml(trimmed)}</p>`);
  }
  closeList();
  if (inCode) output.push(`<pre><code>${htmlEscape(code.join("\n"))}</code></pre>`);
  return output.join("\n");
}

function staticShell({ title, body, pathValue, phase, page, allPages, pathItem }) {
  const pathId = pathItem?.id || "eecs";
  const pathPageIds = pathItem?.page_ids || allPages.map((candidate) => candidate.id);
  const pageIndex = Math.max(0, pathPageIds.indexOf(page.id));
  const pathHref = (pageId) => `/learn/${pathId}/${pageId}`;
  const prev = pageIndex > 0 ? pathHref(pathPageIds[pageIndex - 1]) : `/learn/${pathId}`;
  const next = pageIndex + 1 < pathPageIds.length ? pathHref(pathPageIds[pageIndex + 1]) : "/stack/closure-evidence";
  const links = pathPageIds.map((pageId) => {
    const candidate = allPages.find((item) => item.id === pageId) || page;
    return `<li><a href="${pathHref(candidate.id)}"${candidate.id === page.id ? ' aria-current="page"' : ""}>${htmlEscape(candidate.title)}</a></li>`;
  }).join("");
  const sourceCommit = page.stack_links.source?.match(/\/blob\/([^/]+)/)?.[1] || "main";
  const sourceLinks = page.sources.map((source) => `<li><a href="${htmlEscape(linkForPath(source.path, sourceCommit))}" target="_blank" rel="noreferrer">${htmlEscape(source.label)}</a></li>`).join("");
  const tests = page.test_ids.length ? page.test_ids.map((id) => `<li><a href="${htmlEscape(page.stack_links.tests)}">${htmlEscape(id)}</a></li>`).join("") : "<li>No test record is attached in the current registry.</li>";
  const benchmarks = page.benchmark_ids.length ? page.benchmark_ids.map((id) => `<li>${htmlEscape(id)}</li>`).join("") : "<li>No benchmark record is attached in the current registry.</li>";
  const gaps = page.gap_ids.length ? page.gap_ids.map((id) => `<li>${htmlEscape(id)}</li>`).join("") : "<li>No gap record is attached in the current registry.</li>";
  return `<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>${htmlEscape(title)} · Learn Trit · TreatCode</title>
    <meta name="description" content="${htmlEscape(page.summary)}" />
    <link rel="stylesheet" href="/public-shell.css" />
  </head>
  <body>
    <div id="root">
      <div class="tc-static-shell tc-learning-static-shell">
        <header class="tc-static-nav">
          <a class="tc-wordmark" href="/">TREATCODE</a>
          <nav class="tc-static-links" aria-label="Primary navigation"><a href="/stack">Stack Explorer</a><a href="/learn" aria-current="page">Learn</a><a href="/practice">Practice</a><a href="/api/public/v1/openapi.json">API</a></nav>
        </header>
        <main class="tc-static-learning" data-testid="static-learning-page">
          <div class="tc-static-learning-grid">
            <aside class="tc-static-learning-nav" aria-label="Learning pages"><h2>EECS/systems · 42 lessons</h2><ol>${links}</ol></aside>
            <article class="tc-static-learning-article">
              <span class="tc-kicker">Phase ${String(phase.ordinal).padStart(2, "0")} · ${htmlEscape(phase.name)}</span>
              <h1>${htmlEscape(title)}</h1>
              <p class="tc-static-learning-summary">${htmlEscape(page.summary)}</p>
              <p class="tc-static-learning-status">Path lesson ${pageIndex + 1} of ${pathPageIds.length} · Status: ${htmlEscape(page.implementation_status)} · <a href="/stack/${htmlEscape(phase.slug)}">Open Stack Explorer phase</a></p>
              <div class="tc-static-learning-progress" role="progressbar" aria-label="Learning progress" aria-valuemin="0" aria-valuemax="${pathPageIds.length}" aria-valuenow="${pageIndex + 1}"><span style="width:${Math.round((pageIndex + 1) / pathPageIds.length * 100)}%"></span></div>
              <div class="tc-static-learning-content">${body}</div>
              <section class="tc-static-learning-exercise" aria-labelledby="static-exercise-title"><h2 id="static-exercise-title">Interactive exercise</h2><p>This lesson uses a <strong>${htmlEscape(page.interactive.kind)}</strong> interaction. JavaScript adds feedback and progress persistence; the reading contract remains available here.</p>${page.interactive.kind === "code" ? `<p>Exercise ID: <code>${htmlEscape(page.interactive.exercise_id)}</code>. The bounded runner is <code>/api/learn/exercises/run</code>.</p>` : ""}</section>
              <section class="tc-static-learning-evidence" aria-labelledby="static-evidence-title"><h2 id="static-evidence-title">Source and evidence</h2><div class="tc-static-evidence-grid"><div><h3>Sources</h3><ul>${sourceLinks}</ul></div><div><h3>Tests</h3><ul>${tests}</ul></div><div><h3>Benchmarks</h3><ul>${benchmarks}</ul></div><div><h3>Gaps</h3><ul>${gaps}</ul></div></div></section>
              <nav class="tc-static-learning-next" aria-label="Lesson progression"><a href="${prev}">← Previous or prerequisites</a><a href="${next}">Next lesson →</a></nav>
            </article>
          </div>
        </main>
      </div>
    </div>
    <script type="module" src="/src/main.tsx"></script>
  </body>
</html>
`;
}

function staticIndex(allPages, phases) {
  const phaseLinks = phases.map((phase) => `<li><strong>Phase ${String(phase.ordinal).padStart(2, "0")}</strong> ${htmlEscape(phase.name)} — <a href="/learn/eecs/${phase.lessons[0].id}">start lesson</a></li>`).join("");
  const pathCards = [
    ["Beginner", "representation → first TCL program → system closure", "/learn/beginner/representation-boundaries"],
    ["Programmer", "authority, language, VM, compiler, runtime, apps, and debugging", "/learn/programmer/authority-claims"],
    ["EECS/systems", "every phase, lesson, source, test, gap, and release boundary", "/learn/eecs/authority-claims"],
  ].map(([title, description, href]) => `<article class="tc-static-card"><span class="tc-kicker">${title}</span><h2>${description}</h2><a class="tc-button" href="${href}">Open path</a></article>`).join("");
  return `<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>Learn Trit · TreatCode</title>
    <meta name="description" content="A complete, repository-backed Trit computer-systems curriculum with 21 phases and 42 lessons." />
    <link rel="stylesheet" href="/public-shell.css" />
  </head>
  <body>
    <div id="root">
      <div class="tc-static-shell">
        <header class="tc-static-nav"><a class="tc-wordmark" href="/">TREATCODE</a><nav class="tc-static-links" aria-label="Primary navigation"><a href="/stack">Stack Explorer</a><a href="/learn" aria-current="page">Learn</a><a href="/practice">Practice</a><a href="/api/public/v1/openapi.json">API</a></nav></header>
        <main class="tc-static-hero"><span class="tc-kicker">Evidence-linked learning</span><h1>Learn the whole Trit stack.</h1><p>Start with representation, trace the ISA, VM, compiler, runtime, boot, kernel, storage, devices, user space, packaging, products, security, performance, and closure. Every lesson names its current implementation, planned work, exact source, and validation evidence.</p><div class="tc-static-actions"><a class="tc-button" href="/learn/beginner/representation-boundaries">Start with representation</a><a class="tc-button secondary" href="/stack">Open Stack Explorer</a></div></main>
        <section class="tc-static-grid" aria-label="Learning coverage"><div class="tc-static-card"><strong>21</strong><span>registry phases</span></div><div class="tc-static-card"><strong>42</strong><span>canonical lessons</span></div><div class="tc-static-card"><strong>3</strong><span>ordered paths</span></div><div class="tc-static-card"><strong>75+</strong><span>glossary terms</span></div></section>
        <section class="tc-static-grid" aria-label="Learning paths">${pathCards}</section>
        <section class="tc-static-note"><h2>Complete phase inventory</h2><ol>${phaseLinks}</ol><p>JavaScript enhances the reading route with progress, traces, exercise feedback, and evidence transitions. These direct links retain the lesson text when JavaScript is disabled.</p></section>
        <footer class="tc-static-footer">Repository-backed curriculum · source and tests are commit-addressed in the public snapshot.</footer>
      </div>
    </div>
    <script type="module" src="/src/main.tsx"></script>
  </body>
</html>
`;
}

function main() {
  const registry = readJson(registryPath);
  const testsRegistry = readJson(testsRegistryPath);
  const benchmarksRegistry = readJson(benchmarksRegistryPath);
  const gapsRegistry = readJson(gapsRegistryPath);
  const snapshot = readJson(snapshotPath);
  const nodes = [...registry.data].sort((left, right) => Number(left.ordinal) - Number(right.ordinal));
  if (nodes.length !== PHASE_DEFINITIONS.length) throw new Error(`Learning contract has ${PHASE_DEFINITIONS.length} phases but registry has ${nodes.length}`);
  const definitionsBySlug = new Map(PHASE_DEFINITIONS.map((phase) => [phase.slug, phase]));
  for (const node of nodes) if (!definitionsBySlug.has(node.slug)) throw new Error(`No learning contract exists for stack phase ${node.slug}`);
  const commit = snapshot.snapshot?.commit && snapshot.snapshot.commit !== "unknown" ? snapshot.snapshot.commit : registry.snapshot?.commit || "main";
  const canonicalPages = writeSource ? new Map() : loadCanonicalPages();
  if (writeSource) {
    fs.mkdirSync(contentRoot, { recursive: true });
    for (const entry of fs.readdirSync(contentRoot, { withFileTypes: true })) {
      if (entry.isFile() && entry.name.endsWith(".md")) fs.unlinkSync(path.join(contentRoot, entry.name));
    }
  }
  const pagesByPhase = [];
  const pageIds = [];
  for (const node of nodes) {
    const phase = definitionsBySlug.get(node.slug);
    const ids = phase.lessons.map((lesson) => lesson.id);
    pageIds.push(ids);
    pagesByPhase.push({ node, phase });
  }
  const pages = [];
  for (const { node, phase } of pagesByPhase) {
    for (const lesson of phase.lessons) {
      const filename = `${String(node.ordinal + 1).padStart(2, "0")}-${lesson.id}.md`;
      if (writeSource) {
        const meta = pageMeta(phase, lesson, node, pageIds, commit);
        const body = markdownBody({ phase, lesson, node, page: meta, commit });
        fs.writeFileSync(path.join(contentRoot, filename), `---\n${JSON.stringify(meta, null, 2)}\n---\n\n${body}`, "utf8");
        pages.push({ ...meta, content: body, filename, node, phase });
      } else {
        const page = canonicalPages.get(lesson.id);
        if (!page) throw new Error(`Missing canonical learning source for ${lesson.id}; run npm run generate:learning to create it intentionally`);
        if (page.phase_id !== node.id) throw new Error(`Learning source ${lesson.id} belongs to ${page.phase_id}, expected ${node.id}`);
        pages.push({ ...page, filename, node, phase });
      }
    }
  }
  if (!writeSource) {
    const expectedIds = new Set(pages.map((page) => page.id));
    const extraPages = [...canonicalPages.keys()].filter((id) => !expectedIds.has(id));
    if (extraPages.length) throw new Error(`Canonical learning sources are not in the curriculum contract: ${extraPages.join(", ")}`);
  }
  const pageById = new Map(pages.map((page) => [page.id, page]));
  const allPageIds = pages.map((page) => page.id);
  const beginnerPageIds = [pageIds[1][0], pageIds[0][0], pageIds[0][1], pageIds[1][1], ...pageIds.slice(2).flat()];
  const orderedPageIds = allPageIds;
  const pathData = [
    {
      id: "beginner",
      title: "Beginner path",
      audience: "I am new to ternary computing",
      description: "Start at representation, reach a first TCL program, then continue through compiler, boot, kernel, storage, devices, user space, packaging, products, and closure.",
      page_ids: beginnerPageIds,
      starting_phase: nodes[1].id,
      terminal_lesson_id: "full-system-validation",
      required_phase_coverage: "all",
      capstone: "full-system-validation",
    },
    {
      id: "programmer",
      title: "Programmer path",
      audience: "I already write systems or application code",
      description: "Use the full phase map to connect language, VM, compiler, runtime, SDK, apps, debugging, and OS evidence.",
      page_ids: orderedPageIds,
      starting_phase: nodes[1].id,
      terminal_lesson_id: "full-system-validation",
      required_phase_coverage: "all",
      capstone: "full-system-validation",
    },
    {
      id: "eecs",
      title: "EECS/systems path",
      audience: "I want implementation and validation evidence",
      description: "Read every canonical lesson from authority through representation, hardware, ISA, VM, compiler, kernel, storage, devices, release, security, and performance closure.",
      page_ids: orderedPageIds,
      starting_phase: nodes[0].id,
      terminal_lesson_id: "full-system-validation",
      required_phase_coverage: "all",
      capstone: "full-system-validation",
    },
  ];
  const prerequisiteGraph = pages.flatMap((page) => page.prerequisites.map((from) => ({ from, to: page.id })));
  const glossary = [];
  for (const { node, phase } of pagesByPhase) {
    const canonical = phase.lessons[0].id;
    const sourcePath = sourceReferences(node)[0]?.path || "TEST_MANIFEST.json";
    for (const term of phase.terms) {
      glossary.push({
        term,
        definition: phase.termNotes[term] || `${term} is a phase-specific contract in ${node.name}; read it with the implementation and validation evidence rather than treating the label as a generic synonym.`,
        phase_id: node.id,
        page_id: canonical,
        source_paths: [sourcePath],
        related_terms: phase.terms.filter((candidate) => candidate !== term).slice(0, 3),
      });
    }
  }
  const matrix = {
    schema: "trit.treatcode_curriculum_matrix.v1",
    generated_from: "treatcode/public/api/v1/stack_nodes.json",
    snapshot: registry.snapshot,
    phase_count: nodes.length,
    lesson_count: pages.length,
    phases: pagesByPhase.map(({ node, phase }) => ({
      phase_id: node.id,
      ordinal: node.ordinal,
      slug: node.slug,
      name: node.name,
      required_focus: phase.focus,
      entry: phase.entry,
      exit: phase.exit,
      implementation_status: node.coverage?.implemented?.status || "recorded",
      lessons: phase.lessons.map((lesson) => {
        const page = pageById.get(lesson.id);
        return {
          id: lesson.id,
          title: lesson.title,
          kind: lesson.kind,
          objectives: page.objectives,
          sources: page.sources.map((item) => item.path),
          evidence: page.evidence.map((item) => item.path),
          test_ids: page.test_ids,
          benchmark_ids: page.benchmark_ids,
          gap_ids: page.gap_ids,
          exercise: {
            interaction_kind: page.interactive.kind,
            execution_kind: page.interactive.kind === "code" ? "bounded-compiler-vm" : page.interactive.kind === "trace" ? "deterministic-state-trace" : page.interactive.kind === "source" ? "repository-source-investigation" : "conceptual-check",
            id: page.interactive.exercise_id || page.id,
          },
          prerequisites: page.prerequisites,
          next: page.next,
          stack_phase_id: page.phase_id,
        };
      }),
      canonical_lesson_ids: phase.lessons.map((lesson) => lesson.id),
      glossary_terms: phase.terms,
      source_paths: sourceReferences(node).map((item) => item.path),
      test_ids: node.test_ids || [],
      benchmark_ids: node.benchmark_ids || [],
      gap_ids: node.gap_ids || [],
    })),
    paths: pathData.map((item) => ({ id: item.id, page_ids: item.page_ids, terminal_lesson_id: item.terminal_lesson_id, required_phase_coverage: item.required_phase_coverage })),
    glossary_count: glossary.length,
    prerequisite_edge_count: prerequisiteGraph.length,
  };
  const catalog = {
    schema: "trit.treatcode_learning_catalog.v2",
    generated_from: "treatcode/src/content/learn/P05_CURRICULUM_MATRIX.json",
    snapshot_commit: commit,
    phase_ids: nodes.map((node) => node.id),
    paths: pathData,
    prerequisite_graph: prerequisiteGraph,
    glossary,
    interactive_module_kinds: ["choice", "trace", "source", "code"],
    counts: { phases: nodes.length, lessons: pages.length, glossary_terms: glossary.length },
  };
  writeJson(path.join(contentRoot, "P05_CURRICULUM_MATRIX.json"), matrix);
  writeJson(path.join(contentRoot, "learning-catalog.json"), catalog);
  writeJson(path.join(appRoot, "src", "generated", "learning-snapshot.json"), snapshot);
  const allStaticPages = pages.map((page) => ({ id: page.id, title: page.title }));
  fs.rmSync(routeRoot, { recursive: true, force: true });
  fs.mkdirSync(routeRoot, { recursive: true });
  fs.writeFileSync(path.join(routeRoot, "index.html"), staticIndex(allStaticPages, nodes.map((node) => ({ ...node, name: node.name, lessons: pageIds[node.ordinal] }))), "utf8");
  for (const page of pages) {
    const routePage = path.join(routeRoot, "eecs", page.id, "index.html");
    fs.mkdirSync(path.dirname(routePage), { recursive: true });
    fs.writeFileSync(routePage, staticShell({ title: page.title, body: markdownHtml(page.content), pathValue: `/learn/eecs/${page.id}`, phase: page.node, page, allPages: allStaticPages, pathItem: pathData.find((item) => item.id === "eecs") }), "utf8");
  }
  for (const pathItem of pathData) {
    for (const pageId of pathItem.page_ids) {
      const page = pageById.get(pageId);
      const routePage = path.join(routeRoot, pathItem.id, page.id, "index.html");
      fs.mkdirSync(path.dirname(routePage), { recursive: true });
      fs.writeFileSync(routePage, staticShell({ title: page.title, body: markdownHtml(page.content), pathValue: `/learn/${pathItem.id}/${page.id}`, phase: page.node, page, allPages: allStaticPages, pathItem }), "utf8");
    }
  }
  const staticRouteCount = new Set(pathData.flatMap((pathItem) => pathItem.page_ids.map((pageId) => `${pathItem.id}/${pageId}`))).size;
  console.log(`Generated ${nodes.length} phases, ${pages.length} lessons, ${glossary.length} glossary terms, and ${staticRouteCount} static lesson routes.`);
  console.log(`Registry records: ${nodes.length} phases, ${testsRegistry.data.length} tests, ${benchmarksRegistry.data.length} benchmarks, ${gapsRegistry.data.length} gaps.`);
}

main();
