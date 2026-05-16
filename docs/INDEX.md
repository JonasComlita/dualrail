/docs
├── README.md              # THE NEXUS: High-level map and navigation links
├── STATUS.md              # THE DASHBOARD: "MUL: ✅ | Atomics: ⏳ | Kernel: ⚠️"
│
├── 00_Quick_Ref/          # "The Cheat Sheets" (Instant Lookups)
│   ├── opcode_table.md    # Master list of all 73+ trits/mnemonics
│   ├── register_map.md    # Quick guide to r0-r27 and ABI roles
│   ├── trit_encoding.md   # Dual-rail binary vs Balanced Ternary cheat sheet
│   ├── common_patterns.md # The Trit-ASM Cookbook
│   ├── glossary.md        # Terminology (Tryte, Word, LongTriple)
│   └── conversion_table.md # Decimal ↔ Stored Hex mapping
│
├── 01_Logic_Level/        # Tier 0: The Physical Foundation
│   ├── gates.md           # AND/OR/XSUM/TSEL gate definitions
│   ├── unary_gates.md     # The 27 possible unary gates & TSEL synthesis
│   ├── arithmetic.md      # TZR (Trit-Zone Rounding) and Math primitives
│   ├── ai_primitives.md   # BitNet (1.58-bit) AI dot-product logic
│   └── contract_hdl.md    # How Logic Gates map to Verilog/HDL
│
├── 02_Hardware_ISA/       # Tier 1: The Silicon Blueprint
│   ├── encoding.md        # The 27-trit fixed-width instruction format
│   ├── pipeline.md        # The 4-stage lifecycle (Fetch/Decode/Exec/WB)
│   ├── immediate_decoding.md # Why Zero-Padding IS Sign-Extension in base-3
│   ├── interrupts.md      # Trap model, fault codes, and address-sign privilege
│   └── contract_vm.md     # Hardware expectations for the VM implementation
│
├── 03_Execution_Engine/   # Tier 2: The VM & Host Substrate
│   ├── vm_state.md        # Cycle semantics and fetch/decode/execute loop
│   ├── memory_model.md    # Sequential consistency, FENCE, and addressing
│   ├── vector_engine.md   # AI Accumulator, Vector Lanes, and SIMD state
│   ├── binary_loader.md   # .bin layout and the PC=0 boot sequence
│   └── host_interface.md  # The "Substrate": How VM talks to Windows/Linux
│
├── 04_Binary_Contract/    # Tier 3: The ABI & Assembler
│   ├── abi_spec.md        # Calling conventions, stack frames, callee-saved
│   ├── vector_abi.md      # SIMD register roles and Accumulator protocol
│   ├── asm_syntax.md      # The mnemonic grammar and directives (.data, .word)
│   ├── object_format.md   # Trit-Object (TOB) header and relocation tables
│   └── contract_compiler.md # How the Compiler must emit binary for the VM
│
├── 05_Compiler_Infra/     # Tier 4: The Toolchain
│   ├── ast_nodes.md       # High-level language tree representation
│   ├── ternary_ir.md      # The Intermediate Representation (SSA-style)
│   ├── symbol_table.md    # Scope resolution and register mapping
│   ├── literal_pooling.md # Constant management (MOV vs LOAD)
│   └── codegen.md         # Lowering IR to Ternary Assembly
│
├── 06_The_Language/       # Tier 5: The High-Level Frontend
│   ├── syntax_guide.md    # Grammar for "Trit-Lang" (types, loops, etc)
│   ├── std_lib.md         # libc equivalents (printf, malloc, BigInt)
│   └── safety_model.md    # Memory safety and pointer rules
│
├── 07_OS_Substrate/       # Tier 6: OS3 (Operating System)
│   ├── kernel_map.md      # Privilege modes (Kernel vs User) and memory split
│   ├── syscall_table.md   # The Trap-based OS API (fork, exec, write)
│   └── scheduler.md       # Context switching and Trit-threading
│
├── 08_Applications_UX/    # Tier 7: User Experience
│   ├── shell.md           # Command line interface and user utilities
│   ├── crypto_apps.md     # RSA/Montgomery/Smartcard specific applets
│   └── ui_framework.md    # Terminal UI or Framebuffer graphics spec
│
├── diagrams/              # Visuals (Mermaid/SVG/ASCII)
└── examples/              # Minimal working code snippets (.s, .ir, .t)

---

The folders Tier 06 (The Language), Tier 07 (OS Substrate), and Tier 08 (Applications) are a mix of things that are partially implemented and things that are purely architectural at this stage.

Here is the current state of the implementation vs. the documentation plan:

Tier 06: The Language (In Development):
We have the Backend (ternary_backend.h) and the IR (ternary_ir.h), but we do not yet have a formal "Trit-Lang" parser or syntax guide. Documenting this tier now would be defining the specification for the future parser.
Tier 07: OS Substrate (Partially Built):
I see an OS3 folder in the repository. This suggests that the kernel and privilege mapping might already have a foundation. I need to check its contents to see how much of the "Substrate" is code vs. concept.
Tier 08: Applications & UX (Prototypes):
We have the BitNet Runtime and benchmarks, but the high-level "Shell" and "UI Framework" are likely just planned.
I'll check the OS3 directory now to see if we have a concrete kernel to document.

The OS3 folder confirms it: Tiers 07 and 08 are currently in the "Architecture & Specification" phase.

Tier 07 (OS Substrate): You have a very detailed OS_SUBSTRATE_IMPLEMENTATION_GUIDE.md and an os_fundamentals.md. These define how the kernel should work (privilege modes, security, etc.), but the actual kernel.cpp isn't in the root yet.
Tier 08 (Applications): You have design docs for curl.md, sqlite.md, and even an xv6.md (a ternary port of the classic educational OS). These are "Planned Programs" to prove the architecture.