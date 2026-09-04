# Follow-up prompt for the original P05 agent

You are resuming work on TreatCode Learn in
`C:\Users\jonas\Documents\trit`.

Your previous P05 result was a valid migration/proof-of-function draft, but it
was not a complete implementation of the intended course. Treat the old P05
completion record and beginner rubric as historical v1 evidence only. The
current execution authority is
`docs/11_TreatCode_Platform/plans/P16_P05_curriculum_completeness_amendment.md`.

## What was incomplete

The previous work proved that Markdown lessons, path selectors, provenance,
and an interactive check could render. It did not build the promised
repository-backed course from representation through the operating system. In
particular:

- The implementation contained only a small set of concise representative
  lessons and did not define a coverage inventory for all stack phases.
- The plan set no minimum lesson depth, worked-example requirement, exercise
  requirement, or rule requiring every topic to be taught rather than merely
  linked.
- Content tests checked frontmatter, provenance, a few phrases, and route
  wiring. They did not traverse every lesson, path, prerequisite, glossary
  term, interaction, direct link, or source/test transition.
- Three path labels do not constitute three complete learning paths. A path
  that stops after a first TCL program or an ISA/VM overview is still partial.
- The pending beginner rubric could not substantiate the claimed acting
  educator approval.

Do not defend the old result by pointing to passing v1 content or E2E tests.
Those tests were designed to catch broken wiring, not an incomplete course.

## Mission

Complete P16. Build a coherent course that teaches the whole current Trit
stack: how representation and hardware boundaries work, how the ISA and VM
execute, how assembly/IR/linking/compiler/runtime layers fit together, how boot
enters the kernel, and how memory, processes, storage, devices, networking,
user space, packaging, security, performance, and full-system validation close
the system.

For each topic, explain the general computer-systems concept, the current Trit
implementation, and the project's planned or unavailable work separately.
Every concept needs an explanation, a worked example, a learner action, and
exact repository evidence. A link to Stack Explorer is a transition, not a
lesson.

## Required work

1. Read `P16_P05_curriculum_completeness_amendment.md`, the P03/P15 evidence,
   the current `stack_nodes` registry, the learning catalog, all published
   Markdown, `learningContent.ts`, the source/test registries, and the existing
   `/practice` visual language before changing code.
2. Create a generated curriculum matrix with one row for every current stack
   phase (currently 21). Cover, at minimum, authority/reproducibility,
   representation/gates/hardware, ISA/privilege/traps/ABI, numeric/lane/SIMD,
   VM state/memory, interpreter/JIT/multicore, assembler/binary formats,
   low-level IR/linking, TCL, host compiler, self-hosting, runtime/SDK, boot,
   kernel foundations, storage/VFS/recovery, networking/graphics/devices,
   host integration, user space/shell/GUI/apps, images/distribution,
   specialized libraries/products, and security/fuzzing/performance/closure.
3. Write at least two substantive lessons per current phase: a mental-model
   lesson and a build/trace/source/evidence lesson. This is at least 42 lessons
   for the current 21-phase inventory; split phases further when two lessons
   cannot teach the required outcomes honestly.
4. Every lesson must visibly contain objectives, prerequisites, why the topic
   exists, a clear explanation, a worked example, a common misconception, a
   learner action/exercise, authoritative source links, validation/test
   evidence, and an explicit next step. Each lesson needs at least 600
   substantive prose words outside frontmatter/code, or a validated equivalent
   for an executable lab; filler headings do not count.
5. Provide at least three real interaction types across the course: conceptual
   checks, state/instruction traces or simulations, and executable code/source
   investigations. Code-oriented topics must exercise a real bounded
   compiler/VM/test surface or report a real failure. A button that only
   changes local text is not an implementation exercise.
6. Make all three paths meaningful and ordered:

   - Beginner starts at representation, reaches a first TCL program, and then
     continues through compiler, boot, kernel, storage, devices, user space,
     packaging, and closure.
   - Programmer covers language, VM, compiler, runtime, SDK, apps, debugging,
     and the architecture prerequisites it relies on.
   - EECS/systems covers every phase and every lesson, including hardware,
     memory, boot, kernel, devices, storage, networking, release, security,
     and performance.

   Beginner and systems must each reach at least one canonical lesson for
   every matrix phase. No path may dead-end before its capstone, hide a
   prerequisite, or require a UI-only selection that a direct URL cannot
   recreate.

7. Build a glossary of at least 75 terms, plus every canonical term in the
   matrix and lesson frontmatter. Definitions must be phase-linked, source-
   linked, and precise about commonly confused numeric, lane, and hardware
   representations. Make the prerequisite graph cycle-free and fully
   reachable.
8. Link every lesson bidirectionally with P15 Stack Explorer phase/evidence
   pages. Every lesson must have exact source and test/evidence references;
   planned work must remain clearly labelled as planned.
9. Implement stable deep links, previous/next navigation, prerequisite links,
   progress/coverage indicators, exercise feedback, Stack Explorer/source/
   test/benchmark/gap transitions, JavaScript-disabled reading content, and
   `/practice`-consistent styling. Do not use the legacy `guideContent.ts`
   publication path or a hard-coded sample catalog.
10. Add tests for matrix completeness, lesson depth/required sections,
    glossary/prerequisite integrity, provenance and links, interaction and
    exercise execution, all-path traversal, direct links, no-JavaScript
    output, keyboard/a11y behavior, responsive layout, and visual continuity.
    Required commands are:

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

    If a required script does not exist, implement it and include its report;
    do not remove it from the plan.

## Do not claim completion when

- there are only a few representative pages, even if all three path labels
  render;
- a phase is named or linked but not actually taught by a canonical lesson;
- a lesson is a short summary/link dump or generic quiz without a worked
  example and learner action;
- tests validate only file existence, one selected lesson, or a few strings;
- code exercises are fake successes or disconnected from compiler/VM/test
  behavior;
- the beginner path ends at the first program or the systems path stops at an
  overview;
- glossary/prerequisite/path/source/evidence rows are orphaned;
- the old guide-content path or an incomplete fallback supplies published
  content;
- planned work is presented as implemented; or
- acting approval replaces named product, educator, systems, and accessibility
  review artifacts.

## Required completion proof

Before reporting success, write all P16 evidence artifacts, including the
curriculum matrix, content-depth report, content/provenance validation,
glossary/prerequisite report, exercise-execution report, every-path learning
flow, browser/deep-link report, accessibility report, and Stack integration
audit under `build/treatcode-plan-evidence/P16/`. Obtain named product,
educator, systems, and accessibility approvals. Leave P16 `in_progress` if any
phase, lesson, path, exercise, link, or review row is missing, partial,
unresolved, or unreviewed.

Your final response must list the implementation files changed, the exact
phase/lesson/path/glossary counts, the commands and exit codes, the evidence
paths, and any remaining gap. Do not report “complete” from the old P05 result.

