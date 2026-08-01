const u=`---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "representation-boundaries",
  "title": "Representation boundaries",
  "module": "1. Representation",
  "level": "beginner",
  "order": 10,
  "summary": "Learn why numeric, lane, and hardware-facing trit representations are separate contracts.",
  "prerequisites": [],
  "sources": [
    { "path": "docs/00_Quick_Ref/trit_encoding.md", "label": "Trit encoding quick reference", "kind": "documentation" },
    { "path": "ternary_scalar.h", "label": "Positional numeric storage", "kind": "source" },
    { "path": "ternary_lanes.h", "label": "Packed lane storage", "kind": "source" }
  ],
  "evidence": [
    { "path": "tests/test_ternary_lanes.cpp", "label": "Lane conversion regression tests", "kind": "test" },
    { "path": "tests/test_native_ops.cpp", "label": "Native numeric operation tests", "kind": "test" }
  ],
  "next": "tritwise-operations",
  "interactive": {
    "kind": "choice",
    "title": "Boundary check",
    "prompt": "Which representation is the numeric source of truth for scalar arithmetic?",
    "options": ["Two bits per trit", "Positional base-3 storage", "The HAL enum"],
    "answer": 1,
    "explanation": "Scalar arithmetic uses positional base-3 values. Packed lanes and enums are different interface or transport representations."
  }
}
---

## Why this comes first

A trit is a value with three possible numeric states: \`-1\`, \`0\`, and \`+1\`.
The project stores those states in binary host memory, but the host container is
not the meaning of the value. Keeping that boundary explicit prevents a lane
payload from being mistaken for an integer.

## The three representations

| Representation | What it is for | Important rule |
|---|---|---|
| Positional base-3 | Scalar numeric arithmetic | Decode each digit as \`stored_digit - 1\`. |
| Two-bit lane | SIMD, GPU, vector, and packed tritwise transport | \`00=-1\`, \`01=0\`, \`10=+1\`, and \`11\` is invalid. |
| Trit enum | Hardware-facing helper APIs | It names a state; it is not numeric storage. |

\`T40\` and \`T50\` are numeric widths. \`l40\` and \`l50\` have the same trit counts
but use lane encoding. The width name alone does not tell you which semantics
apply.

## Prerequisites

None. Start here if balanced ternary, lanes, or the VM are new to you.

## Production and evidence

Read the quick reference and then compare the scalar and lane headers. The
linked regression tests are the shortest executable proof that conversion is
intentional rather than a display convention.

## Next step

Continue to **Tritwise operations** and practice applying an operation to each
lane without doing scalar arithmetic on the packed payload.
`,m=`---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "tritwise-operations",
  "title": "Tritwise operations",
  "module": "2. Operations",
  "level": "beginner",
  "order": 20,
  "summary": "Separate per-trit logic from numeric arithmetic and understand invalid lane states.",
  "prerequisites": ["representation-boundaries"],
  "sources": [
    { "path": "ternary_lanes.h", "label": "Lane operations and encoding", "kind": "source" },
    { "path": "ternary_native_ops.h", "label": "Numeric arithmetic operations", "kind": "source" },
    { "path": "ternary_simd.h", "label": "Vector operation surface", "kind": "source" }
  ],
  "evidence": [
    { "path": "tests/test_ternary_lanes.cpp", "label": "Lane operation tests", "kind": "test" },
    { "path": "tests/test_native_ops.cpp", "label": "Numeric operation tests", "kind": "test" }
  ],
  "next": "first-tcl-program",
  "interactive": {
    "kind": "choice",
    "title": "Lane safety check",
    "prompt": "What should happen when a two-bit lane contains the pair 11?",
    "options": ["Treat it as +1", "Treat it as zero", "Reject it as invalid"],
    "answer": 2,
    "explanation": "The pair 11 is reserved as an invalid lane value so malformed transport data can be detected instead of silently becoming a number."
  }
}
---

## Two different kinds of operation

Numeric addition interprets a word as a number. A tritwise operation combines
corresponding trits and preserves the lane boundary. For example, \`tladd.l20\`
and \`tland.l20\` work on packed lanes, while \`vadd.t40\` performs numeric
arithmetic per vector lane.

Do not implement a lane operation with ordinary host \`+\`, \`&\`, or \`|\` on the
packed integer. Those operators can move carries or combine the two-bit
encoding instead of applying the specified trit truth table.

## A useful review question

When reading a function, ask: **does this code need the value of the number, or
does it need the state of each trit?** The answer chooses numeric conversion or
lane-preserving operations.

## Prerequisites

Complete **Representation boundaries** first. You should be able to explain
why \`t20\` and \`l20\` are not interchangeable even though both contain 20 trits.

## Production and evidence

The lane and native-operation headers define the behavior; the focused tests
exercise conversions, invalid patterns, and arithmetic separately. This page
does not copy those implementations into the website.

## Next step

Continue to **Your first TCL program**, where the same distinction appears in a
source language type rather than a C++ helper.
`,g=`---
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
    "starter": "import ulib;\\n\\nfn first_value() -> t40 {\\n    return 1 + 2;\\n}",
    "expectedIncludes": ["fn first_value", "return 1 + 2"],
    "explanation": "This small check confirms the shape of a TCL function. Use Open challenges to send a real solution through the TreatCode compiler and VM."
  }
}
---

## The smallest useful program

TCL source files use the \`.trit\` extension. A function has a name, typed
parameters, a return type, and a body:

\`\`\`tcl
import ulib;

fn first_value() -> t40 {
    return 1 + 2;
}
\`\`\`

The example returns a numeric \`t40\`. It does not pretend that a host integer is
the language's type system; the compiler still sees the declared ternary width.

## Prerequisites

Know the numeric/lane distinction and the idea that \`t1\` is a three-valued
predicate. No local compiler setup is required for this page: use the guided
check, then use the in-app challenge surface to compile and run a solution.

## What to inspect next

The language source is parsed into an AST, checked, lowered, and eventually
assembled. The compiler and VM pages follow that path while keeping the
production files and test evidence beside the explanation.

## Next step

Continue to **ISA and VM execution** to see what the function becomes after
source-level checking.
`,f=`---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "isa-vm-execution",
  "title": "ISA and VM execution",
  "module": "4. Execution",
  "level": "programmer",
  "order": 40,
  "summary": "Trace a 27-trit instruction word through decode, registers, memory, and three-way control flow.",
  "prerequisites": ["tritwise-operations"],
  "sources": [
    { "path": "ternary_isa.h", "label": "Instruction formats and opcodes", "kind": "source" },
    { "path": "ternary_vm_state.h", "label": "VM state model", "kind": "source" },
    { "path": "ternary_vm.h", "label": "VM execution loop", "kind": "source" },
    { "path": "docs/00_Quick_Ref/opcode_table.md", "label": "Opcode quick reference", "kind": "documentation" }
  ],
  "evidence": [
    { "path": "tests/test_isa_asm.cpp", "label": "ISA and assembler tests", "kind": "test" },
    { "path": "tests/test_vm_widths.cpp", "label": "VM width tests", "kind": "test" }
  ],
  "next": "compiler-pipeline",
  "interactive": {
    "kind": "choice",
    "title": "Decode check",
    "prompt": "What does the format trit of a TritWord27 choose?",
    "options": ["The VM memory size", "R-, I-, or B-type instruction shape", "The host CPU endian order"],
    "answer": 1,
    "explanation": "The format trit distinguishes register, immediate, and branch forms before the rest of the instruction fields are decoded."
  }
}
---

## State before instructions

\`VMState\` keeps a program counter, 27 general-purpose registers, a separate
trap register, instruction memory, word-addressed data memory, CSRs, and vector
registers. Instruction and data memory are separate so a data store cannot
rewrite the program being executed.

## Three-way control

\`TCMP\` produces \`-1\`, \`0\`, or \`+1\`. The branch family can select one of those
three states, and \`TSEL\` can choose a value without a branch. That is a machine
level consequence of \`t1\` being a trit, not a boolean stored in disguise.

## Instruction shape

The ISA stores a 27-trit instruction in 54 meaningful host bits. Its format trit
selects R-type, I-type, or B-type decoding. Register fields store an offset from
the middle register index, and immediates use signed balanced ternary fields.

## Prerequisites

Understand lanes and \`t1\` predicates. The first TCL program provides the source
shape; this page provides the execution shape.

## Next step

Continue to **Compiler pipeline** and follow how source is transformed into
these instruction words.
`,b=`---
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

\`\`\`text
.trit source -> tokens -> ModuleAst -> type checks -> structural IR
  -> optimization -> register allocation -> TASM -> assembler/linker image
\`\`\`

The type layer catches wrong widths, invalid conditions, unsafe operations,
pointer-state mistakes, and moved ownership before code generation. This is why
the compiler is more than a text-to-text translator.

## Prerequisites

Read the first TCL and ISA pages. You should know both the source shape and the
instruction shape before studying the lowering boundary.

## Production and evidence

The header files are the current host compiler implementation. The native
\`tcl_*.trit\` files mirror that architecture for the in-OS path; the focused
compiler and assembler tests show which behavior is currently validated.

## Next step

Continue to **Boot and traps** and inspect what happens when an encoded image
crosses into execution.
`,v=`---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "boot-and-traps",
  "title": "Boot and traps",
  "module": "6. Boot boundary",
  "level": "programmer",
  "order": 60,
  "summary": "Understand image headers, boot handoff, invalid instructions, and why traps are evidence rather than silent failure.",
  "prerequisites": ["compiler-pipeline"],
  "sources": [
    { "path": "bootloader.tasm", "label": "Bootloader entry sequence", "kind": "source" },
    { "path": "native_kernel_boot.tasm", "label": "Native kernel boot stub", "kind": "source" },
    { "path": "native_kernel_trap_stub.tasm", "label": "Native trap stub", "kind": "source" },
    { "path": "executable_header_v2.h", "label": "Executable header contract", "kind": "source" },
    { "path": "IMAGE_FORMAT_MANIFEST.json", "label": "Boot image format manifest", "kind": "manifest" }
  ],
  "evidence": [
    { "path": "tests/test_formats.cpp", "label": "Image format tests", "kind": "test" },
    { "path": "tests/test_host_runtime.cpp", "label": "Host boot/runtime tests", "kind": "test" }
  ],
  "next": "kernel-syscalls",
  "interactive": {
    "kind": "choice",
    "title": "Trap check",
    "prompt": "What is the correct response to the invalid lane pair 11 in an instruction word?",
    "options": ["Decode it as +1", "Ignore the instruction", "Raise the illegal-operation trap"],
    "answer": 2,
    "explanation": "Reserved encodings become observable trap behavior. Silent coercion would hide a malformed image or a compiler/assembler bug."
  }
}
---

## The handoff

Compilation produces an executable image; the bootloader validates its header,
loads its sections, and transfers control to the kernel or program entry. The
\`.tboot\` and \`.tdisk\` formats are contracts, not opaque blobs.

## Traps make failure inspectable

An invalid instruction encoding, malformed lane, or forbidden memory operation
must become a recorded trap. The VM and native trap stubs provide a controlled
boundary for diagnostics and recovery. A test that only checks the happy path
would not establish this contract.

## Prerequisites

Know the compiler output and the 27-trit instruction shape. Then read the image
manifest beside the loader and trap sources.

## Next step

Continue to **Kernel syscalls** to see how valid execution crosses from a user
program into OS services.
`,_=`---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "kernel-syscalls",
  "title": "Kernel and syscalls",
  "module": "7. Operating system",
  "level": "eecs",
  "order": 70,
  "summary": "Trace a user request through the kernel dispatch and the documented syscall ABI.",
  "prerequisites": ["boot-and-traps"],
  "sources": [
    { "path": "kernel.trit", "label": "Kernel dispatch and services", "kind": "source" },
    { "path": "ternary_os.h", "label": "Host OS/runtime surface", "kind": "source" },
    { "path": "SYSCALL_MANIFEST.json", "label": "Syscall ABI manifest", "kind": "manifest" },
    { "path": "apps/os_sdk.trit", "label": "Application SDK wrappers", "kind": "source" }
  ],
  "evidence": [
    { "path": "tests/test_os_platform.cpp", "label": "OS platform tests", "kind": "test" },
    { "path": "tests/test_phase_d_kernel.cpp", "label": "Kernel phase-D tests", "kind": "test" }
  ],
  "next": "storage-and-images",
  "interactive": {
    "kind": "choice",
    "title": "ABI check",
    "prompt": "Where does the syscall service ID live according to the documented ABI?",
    "options": ["CSR syscall_id", "The stack only", "The PC low trits"],
    "answer": 0,
    "explanation": "The syscall ID is in CSR syscall_id. Primary arguments use r13 through r16 and returns use r13 through r15."
  }
}
---

## A syscall is a contract

The kernel is written in TCL and owns process state, the VFS, device-facing
services, and syscall dispatch. Application code should use the SDK wrappers;
the manifest remains the ABI source of truth.

The current documented convention places the service ID in CSR \`syscall_id\`,
primary arguments in \`r13\` through \`r16\`, and returns in \`r13\` (status), \`r14\`
(payload), and \`r15\` (detail). A wrapper is useful only when it preserves those
semantics.

## Prerequisites

Complete the boot and trap page. You should be able to distinguish a malformed
instruction trap from a deliberate kernel service request.

## Production and evidence

Use the kernel, OS runtime, syscall manifest, and SDK together. The focused OS
tests validate the integration boundary; this page does not duplicate kernel
code in the website.

## Next step

Continue to **Storage and images** and follow persistence from a syscall to a
recoverable disk image.
`,y=`---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "storage-and-images",
  "title": "Storage and images",
  "module": "8. Persistence",
  "level": "eecs",
  "order": 80,
  "summary": "Connect word-addressed storage, write-ahead logging, recovery, and release image formats.",
  "prerequisites": ["kernel-syscalls"],
  "sources": [
    { "path": "ternary_redo_wal.h", "label": "Redo write-ahead log", "kind": "source" },
    { "path": "docs/09_Host_Runtime/image_format.md", "label": "Image format explanation", "kind": "documentation" },
    { "path": "IMAGE_FORMAT_MANIFEST.json", "label": "Image format contract", "kind": "manifest" },
    { "path": "build_tos_image.cpp", "label": "Release image builder", "kind": "source" }
  ],
  "evidence": [
    { "path": "tests/test_redo_wal_v2.cpp", "label": "WAL recovery tests", "kind": "test" },
    { "path": "tests/test_formats.cpp", "label": "Image format tests", "kind": "test" }
  ],
  "next": "applications-and-validation",
  "interactive": {
    "kind": "choice",
    "title": "Recovery check",
    "prompt": "Why does the storage layer use a write-ahead log?",
    "options": ["To make every word a lane", "To recover a consistent update after interruption", "To replace the image manifest"],
    "answer": 1,
    "explanation": "The WAL records the intended change before the durable state is rewritten, so recovery can distinguish committed, incomplete, and stale records."
  }
}
---

## Two related contracts

The runtime stores data in words and the release builder packages bootable
sections into \`.tboot\` and \`.tdisk\` images. The image manifest explains the
format; the WAL protects updates within the storage system. They solve different
failure boundaries.

## Recovery is part of the feature

A write that works only when power never fails is not a complete storage
contract. The redo log records enough information to replay or discard an
interrupted update. The focused recovery tests are the evidence to read after
the header and WAL implementation.

## Prerequisites

Know the syscall boundary and the difference between a kernel service and a
release artifact. Then compare the format manifest, image builder, and WAL.

## Next step

Continue to **Applications and validation** to see how apps, manifests, and
process-handoff tests turn the stack into a user-facing release.
`,k=`---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "applications-and-validation",
  "title": "Applications and validation",
  "module": "9. User layer",
  "level": "eecs",
  "order": 90,
  "summary": "Finish the path at bundled apps, image registration, process handoff, and repeatable release checks.",
  "prerequisites": ["storage-and-images"],
  "sources": [
    { "path": "apps/os_sdk.trit", "label": "Application SDK", "kind": "source" },
    { "path": "apps/libwidget.trit", "label": "Widget library", "kind": "source" },
    { "path": "APP_MANIFEST.json", "label": "Bundled app manifest", "kind": "manifest" },
    { "path": "build_tos_image.cpp", "label": "Application image registration", "kind": "source" },
    { "path": "tools/trit-test.ps1", "label": "Release test entry point", "kind": "tool" }
  ],
  "evidence": [
    { "path": "tests/test_native_apps.cpp", "label": "Bundled app tests", "kind": "test" },
    { "path": "tests/test_process_handoff.cpp", "label": "Process handoff tests", "kind": "test" }
  ],
  "next": null,
  "interactive": {
    "kind": "choice",
    "title": "Release check",
    "prompt": "What must change when a new bundled app is added?",
    "options": ["Only the UI label", "The app source, image builder, manifest, and focused tests", "Only a Graphify snapshot"],
    "answer": 1,
    "explanation": "A bundled app is a release contract. Its source, image registration, APP_MANIFEST entry, and focused tests must stay aligned."
  }
}
---

## The user-facing end of the stack

Apps compile against the SDK and shared widget/library code. The image builder
registers them, \`APP_MANIFEST.json\` records their guest paths and metadata, and
process-handoff tests verify that a release can actually launch them.

## Validation loop

For a source or manifest change, use the smallest focused test first, then run
the release checks. The repository's doctor, smoke, production, and image
inspection tools are the evidence trail; Graphify remains advisory navigation.

## Prerequisites

Complete storage and images. You should now be able to name the source,
contract, and test that sit at each layer of the path.

## You reached implementation

The EECS path ends at actual app registration and validation evidence. From here
you can choose a challenge, inspect a source symbol, or pick a known gap from
the repository roadmap instead of treating the guide as a replacement for the
implementation.
`,w=[{id:"beginner",title:"Beginner path",audience:"I am new to ternary computing",description:"Build a correct mental model, then write and validate a first TCL function in the browser.",page_ids:["representation-boundaries","tritwise-operations","first-tcl-program"]},{id:"programmer",title:"Programmer path",audience:"I already write systems or application code",description:"Trace a program from source through the ISA, VM, compiler, and boot boundary.",page_ids:["representation-boundaries","tritwise-operations","first-tcl-program","isa-vm-execution","compiler-pipeline","boot-and-traps"]},{id:"eecs",title:"EECS path",audience:"I want implementation and validation evidence",description:"Follow the full silicon-to-user path through kernel services, storage, images, and applications.",page_ids:["representation-boundaries","tritwise-operations","isa-vm-execution","compiler-pipeline","boot-and-traps","kernel-syscalls","storage-and-images","applications-and-validation"]}],T={paths:w},x=Object.assign({"./content/learn/01-representation-boundaries.md":u,"./content/learn/02-tritwise-operations.md":m,"./content/learn/03-first-tcl-program.md":g,"./content/learn/04-isa-vm-execution.md":f,"./content/learn/05-compiler-pipeline.md":b,"./content/learn/06-boot-and-traps.md":v,"./content/learn/07-kernel-syscalls.md":_,"./content/learn/08-storage-and-images.md":y,"./content/learn/09-applications-and-validation.md":k});function A(t,n){const i=t.match(/^---\r?\n([\s\S]*?)\r?\n---\r?\n([\s\S]*)$/);if(!i)throw new Error(`Learning document ${n} is missing JSON front matter`);let e;try{e=JSON.parse(i[1])}catch(a){throw new Error(`Learning document ${n} has invalid JSON front matter: ${String(a)}`)}return{...e,content:i[2].trim()}}const I=T,L=Object.entries(x).map(([t,n])=>A(n,t)).sort((t,n)=>t.order-n.order);function S(t){return t.split("|").map(n=>n.trim()).filter((n,i,e)=>i>0&&i<e.length-1)}function C(t){return t.length>0&&t.every(n=>/^:?-{3,}:?$/.test(n))}function P(t){var c,d;const n=t.split(/\r?\n/),i=[];let e=null;const a=()=>{e&&(i.push(e),e=null)};for(const l of n){const r=l.trim();if(r.startsWith("```")){(e==null?void 0:e.type)==="code"?a():(a(),e={type:"code",content:"",lang:r.substring(3).trim()});continue}if((e==null?void 0:e.type)==="code"){e.content+=e.content?`
${l}`:l;continue}if(!r){a();continue}const s=r.match(/^(#{2,4})\s+(.+)$/);if(s){a(),e={type:s[1].length===2?"h2":s[1].length===3?"h3":"h4",content:s[2].trim()},a();continue}if(r.startsWith("|")){const o=S(r);if(C(o))continue;!e||e.type!=="table"?(a(),e={type:"table",content:"",headers:o,rows:[]}):(e.rows=e.rows||[],e.rows.push(o));continue}const p=r.match(/^[-*]\s+(.+)$/);if(p){(!e||e.type!=="ul")&&(a(),e={type:"ul",content:"",items:[]}),(c=e.items)==null||c.push(p[1]);continue}const h=r.match(/^\d+\.\s+(.+)$/);if(h){(!e||e.type!=="ol")&&(a(),e={type:"ol",content:"",items:[]}),(d=e.items)==null||d.push(h[1]);continue}!e||e.type!=="p"?(a(),e={type:"p",content:r}):e.content+=` ${r}`}return a(),i}export{I as L,L as a,P as p};
