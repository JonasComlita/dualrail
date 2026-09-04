# P16 — P05 Completeness Amendment: Full Learning Curriculum

## Metadata

- **Plan ID:** P16
- **Version:** 1
- **Status:** `in_progress`
- **Depends on:** P03, P04, P05, P15
- **Scope owner:** Education and documentation
- **Replaces for completeness purposes:** the v1 user-facing scope of P05
- **Agent handoff prompt:** [P05 agent completion prompt](P05_AGENT_COMPLETION_PROMPT.md)

## Why this amendment exists

P05 v1 migrated nine short Markdown pages, created three path labels, and
proved that a lesson could render with provenance and an interactive check. It
did not define a curriculum inventory, a minimum depth for a lesson, a
complete path through the stack, an exercise for every domain, or a browser
journey through every page and interaction. The old tests asserted a few
representative strings and route wiring, so a small first draft could satisfy
them.

The v1 completion record and the existing beginner rubric are retained as
historical evidence for that migration slice. They are not approval for a
complete computer-systems curriculum. P16 is the execution authority for the
full learning goal.

## Objective

Build a repository-backed course that teaches a learner how a computer is
represented and executes instructions, how the Trit ISA/VM/compiler/runtime
works, how boot and traps enter the kernel, and how storage, devices,
networking, user space, packaging, security, and performance close the system.
Every concept must lead to an understandable explanation, a worked example,
a learner action, and exact production evidence.

The course must distinguish the ideal/computer-science concept, the current
Trit implementation, and the project's planned gaps. It must never imply that
a concise overview or a link to Stack Explorer is equivalent to teaching the
topic.

## Required curriculum inventory

Create `treatcode/src/content/learn/P05_CURRICULUM_MATRIX.json` (or an
equivalent generated artifact) with one row for every current `stack_nodes`
record. The matrix is the source of truth for lesson coverage, not a manually
curated list that can omit new phases. At minimum, the current 21 phases must
have the following learning outcomes:

| Phase | Required learning focus |
|---|---|
| Authority, build graph, and review controls | source of truth, reproducible builds, tests, evidence, and how to read the project's claims safely |
| Trit representation, gates, and hardware realization | trits, ternary encodings, balanced/unbalanced choices, gates, dual-rail realization, timing, and the hardware boundary |
| ISA, registers, privilege, CSRs, traps, and ABI | instruction shape/opcodes, register state, privilege, CSRs, trap causes/entry, calling convention, and ABI boundaries |
| Numeric model, native operations, lanes, SIMD, and accelerators | numeric versus lane representation, arithmetic semantics, vector/SIMD behavior, and accelerator limits |
| VM architectural state and memory system | PC/registers, memory/addressing, stack, fetch state, vectors, faults, and architectural invariants |
| VM interpreter, dispatch, trace JIT, and multicore execution | fetch/decode/execute, dispatch, tracing/JIT tradeoffs, scheduling, multicore state, and synchronization |
| Assembler, disassembler, relocations, and binary contract | assembly syntax, encoding/decoding, symbols, relocation, object boundaries, and binary compatibility |
| Low-level assembly IR and linking boundary | IR purpose, lowering, object/link stages, symbol resolution, and link failures |
| TCL language contract | syntax, types, control flow, memory/ownership/pointers, errors, and the language's explicit constraints |
| Host compiler: front end through object and link output | lexing, parsing, AST, type checking, IR, optimization, allocation, code generation, object files, and linking |
| Self-hosted TCL compiler | bootstrap stages, trust boundaries, self-hosting constraints, compiler tests, and reproducibility |
| User runtime, libraries, streams, and SDK ABI | runtime services, libraries, streams, handles, SDK wrappers, ABI stability, and error propagation |
| Reset, boot, trap entry, and first executable programs | reset state, image loading, boot stages, trap entry, first user program, and failure diagnosis |
| Kernel foundations: HAL, memory, MMU/TLB, process, traps, and IPC | hardware abstraction, allocation, address translation, TLBs, process/context state, scheduling, traps, IPC, and isolation |
| Kernel storage: BIO, buffer pool, WAL, relational state, and VFS | block I/O, caching, write-ahead logging, recovery, relational state, filesystem/VFS operations, and consistency |
| Kernel networking, graphics, devices, and services | device discovery/lifecycle, drivers, interrupts/DMA boundaries, networking, graphics, services, and current gaps |
| Host OS model and native integration | host/guest boundaries, native runtime integration, portability, virtualization assumptions, and observability |
| User space, shell, GUI, and bundled applications | processes, shell/GUI composition, app SDKs, bundled apps, permissions, and user-facing failure paths |
| Images, loading, packaging, and distribution | image formats, loaders, manifests, packaging, release channels, compatibility, and recovery |
| Specialized libraries and developer products | libraries/tooling, product boundaries, extension points, and how contributors use the platform |
| Security, fuzzing, performance, and full-system closure | threat model, isolation, fuzzing, benchmark interpretation, performance tradeoffs, and end-to-end validation |

If the registry adds a phase, the matrix, lessons, prerequisites, glossary,
and tests must gain the corresponding row before the plan can remain complete.

## Minimum content and interaction depth

- Produce at least two substantive lessons per required phase: one mental-model
  or concept lesson and one build/trace/source/evidence lesson. This is a
  minimum of 42 lessons for the current inventory; split a phase into more
  lessons when two lessons cannot teach its required outcomes honestly.
- Each lesson must contain, in visible learner-facing content, objectives,
  prerequisites, an explanation of why the concept exists, a worked example,
  one common misconception, one learner action/exercise, authoritative source
  links, validation/test evidence, and an explicit next step.
- Each lesson must contain at least 600 substantive prose words outside
  frontmatter and code, unless it is an executable lab whose equivalent
  explanation, trace, and learner interaction are validated by the content
  test. Headings with empty or repetitive filler do not count.
- Use at least three interaction types across the course: conceptual checks,
  state/instruction traces or simulations, and executable code/source
  investigations. Every code-oriented phase must have at least one exercise
  that runs against a real bounded compiler/VM/test surface or reports a real
  failure; a button that only changes local text is not an implementation
  exercise.
- Every lesson must map to one or more P15 stack phase IDs, exact source
  records, and exact test/evidence records. Every stack phase must link back to
  its canonical lessons so Stack Explorer and Learn form one navigable system.

## Paths, glossary, and prerequisites

1. **Beginner path:** starts with representation and execution, reaches a
   first TCL program, and then continues through compiler, boot, kernel,
   storage, devices, user space, packaging, and closure. It may link to
   deeper lessons, but it may not end at the first program or stop after a
   representative sample.
2. **Programmer path:** teaches the language, VM, compiler, runtime, SDK, app,
   and debugging workflow while linking back to the architecture prerequisites
   and forward to OS evidence. It must expose the full phase map rather than
   hiding prerequisites behind a few pages.
3. **EECS/systems path:** provides the complete implementation/evidence route
   across all phases, including hardware, ISA, memory, boot, kernel, devices,
   storage, networking, user space, images, security, and performance.

All three paths must be ordered, direct-linkable, and finite. The beginner and
systems paths must each reach at least one canonical lesson for every matrix
phase; the systems path must reach every lesson. No path may contain an
unresolved prerequisite, a cycle, a dead end before the final capstone, or a
lesson that is only accessible through a UI state that a direct URL cannot
recreate.

The glossary must contain at least 75 defined terms for the current curriculum
and, more importantly, every canonical term named in the matrix or lesson
frontmatter. Each definition must identify its phase, distinguish related
representations where confusion is likely, and link to a lesson and source.

## Deliverables

1. **Curriculum matrix and content model.** Add the phase/lesson matrix,
   structured frontmatter schema, path catalog, glossary, prerequisite graph,
   and generated coverage report. Remove any legacy hard-coded guide data from
   the publication path.
2. **Complete lesson set.** Write and render the required 42+ lessons with
   real examples, explanations, exercises, source links, tests/evidence,
   misconceptions, and next steps. Keep current/implemented/planned status
   explicit.
3. **Learning experience.** Implement path navigation, previous/next and
   prerequisite links, stable deep links, progress/coverage indicators,
   interactive checks, exercise feedback, and transitions to Stack Explorer,
   source, test, benchmark, gap, and challenge surfaces.
4. **Executable learning surfaces.** Connect code/trace exercises to real
   repository-backed validation where the topic supports it. For topics that
   cannot execute in-browser, provide a deterministic trace/simulation with
   inspectable state and an explanation of its limits.
5. **Validation and editorial review.** Add tests for matrix completeness,
   lesson depth/required sections, glossary/prerequisite integrity, source and
   evidence links, interaction coverage, path traversal, direct links,
   JavaScript-disabled output, keyboard/a11y behavior, and visual continuity
   with `/practice`.
6. **Full-course rubric.** Extend
   `docs/11_TreatCode_Platform/P05_BEGINNER_RUBRIC.md` or use the linked
   [`P16_FULL_COURSE_RUBRIC.md`](../P16_FULL_COURSE_RUBRIC.md) to review every
   path and phase. A pending or acting-only rubric is not an approval.

## Completeness gates (the anti-partial-implementation contract)

An agent may not mark P16 complete when any of the following is true:

- There are only a few representative Markdown pages, even if all three path
  labels exist and the route renders.
- A stack phase appears in Stack Explorer but has no canonical learning module,
  or a lesson has no exact source/test/evidence mapping.
- A lesson consists of a short summary, a link dump, or a generic quiz without
  a worked example and learner action.
- The validator checks only that files exist or that one selected lesson
  renders. It must traverse every path, every lesson, every interaction type,
  every glossary entry, and every matrix row.
- A code exercise is a fake success, a static button, or disconnected from the
  actual compiler/VM/test behavior.
- A missing or planned implementation is taught as if it were shipped, or a
  source link is used to hide the absence of an explanation.
- The old `guideContent.ts` path, a hard-coded sample catalog, or an
  incomplete fallback is used for published content.
- The beginner or systems path stops at the first program, ISA/VM, compiler,
  or kernel overview instead of completing the route to system closure.

## Acceptance criteria

- [ ] The curriculum matrix contains every current stack phase and maps each
      phase to two or more substantive lessons, objectives, sources, evidence,
      exercises, and prerequisite/next links.
- [ ] At least 42 lessons pass the required-section and substantive-depth
      checks for the current 21-phase inventory.
- [ ] Beginner, programmer, and EECS/systems paths are ordered, deep-linkable,
      cycle-free, and reach their stated terminal outcomes; beginner and
      systems each cover all matrix phases.
- [ ] Every lesson has a real worked example, misconception check, learner
      action, interaction, exact source, exact validation evidence, and next
      step; code topics execute against real bounded behavior.
- [ ] Glossary coverage, prerequisite graph, path links, source links, test /
      evidence links, and Stack Explorer cross-links validate with no orphans.
- [ ] A browser test traverses every lesson and interaction, including direct
      links, previous/next, path changes, exercise feedback, and transitions to
      P15 evidence views, with JavaScript disabled checks for reading content.
- [ ] Accessibility, keyboard navigation, responsive layout, and visual
      continuity pass at 390, 768, and 1280 CSS pixels.
- [ ] Current implementation, planned work, and unavailable capabilities are
      labelled consistently and supported by repository evidence.
- [ ] Product, educator, systems/architecture, and accessibility reviewers
      approve the complete course rubric and coverage reports.

## Verification

Run these commands sequentially after P15's complete evidence is available.

```powershell
python tools/trit_tool.py knowledge status
npm.cmd --prefix treatcode run build
npm.cmd --prefix treatcode run test:content
npm.cmd --prefix treatcode run test:curriculum-coverage
npm.cmd --prefix treatcode run test:content-depth
npm.cmd --prefix treatcode run test:e2e:learn
npm.cmd --prefix treatcode run test:e2e:learn-complete
npm.cmd --prefix treatcode run test:a11y
npm.cmd --prefix treatcode run test:public-coverage
python tools/trit_tool.py website plans validate
python tools/trit_tool.py website plan verify P16
```

The new coverage, depth, and complete browser tests are part of the
implementation. If a command does not yet exist, adding the plan without
adding the test is incomplete.

## Required Evidence

- `build/treatcode-plan-evidence/P16/result.json` — machine-readable plan
  result with the complete status and command outputs.
- `build/treatcode-plan-evidence/P16/curriculum-matrix.json` — all phase,
  lesson, objective, source, evidence, exercise, and path rows.
- `build/treatcode-plan-evidence/P16/content-depth.json` — required sections,
  substantive depth, worked examples, misconceptions, and learner actions.
- `build/treatcode-plan-evidence/P16/content-validation.json` — frontmatter,
  links, code, provenance, status labels, and legacy-source audit.
- `build/treatcode-plan-evidence/P16/glossary-prereq.json` — glossary coverage,
  definitions, graph integrity, reachability, and cycle report.
- `build/treatcode-plan-evidence/P16/exercise-execution.json` — real code,
  trace, simulation, and feedback interaction results.
- `build/treatcode-plan-evidence/P16/learning-flow.json` — every path and
  lesson traversal report.
- `build/treatcode-plan-evidence/P16/browser-e2e.json` — direct-link,
  JavaScript-disabled, responsive, and screenshot manifest.
- `build/treatcode-plan-evidence/P16/accessibility.json` — keyboard,
  landmarks, names, focus, contrast, and responsive accessibility report.
- `build/treatcode-plan-evidence/P16/stack-integration.json` — bidirectional
  Learn/Stack Explorer/source/test/evidence link audit.
- `docs/11_TreatCode_Platform/P05_BEGINNER_RUBRIC.md` or the linked full-course
  [`P16_FULL_COURSE_RUBRIC.md`](../P16_FULL_COURSE_RUBRIC.md) with named
  educator, systems, accessibility, and product decisions.

## Completion Record

- **Verified commit:** pending
- **Evidence artifact:** pending
- **Evidence hashes:** pending
- **Human approvals:** pending
- **Date:** pending
