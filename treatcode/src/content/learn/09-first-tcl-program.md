---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "first-tcl-program",
  "title": "First TCL program",
  "module": "Phase 08 · TCL language contract",
  "level": "beginner",
  "order": 17,
  "summary": "Trace the repository evidence for TCL language contract.",
  "phase_id": "tc:layer:phase-08-tcl-language",
  "phase_slug": "tcl-language",
  "phase_name": "TCL language contract",
  "lesson_kind": "build-trace",
  "implementation_status": "complete",
  "canonical_terms": [
    "TCL",
    "type contract",
    "ownership",
    "pointer",
    "source error"
  ],
  "objectives": [
    "Explain syntax, types, control flow, memory and ownership, pointers, errors, and explicit language constraints.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "tcl-language-contract"
  ],
  "sources": [
    {
      "path": "TCL_Spec_1.0.md",
      "label": "TCL 1.0 language specification",
      "kind": "source"
    },
    {
      "path": "docs/06_Language/tcl_language.md",
      "label": "tcl_language.md",
      "kind": "documentation"
    },
    {
      "path": "tcl_parser.trit",
      "label": "tcl_parser.trit",
      "kind": "source"
    }
  ],
  "evidence": [
    {
      "path": "TEST_MANIFEST.json",
      "label": "Authoritative test manifest",
      "kind": "manifest"
    },
    {
      "path": "tests/test_phase7_compiler.cpp",
      "label": "tc:test:test-phase7-compiler test source",
      "kind": "test"
    },
    {
      "path": "tests_next/04_compiler/next_compiler_pipeline.cpp",
      "label": "tc:test:next-compiler-pipeline test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-phase7-compiler",
    "tc:test:next-compiler-pipeline"
  ],
  "benchmark_ids": [],
  "gap_ids": [],
  "stack_links": {
    "phase": "/stack/tcl-language",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TCL_Spec_1.0.md",
    "tests": "/stack?focus=tc%3Atest%3Atest-phase7-compiler"
  },
  "next": "compiler-pipeline",
  "interactive": {
    "kind": "code",
    "title": "Try the first function",
    "starter": "import ulib;\n\nfn first_value() -> t40 {\n    return 1 + 2;\n}",
    "expectedIncludes": [
      "fn first_value",
      "return 1 + 2"
    ],
    "exercise_id": "first-tcl-program",
    "runner": "treatcode-learning-compiler",
    "explanation": "Submit the bounded TCL example to the repository-backed learning runner. It may pass or report a real compiler failure; either result is evidence about the TCL language contract exercise, not a local text-only success."
  }
}
---

## Objectives

- Explain syntax, types, control flow, memory and ownership, pointers, errors, and explicit language constraints in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [tcl-language-contract](/learn/eecs/tcl-language-contract) first. Those lessons introduce the state and vocabulary that this page assumes. This is the build, trace, and evidence lesson for TCL language contract. The goal is to inspect a real repository transition and report both what the current tests prove and what they do not prove. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

A systems language makes resource and representation choices visible. Syntax is only the surface: types constrain operations, control flow determines which states are reachable, ownership describes who may use memory, and error rules explain how failure crosses a function boundary. A learner should be able to tell which behavior belongs to the language specification and which is a compiler or runtime extension. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is whether the repository's files and tests actually carry that invariant through a build or execution step.

## Explanation

TCL_Spec_1.0.md, the language documentation, and the native lexer/parser define the current front-end contract. Compiler smoke inputs and phase-seven tests provide validation. The first program uses a typed t40 result and an ordinary function body so the learner sees a real source shape rather than a UI-only snippet. The phase enters through a .trit source file and the TCL language contract and leaves through a source program whose types, ownership, and failure behavior are explainable. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

Read the TCL specification section for functions and compare it with tcl_parser.trit. Open the compiler smoke input and identify the syntax that is actually exercised. Then mark one language promise that is tested and one promise that is documented but still constrained by a planned runtime feature. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

TCL_Spec_1.0.md, the language documentation, and the native lexer/parser define the current front-end contract. Compiler smoke inputs and phase-seven tests provide validation. The first program uses a typed t40 result and an ordinary function body so the learner sees a real source shape rather than a UI-only snippet. The current registry row is tc:layer:phase-08-tcl-language, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

The self-hosted compiler and richer library surface are separate phases. Where pointer safety or ownership analysis is incomplete, the lesson records the constraint and points to the exact test or gap. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

Consider a function that returns 1 + 2 as t40. The return type tells the compiler which numeric width to check; the expression is not merely a host-language integer. If a pointer is passed to a buffer helper, the lifetime and bounds rule must be described separately from the function's syntax. A parser success therefore answers only one question in the pipeline. For this lesson, write the example as a sequence: first identify a .trit source file and the TCL language contract; next apply the tcl-language rule; then inspect a source program whose types, ownership, and failure behavior are explainable; finally compare the result with the named validation record. The repository investigation should begin at TCL_Spec_1.0.md and cross-check the test IDs tc:test:test-phase7-compiler, tc:test:next-compiler-pipeline. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

A valid-looking TCL snippet is not proof that the compiler accepts every related program. The specification, parser, type checker, runtime, and target ABI each own different failure modes. Likewise, a pointer value is not a permission grant to arbitrary memory. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Annotate a small TCL function with syntax, type, ownership, runtime, and ABI notes. Change one line to make a type or ownership error and predict the stage that should reject it before running the compiler test. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [TCL 1.0 language specification](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TCL_Spec_1.0.md) — TCL_Spec_1.0.md
- [tcl_language.md](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/docs/06_Language/tcl_language.md) — docs/06_Language/tcl_language.md
- [tcl_parser.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tcl_parser.trit) — tcl_parser.trit

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/tcl-language) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-phase7-compiler test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_phase7_compiler.cpp) — tests/test_phase7_compiler.cpp
- [tc:test:next-compiler-pipeline test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/04_compiler/next_compiler_pipeline.cpp) — tests_next/04_compiler/next_compiler_pipeline.cpp

The test IDs attached to this lesson are tc:test:test-phase7-compiler, tc:test:next-compiler-pipeline. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase has no benchmark record in the current registry.

## Next step

[Continue to the next lesson](/learn/eecs/compiler-pipeline). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
