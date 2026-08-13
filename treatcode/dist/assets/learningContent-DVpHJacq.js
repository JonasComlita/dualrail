const u=`---\r
{\r
  "schema": "trit.treatcode_learning_page.v1",\r
  "id": "representation-boundaries",\r
  "title": "Representation boundaries",\r
  "module": "1. Representation",\r
  "level": "beginner",\r
  "order": 10,\r
  "summary": "Learn why numeric, lane, and hardware-facing trit representations are separate contracts.",\r
  "prerequisites": [],\r
  "sources": [\r
    { "path": "docs/00_Quick_Ref/trit_encoding.md", "label": "Trit encoding quick reference", "kind": "documentation" },\r
    { "path": "ternary_scalar.h", "label": "Positional numeric storage", "kind": "source" },\r
    { "path": "ternary_lanes.h", "label": "Packed lane storage", "kind": "source" }\r
  ],\r
  "evidence": [\r
    { "path": "tests/test_ternary_lanes.cpp", "label": "Lane conversion regression tests", "kind": "test" },\r
    { "path": "tests/test_native_ops.cpp", "label": "Native numeric operation tests", "kind": "test" }\r
  ],\r
  "next": "tritwise-operations",\r
  "interactive": {\r
    "kind": "choice",\r
    "title": "Boundary check",\r
    "prompt": "Which representation is the numeric source of truth for scalar arithmetic?",\r
    "options": ["Two bits per trit", "Positional base-3 storage", "The HAL enum"],\r
    "answer": 1,\r
    "explanation": "Scalar arithmetic uses positional base-3 values. Packed lanes and enums are different interface or transport representations."\r
  }\r
}\r
---\r
\r
## Why this comes first\r
\r
A trit is a value with three possible numeric states: \`-1\`, \`0\`, and \`+1\`.\r
The project stores those states in binary host memory, but the host container is\r
not the meaning of the value. Keeping that boundary explicit prevents a lane\r
payload from being mistaken for an integer.\r
\r
## The three representations\r
\r
| Representation | What it is for | Important rule |\r
|---|---|---|\r
| Positional base-3 | Scalar numeric arithmetic | Decode each digit as \`stored_digit - 1\`. |\r
| Two-bit lane | SIMD, GPU, vector, and packed tritwise transport | \`00=-1\`, \`01=0\`, \`10=+1\`, and \`11\` is invalid. |\r
| Trit enum | Hardware-facing helper APIs | It names a state; it is not numeric storage. |\r
\r
\`T40\` and \`T50\` are numeric widths. \`l40\` and \`l50\` have the same trit counts\r
but use lane encoding. The width name alone does not tell you which semantics\r
apply.\r
\r
## Prerequisites\r
\r
None. Start here if balanced ternary, lanes, or the VM are new to you.\r
\r
## Production and evidence\r
\r
Read the quick reference and then compare the scalar and lane headers. The\r
linked regression tests are the shortest executable proof that conversion is\r
intentional rather than a display convention.\r
\r
## Next step\r
\r
Continue to **Tritwise operations** and practice applying an operation to each\r
lane without doing scalar arithmetic on the packed payload.\r
`,m=`---\r
{\r
  "schema": "trit.treatcode_learning_page.v1",\r
  "id": "tritwise-operations",\r
  "title": "Tritwise operations",\r
  "module": "2. Operations",\r
  "level": "beginner",\r
  "order": 20,\r
  "summary": "Separate per-trit logic from numeric arithmetic and understand invalid lane states.",\r
  "prerequisites": ["representation-boundaries"],\r
  "sources": [\r
    { "path": "ternary_lanes.h", "label": "Lane operations and encoding", "kind": "source" },\r
    { "path": "ternary_native_ops.h", "label": "Numeric arithmetic operations", "kind": "source" },\r
    { "path": "ternary_simd.h", "label": "Vector operation surface", "kind": "source" }\r
  ],\r
  "evidence": [\r
    { "path": "tests/test_ternary_lanes.cpp", "label": "Lane operation tests", "kind": "test" },\r
    { "path": "tests/test_native_ops.cpp", "label": "Numeric operation tests", "kind": "test" }\r
  ],\r
  "next": "first-tcl-program",\r
  "interactive": {\r
    "kind": "choice",\r
    "title": "Lane safety check",\r
    "prompt": "What should happen when a two-bit lane contains the pair 11?",\r
    "options": ["Treat it as +1", "Treat it as zero", "Reject it as invalid"],\r
    "answer": 2,\r
    "explanation": "The pair 11 is reserved as an invalid lane value so malformed transport data can be detected instead of silently becoming a number."\r
  }\r
}\r
---\r
\r
## Two different kinds of operation\r
\r
Numeric addition interprets a word as a number. A tritwise operation combines\r
corresponding trits and preserves the lane boundary. For example, \`tladd.l20\`\r
and \`tland.l20\` work on packed lanes, while \`vadd.t40\` performs numeric\r
arithmetic per vector lane.\r
\r
Do not implement a lane operation with ordinary host \`+\`, \`&\`, or \`|\` on the\r
packed integer. Those operators can move carries or combine the two-bit\r
encoding instead of applying the specified trit truth table.\r
\r
## A useful review question\r
\r
When reading a function, ask: **does this code need the value of the number, or\r
does it need the state of each trit?** The answer chooses numeric conversion or\r
lane-preserving operations.\r
\r
## Prerequisites\r
\r
Complete **Representation boundaries** first. You should be able to explain\r
why \`t20\` and \`l20\` are not interchangeable even though both contain 20 trits.\r
\r
## Production and evidence\r
\r
The lane and native-operation headers define the behavior; the focused tests\r
exercise conversions, invalid patterns, and arithmetic separately. This page\r
does not copy those implementations into the website.\r
\r
## Next step\r
\r
Continue to **Your first TCL program**, where the same distinction appears in a\r
source language type rather than a C++ helper.\r
`,g=`---\r
{\r
  "schema": "trit.treatcode_learning_page.v1",\r
  "id": "first-tcl-program",\r
  "title": "Your first TCL program",\r
  "module": "3. Language",\r
  "level": "beginner",\r
  "order": 30,\r
  "summary": "Write a minimal .trit function and move from a guided example into a runnable TreatCode challenge.",\r
  "prerequisites": ["tritwise-operations"],\r
  "sources": [\r
    { "path": "TCL_Spec_1.0.md", "label": "TCL 1.0 language specification", "kind": "specification" },\r
    { "path": "tcl_lexer.trit", "label": "Native TCL lexer", "kind": "source" },\r
    { "path": "tcl_parser.trit", "label": "Native TCL parser", "kind": "source" },\r
    { "path": "treatcode/src/App.tsx", "label": "TreatCode challenge surface", "kind": "product" }\r
  ],\r
  "evidence": [\r
    { "path": "tests/test_compiler_frontend_smoke.trit", "label": "Compiler frontend smoke input", "kind": "test" },\r
    { "path": "tests/test_phase7_compiler.cpp", "label": "Compiler and runtime tests", "kind": "test" }\r
  ],\r
  "next": "isa-vm-execution",\r
  "interactive": {\r
    "kind": "code",\r
    "title": "Try the first function",\r
    "starter": "import ulib;\\n\\nfn first_value() -> t40 {\\n    return 1 + 2;\\n}",\r
    "expectedIncludes": ["fn first_value", "return 1 + 2"],\r
    "explanation": "This small check confirms the shape of a TCL function. Use Open challenges to send a real solution through the TreatCode compiler and VM."\r
  }\r
}\r
---\r
\r
## The smallest useful program\r
\r
TCL source files use the \`.trit\` extension. A function has a name, typed\r
parameters, a return type, and a body:\r
\r
\`\`\`tcl\r
import ulib;\r
\r
fn first_value() -> t40 {\r
    return 1 + 2;\r
}\r
\`\`\`\r
\r
The example returns a numeric \`t40\`. It does not pretend that a host integer is\r
the language's type system; the compiler still sees the declared ternary width.\r
\r
## Prerequisites\r
\r
Know the numeric/lane distinction and the idea that \`t1\` is a three-valued\r
predicate. No local compiler setup is required for this page: use the guided\r
check, then use the in-app challenge surface to compile and run a solution.\r
\r
## What to inspect next\r
\r
The language source is parsed into an AST, checked, lowered, and eventually\r
assembled. The compiler and VM pages follow that path while keeping the\r
production files and test evidence beside the explanation.\r
\r
## Next step\r
\r
Continue to **ISA and VM execution** to see what the function becomes after\r
source-level checking.\r
`,f=`---\r
{\r
  "schema": "trit.treatcode_learning_page.v1",\r
  "id": "isa-vm-execution",\r
  "title": "ISA and VM execution",\r
  "module": "4. Execution",\r
  "level": "programmer",\r
  "order": 40,\r
  "summary": "Trace a 27-trit instruction word through decode, registers, memory, and three-way control flow.",\r
  "prerequisites": ["tritwise-operations"],\r
  "sources": [\r
    { "path": "ternary_isa.h", "label": "Instruction formats and opcodes", "kind": "source" },\r
    { "path": "ternary_vm_state.h", "label": "VM state model", "kind": "source" },\r
    { "path": "ternary_vm.h", "label": "VM execution loop", "kind": "source" },\r
    { "path": "docs/00_Quick_Ref/opcode_table.md", "label": "Opcode quick reference", "kind": "documentation" }\r
  ],\r
  "evidence": [\r
    { "path": "tests/test_isa_asm.cpp", "label": "ISA and assembler tests", "kind": "test" },\r
    { "path": "tests/test_vm_widths.cpp", "label": "VM width tests", "kind": "test" }\r
  ],\r
  "next": "compiler-pipeline",\r
  "interactive": {\r
    "kind": "choice",\r
    "title": "Decode check",\r
    "prompt": "What does the format trit of a TritWord27 choose?",\r
    "options": ["The VM memory size", "R-, I-, or B-type instruction shape", "The host CPU endian order"],\r
    "answer": 1,\r
    "explanation": "The format trit distinguishes register, immediate, and branch forms before the rest of the instruction fields are decoded."\r
  }\r
}\r
---\r
\r
## State before instructions\r
\r
\`VMState\` keeps a program counter, 27 general-purpose registers, a separate\r
trap register, instruction memory, word-addressed data memory, CSRs, and vector\r
registers. Instruction and data memory are separate so a data store cannot\r
rewrite the program being executed.\r
\r
## Three-way control\r
\r
\`TCMP\` produces \`-1\`, \`0\`, or \`+1\`. The branch family can select one of those\r
three states, and \`TSEL\` can choose a value without a branch. That is a machine\r
level consequence of \`t1\` being a trit, not a boolean stored in disguise.\r
\r
## Instruction shape\r
\r
The ISA stores a 27-trit instruction in 54 meaningful host bits. Its format trit\r
selects R-type, I-type, or B-type decoding. Register fields store an offset from\r
the middle register index, and immediates use signed balanced ternary fields.\r
\r
## Prerequisites\r
\r
Understand lanes and \`t1\` predicates. The first TCL program provides the source\r
shape; this page provides the execution shape.\r
\r
## Next step\r
\r
Continue to **Compiler pipeline** and follow how source is transformed into\r
these instruction words.\r
`,b=`---\r
{\r
  "schema": "trit.treatcode_learning_page.v1",\r
  "id": "compiler-pipeline",\r
  "title": "Compiler pipeline",\r
  "module": "5. Compilation",\r
  "level": "programmer",\r
  "order": 50,\r
  "summary": "Follow TCL from tokens to AST, type checks, IR, register allocation, TASM, and an image.",\r
  "prerequisites": ["first-tcl-program", "isa-vm-execution"],\r
  "sources": [\r
    { "path": "ternary_compiler_lexer.h", "label": "Host compiler lexer", "kind": "source" },\r
    { "path": "ternary_compiler_parser.h", "label": "Host compiler parser", "kind": "source" },\r
    { "path": "ternary_compiler_ast.h", "label": "Compiler AST", "kind": "source" },\r
    { "path": "ternary_compiler_codegen.h", "label": "TASM code generation", "kind": "source" },\r
    { "path": "tritc.cpp", "label": "Compiler driver", "kind": "source" }\r
  ],\r
  "evidence": [\r
    { "path": "tests/test_phase7_compiler.cpp", "label": "Compiler pipeline tests", "kind": "test" },\r
    { "path": "tests/test_tcl_asm.cpp", "label": "TCL assembler tests", "kind": "test" }\r
  ],\r
  "next": "boot-and-traps",\r
  "interactive": {\r
    "kind": "choice",\r
    "title": "Pipeline check",\r
    "prompt": "Which artifact is emitted immediately before the assembler encodes instruction words?",\r
    "options": ["Raw source text", "TASM text and structural IR metadata", "A screenshot of the VM"],\r
    "answer": 1,\r
    "explanation": "The code generator emits textual TASM plus structural metadata; the assembler then resolves labels and encodes TritWord27 values."\r
  }\r
}\r
---\r
\r
## Source to image\r
\r
The reference pipeline is:\r
\r
\`\`\`text\r
.trit source -> tokens -> ModuleAst -> type checks -> structural IR\r
  -> optimization -> register allocation -> TASM -> assembler/linker image\r
\`\`\`\r
\r
The type layer catches wrong widths, invalid conditions, unsafe operations,\r
pointer-state mistakes, and moved ownership before code generation. This is why\r
the compiler is more than a text-to-text translator.\r
\r
## Prerequisites\r
\r
Read the first TCL and ISA pages. You should know both the source shape and the\r
instruction shape before studying the lowering boundary.\r
\r
## Production and evidence\r
\r
The header files are the current host compiler implementation. The native\r
\`tcl_*.trit\` files mirror that architecture for the in-OS path; the focused\r
compiler and assembler tests show which behavior is currently validated.\r
\r
## Next step\r
\r
Continue to **Boot and traps** and inspect what happens when an encoded image\r
crosses into execution.\r
`,v=`---\r
{\r
  "schema": "trit.treatcode_learning_page.v1",\r
  "id": "boot-and-traps",\r
  "title": "Boot and traps",\r
  "module": "6. Boot boundary",\r
  "level": "programmer",\r
  "order": 60,\r
  "summary": "Understand image headers, boot handoff, invalid instructions, and why traps are evidence rather than silent failure.",\r
  "prerequisites": ["compiler-pipeline"],\r
  "sources": [\r
    { "path": "bootloader.tasm", "label": "Bootloader entry sequence", "kind": "source" },\r
    { "path": "native_kernel_boot.tasm", "label": "Native kernel boot stub", "kind": "source" },\r
    { "path": "native_kernel_trap_stub.tasm", "label": "Native trap stub", "kind": "source" },\r
    { "path": "executable_header_v2.h", "label": "Executable header contract", "kind": "source" },\r
    { "path": "IMAGE_FORMAT_MANIFEST.json", "label": "Boot image format manifest", "kind": "manifest" }\r
  ],\r
  "evidence": [\r
    { "path": "tests/test_formats.cpp", "label": "Image format tests", "kind": "test" },\r
    { "path": "tests/test_host_runtime.cpp", "label": "Host boot/runtime tests", "kind": "test" }\r
  ],\r
  "next": "kernel-syscalls",\r
  "interactive": {\r
    "kind": "choice",\r
    "title": "Trap check",\r
    "prompt": "What is the correct response to the invalid lane pair 11 in an instruction word?",\r
    "options": ["Decode it as +1", "Ignore the instruction", "Raise the illegal-operation trap"],\r
    "answer": 2,\r
    "explanation": "Reserved encodings become observable trap behavior. Silent coercion would hide a malformed image or a compiler/assembler bug."\r
  }\r
}\r
---\r
\r
## The handoff\r
\r
Compilation produces an executable image; the bootloader validates its header,\r
loads its sections, and transfers control to the kernel or program entry. The\r
\`.tboot\` and \`.tdisk\` formats are contracts, not opaque blobs.\r
\r
## Traps make failure inspectable\r
\r
An invalid instruction encoding, malformed lane, or forbidden memory operation\r
must become a recorded trap. The VM and native trap stubs provide a controlled\r
boundary for diagnostics and recovery. A test that only checks the happy path\r
would not establish this contract.\r
\r
## Prerequisites\r
\r
Know the compiler output and the 27-trit instruction shape. Then read the image\r
manifest beside the loader and trap sources.\r
\r
## Next step\r
\r
Continue to **Kernel syscalls** to see how valid execution crosses from a user\r
program into OS services.\r
`,_=`---\r
{\r
  "schema": "trit.treatcode_learning_page.v1",\r
  "id": "kernel-syscalls",\r
  "title": "Kernel and syscalls",\r
  "module": "7. Operating system",\r
  "level": "eecs",\r
  "order": 70,\r
  "summary": "Trace a user request through the kernel dispatch and the documented syscall ABI.",\r
  "prerequisites": ["boot-and-traps"],\r
  "sources": [\r
    { "path": "kernel.trit", "label": "Kernel dispatch and services", "kind": "source" },\r
    { "path": "ternary_os.h", "label": "Host OS/runtime surface", "kind": "source" },\r
    { "path": "SYSCALL_MANIFEST.json", "label": "Syscall ABI manifest", "kind": "manifest" },\r
    { "path": "apps/os_sdk.trit", "label": "Application SDK wrappers", "kind": "source" }\r
  ],\r
  "evidence": [\r
    { "path": "tests/test_os_platform.cpp", "label": "OS platform tests", "kind": "test" },\r
    { "path": "tests/test_phase_d_kernel.cpp", "label": "Kernel phase-D tests", "kind": "test" }\r
  ],\r
  "next": "storage-and-images",\r
  "interactive": {\r
    "kind": "choice",\r
    "title": "ABI check",\r
    "prompt": "Where does the syscall service ID live according to the documented ABI?",\r
    "options": ["CSR syscall_id", "The stack only", "The PC low trits"],\r
    "answer": 0,\r
    "explanation": "The syscall ID is in CSR syscall_id. Primary arguments use r13 through r16 and returns use r13 through r15."\r
  }\r
}\r
---\r
\r
## A syscall is a contract\r
\r
The kernel is written in TCL and owns process state, the VFS, device-facing\r
services, and syscall dispatch. Application code should use the SDK wrappers;\r
the manifest remains the ABI source of truth.\r
\r
The current documented convention places the service ID in CSR \`syscall_id\`,\r
primary arguments in \`r13\` through \`r16\`, and returns in \`r13\` (status), \`r14\`\r
(payload), and \`r15\` (detail). A wrapper is useful only when it preserves those\r
semantics.\r
\r
## Prerequisites\r
\r
Complete the boot and trap page. You should be able to distinguish a malformed\r
instruction trap from a deliberate kernel service request.\r
\r
## Production and evidence\r
\r
Use the kernel, OS runtime, syscall manifest, and SDK together. The focused OS\r
tests validate the integration boundary; this page does not duplicate kernel\r
code in the website.\r
\r
## Next step\r
\r
Continue to **Storage and images** and follow persistence from a syscall to a\r
recoverable disk image.\r
`,y=`---\r
{\r
  "schema": "trit.treatcode_learning_page.v1",\r
  "id": "storage-and-images",\r
  "title": "Storage and images",\r
  "module": "8. Persistence",\r
  "level": "eecs",\r
  "order": 80,\r
  "summary": "Connect word-addressed storage, write-ahead logging, recovery, and release image formats.",\r
  "prerequisites": ["kernel-syscalls"],\r
  "sources": [\r
    { "path": "ternary_redo_wal.h", "label": "Redo write-ahead log", "kind": "source" },\r
    { "path": "docs/09_Host_Runtime/image_format.md", "label": "Image format explanation", "kind": "documentation" },\r
    { "path": "IMAGE_FORMAT_MANIFEST.json", "label": "Image format contract", "kind": "manifest" },\r
    { "path": "build_tos_image.cpp", "label": "Release image builder", "kind": "source" }\r
  ],\r
  "evidence": [\r
    { "path": "tests/test_redo_wal_v2.cpp", "label": "WAL recovery tests", "kind": "test" },\r
    { "path": "tests/test_formats.cpp", "label": "Image format tests", "kind": "test" }\r
  ],\r
  "next": "applications-and-validation",\r
  "interactive": {\r
    "kind": "choice",\r
    "title": "Recovery check",\r
    "prompt": "Why does the storage layer use a write-ahead log?",\r
    "options": ["To make every word a lane", "To recover a consistent update after interruption", "To replace the image manifest"],\r
    "answer": 1,\r
    "explanation": "The WAL records the intended change before the durable state is rewritten, so recovery can distinguish committed, incomplete, and stale records."\r
  }\r
}\r
---\r
\r
## Two related contracts\r
\r
The runtime stores data in words and the release builder packages bootable\r
sections into \`.tboot\` and \`.tdisk\` images. The image manifest explains the\r
format; the WAL protects updates within the storage system. They solve different\r
failure boundaries.\r
\r
## Recovery is part of the feature\r
\r
A write that works only when power never fails is not a complete storage\r
contract. The redo log records enough information to replay or discard an\r
interrupted update. The focused recovery tests are the evidence to read after\r
the header and WAL implementation.\r
\r
## Prerequisites\r
\r
Know the syscall boundary and the difference between a kernel service and a\r
release artifact. Then compare the format manifest, image builder, and WAL.\r
\r
## Next step\r
\r
Continue to **Applications and validation** to see how apps, manifests, and\r
process-handoff tests turn the stack into a user-facing release.\r
`,k=`---\r
{\r
  "schema": "trit.treatcode_learning_page.v1",\r
  "id": "applications-and-validation",\r
  "title": "Applications and validation",\r
  "module": "9. User layer",\r
  "level": "eecs",\r
  "order": 90,\r
  "summary": "Finish the path at bundled apps, image registration, process handoff, and repeatable release checks.",\r
  "prerequisites": ["storage-and-images"],\r
  "sources": [\r
    { "path": "apps/os_sdk.trit", "label": "Application SDK", "kind": "source" },\r
    { "path": "apps/libwidget.trit", "label": "Widget library", "kind": "source" },\r
    { "path": "APP_MANIFEST.json", "label": "Bundled app manifest", "kind": "manifest" },\r
    { "path": "build_tos_image.cpp", "label": "Application image registration", "kind": "source" },\r
    { "path": "tools/trit-test.ps1", "label": "Release test entry point", "kind": "tool" }\r
  ],\r
  "evidence": [\r
    { "path": "tests/test_native_apps.cpp", "label": "Bundled app tests", "kind": "test" },\r
    { "path": "tests/test_process_handoff.cpp", "label": "Process handoff tests", "kind": "test" }\r
  ],\r
  "next": null,\r
  "interactive": {\r
    "kind": "choice",\r
    "title": "Release check",\r
    "prompt": "What must change when a new bundled app is added?",\r
    "options": ["Only the UI label", "The app source, image builder, manifest, and focused tests", "Only a Graphify snapshot"],\r
    "answer": 1,\r
    "explanation": "A bundled app is a release contract. Its source, image registration, APP_MANIFEST entry, and focused tests must stay aligned."\r
  }\r
}\r
---\r
\r
## The user-facing end of the stack\r
\r
Apps compile against the SDK and shared widget/library code. The image builder\r
registers them, \`APP_MANIFEST.json\` records their guest paths and metadata, and\r
process-handoff tests verify that a release can actually launch them.\r
\r
## Validation loop\r
\r
For a source or manifest change, use the smallest focused test first, then run\r
the release checks. The repository's doctor, smoke, production, and image\r
inspection tools are the evidence trail; Graphify remains advisory navigation.\r
\r
## Prerequisites\r
\r
Complete storage and images. You should now be able to name the source,\r
contract, and test that sit at each layer of the path.\r
\r
## You reached implementation\r
\r
The EECS path ends at actual app registration and validation evidence. From here\r
you can choose a challenge, inspect a source symbol, or pick a known gap from\r
the repository roadmap instead of treating the guide as a replacement for the\r
implementation.\r
`,w=[{id:"beginner",title:"Beginner path",audience:"I am new to ternary computing",description:"Build a correct mental model, then write and validate a first TCL function in the browser.",page_ids:["representation-boundaries","tritwise-operations","first-tcl-program"]},{id:"programmer",title:"Programmer path",audience:"I already write systems or application code",description:"Trace a program from source through the ISA, VM, compiler, and boot boundary.",page_ids:["representation-boundaries","tritwise-operations","first-tcl-program","isa-vm-execution","compiler-pipeline","boot-and-traps"]},{id:"eecs",title:"EECS path",audience:"I want implementation and validation evidence",description:"Follow the full silicon-to-user path through kernel services, storage, images, and applications.",page_ids:["representation-boundaries","tritwise-operations","isa-vm-execution","compiler-pipeline","boot-and-traps","kernel-syscalls","storage-and-images","applications-and-validation"]}],T={paths:w},x=Object.assign({"./content/learn/01-representation-boundaries.md":u,"./content/learn/02-tritwise-operations.md":m,"./content/learn/03-first-tcl-program.md":g,"./content/learn/04-isa-vm-execution.md":f,"./content/learn/05-compiler-pipeline.md":b,"./content/learn/06-boot-and-traps.md":v,"./content/learn/07-kernel-syscalls.md":_,"./content/learn/08-storage-and-images.md":y,"./content/learn/09-applications-and-validation.md":k});function A(n,r){const i=n.match(/^---\r?\n([\s\S]*?)\r?\n---\r?\n([\s\S]*)$/);if(!i)throw new Error(`Learning document ${r} is missing JSON front matter`);let e;try{e=JSON.parse(i[1])}catch(t){throw new Error(`Learning document ${r} has invalid JSON front matter: ${String(t)}`)}return{...e,content:i[2].trim()}}const I=T,L=Object.entries(x).map(([n,r])=>A(r,n)).sort((n,r)=>n.order-r.order);function S(n){return n.split("|").map(r=>r.trim()).filter((r,i,e)=>i>0&&i<e.length-1)}function C(n){return n.length>0&&n.every(r=>/^:?-{3,}:?$/.test(r))}function P(n){var c,d;const r=n.split(/\r?\n/),i=[];let e=null;const t=()=>{e&&(i.push(e),e=null)};for(const l of r){const a=l.trim();if(a.startsWith("```")){(e==null?void 0:e.type)==="code"?t():(t(),e={type:"code",content:"",lang:a.substring(3).trim()});continue}if((e==null?void 0:e.type)==="code"){e.content+=e.content?`
${l}`:l;continue}if(!a){t();continue}const s=a.match(/^(#{2,4})\s+(.+)$/);if(s){t(),e={type:s[1].length===2?"h2":s[1].length===3?"h3":"h4",content:s[2].trim()},t();continue}if(a.startsWith("|")){const o=S(a);if(C(o))continue;!e||e.type!=="table"?(t(),e={type:"table",content:"",headers:o,rows:[]}):(e.rows=e.rows||[],e.rows.push(o));continue}const p=a.match(/^[-*]\s+(.+)$/);if(p){(!e||e.type!=="ul")&&(t(),e={type:"ul",content:"",items:[]}),(c=e.items)==null||c.push(p[1]);continue}const h=a.match(/^\d+\.\s+(.+)$/);if(h){(!e||e.type!=="ol")&&(t(),e={type:"ol",content:"",items:[]}),(d=e.items)==null||d.push(h[1]);continue}!e||e.type!=="p"?(t(),e={type:"p",content:a}):e.content+=` ${a}`}return t(),i}export{I as L,L as a,P as p};
