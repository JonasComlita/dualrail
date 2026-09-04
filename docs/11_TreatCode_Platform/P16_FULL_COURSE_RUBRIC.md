# P16 Full-Course Review Rubric

This rubric is the human review surface for the complete P05 curriculum. It
is intentionally separate from the automated validators: a green validator
proves that the declared inventory, links, depth, and route contracts hold;
it does not substitute for product, educator, systems, or accessibility
judgment.

## Phase coverage review

Reviewers should open the linked lesson pair, inspect its source and test
evidence, and record whether the learner-facing explanation distinguishes the
general computer-science concept from current, planned, and unavailable Trit
behavior.

| Phase | Learner outcome to review | Evidence surface |
|---|---|---|
| Authority, build graph, and review controls | Safely distinguish a repository claim, its authority, and a reproducible check. | `authority-claims`, `authority-build-evidence` |
| Trit representation, gates, and hardware realization | Explain trits, balanced ternary, gates, dual-rail realization, and hardware limits. | `representation-boundaries`, `hardware-gates` |
| ISA, registers, privilege, CSRs, traps, and ABI | Trace instruction fields, register state, privilege, trap causes, and calling conventions. | `isa-vm-execution`, `isa-trap-abi-trace` |
| Numeric model, native operations, lanes, SIMD, and accelerators | Keep scalar numeric meaning separate from packed lanes and accelerator claims. | `tritwise-operations`, `simd-accelerator-evidence` |
| VM architectural state and memory system | Predict PC, register, memory, stack, and fault transitions. | `vm-state-memory`, `vm-memory-trace` |
| VM interpreter, dispatch, trace JIT, and multicore execution | Compare fetch/decode/execute, dispatch, tracing, scheduling, and synchronization. | `vm-dispatch-multicore`, `vm-jit-benchmark` |
| Assembler, disassembler, relocations, and binary contract | Read syntax, encodings, symbols, relocations, and binary compatibility boundaries. | `assembler-binary-contract`, `assembler-relocation-lab` |
| Low-level assembly IR and linking boundary | Follow lowering, object creation, symbol resolution, and link failure. | `low-level-ir-linking`, `link-failure-investigation` |
| TCL language contract | Use syntax, types, control flow, ownership, pointers, and explicit error constraints. | `tcl-language-contract`, `first-tcl-program` |
| Host compiler: front end through object and link output | Locate lexing, parsing, AST, typing, optimization, allocation, code generation, and link stages. | `compiler-pipeline`, `compiler-object-trace` |
| Self-hosted TCL compiler | Explain bootstrap stages, trust boundaries, reproducibility, and self-hosting tests. | `self-hosting-bootstrap`, `self-hosted-reproducibility` |
| User runtime, libraries, streams, and SDK ABI | Connect runtime services, libraries, streams, handles, wrappers, and error propagation. | `runtime-sdk-abi`, `runtime-streams-errors` |
| Reset, boot, trap entry, and first executable programs | Trace reset, image loading, boot stages, trap entry, and the first program. | `boot-and-traps`, `first-executable-boot` |
| Kernel foundations: HAL, memory, MMU/TLB, process, traps, and IPC | Explain translation, process state, scheduling, traps, IPC, and isolation boundaries. | `kernel-syscalls`, `kernel-memory-process` |
| Kernel storage: BIO, buffer pool, WAL, relational state, and VFS | Follow block I/O, caching, logging, recovery, and filesystem consistency. | `storage-and-images`, `storage-recovery-vfs` |
| Kernel networking, graphics, devices, and services | Identify device lifecycle, driver, interrupt/DMA, network, graphics, and service limits. | `devices-services`, `network-graphics-boundary` |
| Host OS model and native integration | Separate guest state from host integration and interpret observability evidence. | `host-runtime-integration`, `host-observability-portability` |
| User space, shell, GUI, and bundled applications | Connect processes, app SDKs, shell/GUI composition, permissions, and user failures. | `applications-and-validation`, `shell-gui-apps` |
| Images, loading, packaging, and distribution | Read image formats, loaders, manifests, release channels, compatibility, and recovery. | `images-packaging-distribution`, `release-loading-recovery` |
| Specialized libraries and developer products | Evaluate library/tooling boundaries, extensions, and contributor workflows. | `specialized-libraries-products`, `developer-extension-workflow` |
| Security, fuzzing, performance, and full-system closure | Build a scoped closure claim with threat model, oracle, workload, and residual gaps. | `security-fuzzing-performance-closure`, `full-system-validation` |

## Cross-course gates

| Gate | Pass condition | Automated evidence | Human question |
|---|---|---|---|
| Coverage | Every current stack phase has two or more lessons and exact registry links. | `curriculum-matrix.json`, `stack-integration.json` | Is any required outcome merely named rather than taught? |
| Depth | Every lesson has the required sections and at least 600 substantive prose words. | `content-depth.json`, `content-validation.json` | Does the explanation make the example and misconception understandable? |
| Progression | Beginner starts at representation and all paths reach the closure lesson without cycles or dead ends. | `learning-flow.json`, `browser-e2e.json` | Can a learner recover from a direct link or a missed prerequisite? |
| Interaction | Conceptual checks, state traces, source investigations, and bounded code execution are present. | `exercise-execution.json` | Does feedback reveal a real state/result or merely change local copy? |
| Evidence | Source, test, benchmark, gap, and Stack Explorer transitions preserve exact IDs and status labels. | `stack-integration.json` | Are planned and unavailable items clearly bounded? |
| Accessibility | Keyboard names, focusable controls, live feedback, responsive layout, and no-JavaScript reading work at 390, 768, and 1280 CSS pixels. | `accessibility.json`, `browser-e2e.json` | Can the intended audience complete the reading and feedback loop? |
| Product continuity | The visual language and challenge transition remain consistent with `/practice`. | `browser-e2e.json`, built route pages | Does Learn feel like one TreatCode product rather than a detached document dump? |

## Approval record

- **Product owner:** pending
- **Educator:** pending
- **Systems reviewer:** pending
- **Accessibility reviewer:** pending
- **Decision:** pending
- **Verified commit:** pending
- **Date:** pending
