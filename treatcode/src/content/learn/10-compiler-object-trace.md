---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "compiler-object-trace",
  "title": "Trace source to object and link",
  "module": "Phase 09 · Host compiler: front end through object and link output",
  "level": "programmer",
  "order": 19,
  "summary": "Trace the repository evidence for Host compiler: front end through object and link output.",
  "phase_id": "tc:layer:phase-09-host-compiler",
  "phase_slug": "host-compiler",
  "phase_name": "Host compiler: front end through object and link output",
  "lesson_kind": "build-trace",
  "implementation_status": "complete",
  "canonical_terms": [
    "lexer",
    "AST",
    "type checker",
    "code generation",
    "object file"
  ],
  "objectives": [
    "Explain lexing, parsing, AST, type checking, IR, optimization, allocation, code generation, object files, and linking.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "compiler-pipeline"
  ],
  "sources": [
    {
      "path": "ternary_compiler.h",
      "label": "ternary_compiler.h",
      "kind": "source"
    },
    {
      "path": "ternary_compiler_codegen.h",
      "label": "ternary_compiler_codegen.h",
      "kind": "source"
    },
    {
      "path": "tritc.cpp",
      "label": "tritc.cpp",
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
    },
    {
      "path": "tests_next/04_compiler/next_compiler_phase_c5_pred.cpp",
      "label": "tc:test:next-compiler-phase-c5-pred test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-phase7-compiler",
    "tc:test:next-compiler-pipeline",
    "tc:test:next-compiler-phase-c5-pred"
  ],
  "benchmark_ids": [
    "tc:benchmark:test-malloc-micro"
  ],
  "gap_ids": [
    "tc:gap:trit-gap-syscall-tracing"
  ],
  "stack_links": {
    "phase": "/stack/host-compiler",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_compiler.h",
    "tests": "/stack?focus=tc%3Atest%3Atest-phase7-compiler"
  },
  "next": "self-hosting-bootstrap",
  "interactive": {
    "kind": "code",
    "title": "Run a bounded compiler check",
    "starter": "import ulib;\n\nfn first_value() -> t40 {\n    return 1 + 2;\n}",
    "expectedIncludes": [
      "fn first_value",
      "return 1 + 2"
    ],
    "exercise_id": "compiler-object-trace",
    "runner": "treatcode-learning-compiler",
    "explanation": "Submit the bounded TCL example to the repository-backed learning runner. It may pass or report a real compiler failure; either result is evidence about the Host compiler: front end through object and link output exercise, not a local text-only success."
  }
}
---

## Objectives

- Explain lexing, parsing, AST, type checking, IR, optimization, allocation, code generation, object files, and linking in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [compiler-pipeline](/learn/eecs/compiler-pipeline) first. Those lessons introduce the state and vocabulary that this page assumes. This is the build, trace, and evidence lesson for Host compiler: front end through object and link output. The goal is to inspect a real repository transition and report both what the current tests prove and what they do not prove. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

A compiler is a sequence of contracts, not one magical translation step. Lexing recognizes tokens, parsing builds structure, type checking proves local meaning, IR makes transformations inspectable, optimization changes representation under invariants, allocation chooses locations, code generation emits target operations, and object/link stages make cross-file addresses concrete. Diagnostics should make the first broken contract visible. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is whether the repository's files and tests actually carry that invariant through a build or execution step.

## Explanation

ternary_compiler.h, ternary_compiler_codegen.h, and tritc.cpp cover the host compiler driver and output path. Compiler, ABI, allocator, and corpus tests supply evidence at multiple stages. The plan labels syscall tracing as a gap so a missing diagnostic surface is not mistaken for a successful compiler trace. The phase enters through TCL source and compiler configuration and leaves through an object or linked artifact plus diagnostics that identify every major stage. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

Run the frontend smoke or phase-seven compiler test and read its input beside the compiler entry point. Search for the AST or IR handoff, then inspect the codegen wrapper that maps an operation to an instruction. Capture one success and one failure stage in your notes. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

ternary_compiler.h, ternary_compiler_codegen.h, and tritc.cpp cover the host compiler driver and output path. Compiler, ABI, allocator, and corpus tests supply evidence at multiple stages. The plan labels syscall tracing as a gap so a missing diagnostic surface is not mistaken for a successful compiler trace. The current registry row is tc:layer:phase-09-host-compiler, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Some end-to-end tracing and optimization observability remain limited. The route therefore exposes the exact source and test records and gives a bounded exercise that can report either a compiler success or a real failure. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

For first_value returning 1 + 2, lexing produces identifiers and literals, parsing creates a function node, typing checks t40, lowering creates an add and return, allocation chooses registers or temporaries, code generation emits instructions, and linking resolves runtime symbols. If t40 is misspelled, the error should occur before allocation; if a runtime symbol is missing, it belongs later. For this lesson, write the example as a sequence: first identify TCL source and compiler configuration; next apply the host-compiler rule; then inspect an object or linked artifact plus diagnostics that identify every major stage; finally compare the result with the named validation record. The repository investigation should begin at ternary_compiler.h and cross-check the test IDs tc:test:test-phase7-compiler, tc:test:next-compiler-pipeline, tc:test:next-compiler-phase-c5-pred. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

Optimization is not allowed to change observable semantics, and a compiler binary existing on disk is not evidence that a particular source program passed every stage. A successful parse does not imply type correctness or a linked object. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Make a pipeline ledger with one input and one output for each compiler stage. Add a failure injection at lexing, typing, and linking, and state which evidence file would prove the failure is caught. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [ternary_compiler.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_compiler.h) — ternary_compiler.h
- [ternary_compiler_codegen.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_compiler_codegen.h) — ternary_compiler_codegen.h
- [tritc.cpp](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tritc.cpp) — tritc.cpp

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/host-compiler) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-phase7-compiler test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_phase7_compiler.cpp) — tests/test_phase7_compiler.cpp
- [tc:test:next-compiler-pipeline test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/04_compiler/next_compiler_pipeline.cpp) — tests_next/04_compiler/next_compiler_pipeline.cpp
- [tc:test:next-compiler-phase-c5-pred test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/04_compiler/next_compiler_phase_c5_pred.cpp) — tests_next/04_compiler/next_compiler_phase_c5_pred.cpp

The test IDs attached to this lesson are tc:test:test-phase7-compiler, tc:test:next-compiler-pipeline, tc:test:next-compiler-phase-c5-pred. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase also has benchmark records tc:benchmark:test-malloc-micro; interpret them only with their workload and baseline.

## Next step

[Continue to the next lesson](/learn/eecs/self-hosting-bootstrap). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
