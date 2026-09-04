---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "self-hosted-reproducibility",
  "title": "Verify a bootstrap boundary",
  "module": "Phase 10 · Self-hosted TCL compiler",
  "level": "programmer",
  "order": 21,
  "summary": "Trace the repository evidence for Self-hosted TCL compiler.",
  "phase_id": "tc:layer:phase-10-self-hosted-compiler",
  "phase_slug": "self-hosted-compiler",
  "phase_name": "Self-hosted TCL compiler",
  "lesson_kind": "build-trace",
  "implementation_status": "partial",
  "canonical_terms": [
    "bootstrap",
    "self-hosting",
    "trust boundary",
    "compiler seed",
    "reproducibility witness"
  ],
  "objectives": [
    "Explain bootstrap stages, trust boundaries, self-hosting constraints, compiler tests, and reproducibility.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "self-hosting-bootstrap"
  ],
  "sources": [
    {
      "path": "tcl_frontend.trit",
      "label": "tcl_frontend.trit",
      "kind": "source"
    },
    {
      "path": "tcl_backend.trit",
      "label": "tcl_backend.trit",
      "kind": "source"
    },
    {
      "path": "treatcode/compiler_driver.trit",
      "label": "compiler_driver.trit",
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
      "path": "tests_next/04_compiler/next_compiler_native_ulib.cpp",
      "label": "tc:test:next-compiler-native-ulib test source",
      "kind": "test"
    },
    {
      "path": "tests_next/04_compiler/next_compiler_pipeline.cpp",
      "label": "tc:test:next-compiler-pipeline test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:next-compiler-native-ulib",
    "tc:test:next-compiler-pipeline"
  ],
  "benchmark_ids": [],
  "gap_ids": [],
  "stack_links": {
    "phase": "/stack/self-hosted-compiler",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tcl_frontend.trit",
    "tests": "/stack?focus=tc%3Atest%3Anext-compiler-native-ulib"
  },
  "next": "runtime-sdk-abi",
  "interactive": {
    "kind": "source",
    "title": "Source investigation",
    "prompt": "Find the exact implementation boundary for Self-hosted TCL compiler.",
    "sourcePath": "tcl_frontend.trit",
    "sourceHref": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tcl_frontend.trit",
    "expectedIncludes": [
      "tcl_frontend.trit"
    ],
    "explanation": "Open the source and compare it with the tc:test:next-compiler-native-ulib validation record. This exercise reports the repository boundary instead of pretending that a link is a lesson."
  }
}
---

## Objectives

- Explain bootstrap stages, trust boundaries, self-hosting constraints, compiler tests, and reproducibility in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [self-hosting-bootstrap](/learn/eecs/self-hosting-bootstrap) first. Those lessons introduce the state and vocabulary that this page assumes. This is the build, trace, and evidence lesson for Self-hosted TCL compiler. The goal is to inspect a real repository transition and report both what the current tests prove and what they do not prove. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

Self-hosting means a compiler can eventually build itself, but the route there is a chain of trust. A host compiler may build an early compiler, that compiler may build a later compiler, and the outputs should be compared or tested so a bootstrap stage does not silently introduce a different language or ABI. The important question is not whether a compiler invokes itself, but what is trusted at each stage and how the result is checked. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is whether the repository's files and tests actually carry that invariant through a build or execution step.

## Explanation

tcl_frontend.trit, tcl_backend.trit, and the TreatCode compiler driver describe the repository's self-hosting direction. Native library and compiler pipeline tests validate pieces of the route. The course keeps bootstrap output separate from a claim that the entire toolchain is independently reproducible. The phase enters through a host compiler, TCL compiler sources, and a bootstrap plan and leaves through a staged compiler claim with independent checks for each trust boundary. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

Read the frontend and backend entry points and locate the tests that exercise native ulib or compiler pipeline behavior. Map each stage to its input compiler and output artifact. Then list one threat to the bootstrap trust boundary, such as an untracked host dependency. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

tcl_frontend.trit, tcl_backend.trit, and the TreatCode compiler driver describe the repository's self-hosting direction. Native library and compiler pipeline tests validate pieces of the route. The course keeps bootstrap output separate from a claim that the entire toolchain is independently reproducible. The current registry row is tc:layer:phase-10-self-hosted-compiler, with implementation coverage marked partial and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

A fully self-hosted and independently bootstrapped release remains a stronger goal than the current evidence. The gap is explicit so learners can distinguish native-hosted compilation from a trusted bootstrap chain. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

Stage A uses the host compiler to build a small TCL compiler. Stage B runs that compiler to build the same source again. A meaningful comparison records compiler version, source commit, input files, and output hashes; a visual 'it ran twice' is not enough. If Stage B emits a different object, the difference becomes a reproducibility investigation rather than a silent success. For this lesson, write the example as a sequence: first identify a host compiler, TCL compiler sources, and a bootstrap plan; next apply the self-hosted-compiler rule; then inspect a staged compiler claim with independent checks for each trust boundary; finally compare the result with the named validation record. The repository investigation should begin at tcl_frontend.trit and cross-check the test IDs tc:test:next-compiler-native-ulib, tc:test:next-compiler-pipeline. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

A compiler written in TCL is not automatically self-hosted. Self-hosting requires a working build path, a bootstrap strategy, and evidence that the resulting compiler respects the same contract. Reusing a host compiler behind the scenes can be useful without satisfying that stronger claim. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Draw a three-stage bootstrap graph and attach a falsifiable check to every edge. Include the source commit, expected output identity, and the failure label you would publish if the comparison diverges. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [tcl_frontend.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tcl_frontend.trit) — tcl_frontend.trit
- [tcl_backend.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tcl_backend.trit) — tcl_backend.trit
- [compiler_driver.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/treatcode/compiler_driver.trit) — treatcode/compiler_driver.trit

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/self-hosted-compiler) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:next-compiler-native-ulib test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/04_compiler/next_compiler_native_ulib.cpp) — tests_next/04_compiler/next_compiler_native_ulib.cpp
- [tc:test:next-compiler-pipeline test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/04_compiler/next_compiler_pipeline.cpp) — tests_next/04_compiler/next_compiler_pipeline.cpp

The test IDs attached to this lesson are tc:test:next-compiler-native-ulib, tc:test:next-compiler-pipeline. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase has no benchmark record in the current registry.

## Next step

[Continue to the next lesson](/learn/eecs/runtime-sdk-abi). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
