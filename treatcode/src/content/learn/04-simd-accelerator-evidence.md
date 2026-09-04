---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "simd-accelerator-evidence",
  "title": "SIMD and accelerator evidence",
  "module": "Phase 03 · Numeric model, native operations, lanes, SIMD, and accelerators",
  "level": "beginner",
  "order": 7,
  "summary": "Trace the repository evidence for Numeric model, native operations, lanes, SIMD, and accelerators.",
  "phase_id": "tc:layer:phase-03-numeric-operations",
  "phase_slug": "numeric-operations",
  "phase_name": "Numeric model, native operations, lanes, SIMD, and accelerators",
  "lesson_kind": "build-trace",
  "implementation_status": "complete",
  "canonical_terms": [
    "numeric representation",
    "lane",
    "SIMD",
    "saturation",
    "accelerator"
  ],
  "objectives": [
    "Explain numeric versus lane representation, arithmetic semantics, vector and SIMD behavior, and accelerator limits.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "tritwise-operations"
  ],
  "sources": [
    {
      "path": "ternary_scalar.h",
      "label": "ternary_scalar.h",
      "kind": "source"
    },
    {
      "path": "ternary_native_ops.h",
      "label": "ternary_native_ops.h",
      "kind": "source"
    },
    {
      "path": "ternary_simd.h",
      "label": "ternary_simd.h",
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
      "path": "tests/test_native_ops.cpp",
      "label": "tc:test:test-native-ops test source",
      "kind": "test"
    },
    {
      "path": "tests/test_numeric_workloads.cpp",
      "label": "tc:test:test-numeric-workloads test source",
      "kind": "test"
    },
    {
      "path": "tests/test_ternary_lanes.cpp",
      "label": "tc:test:test-ternary-lanes test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-native-ops",
    "tc:test:test-numeric-workloads",
    "tc:test:test-ternary-lanes"
  ],
  "benchmark_ids": [
    "tc:benchmark:test-execution-backends-benchmark"
  ],
  "gap_ids": [],
  "stack_links": {
    "phase": "/stack/numeric-operations",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_scalar.h",
    "tests": "/stack?focus=tc%3Atest%3Atest-native-ops"
  },
  "next": "vm-state-memory",
  "interactive": {
    "kind": "code",
    "title": "Run a bounded compiler check",
    "starter": "import ulib;\n\nfn first_value() -> t40 {\n    return 1 + 2;\n}",
    "expectedIncludes": [
      "fn first_value",
      "return 1 + 2"
    ],
    "exercise_id": "simd-accelerator-evidence",
    "runner": "treatcode-learning-compiler",
    "explanation": "Submit the bounded TCL example to the repository-backed learning runner. It may pass or report a real compiler failure; either result is evidence about the Numeric model, native operations, lanes, SIMD, and accelerators exercise, not a local text-only success."
  }
}
---

## Objectives

- Explain numeric versus lane representation, arithmetic semantics, vector and SIMD behavior, and accelerator limits in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [tritwise-operations](/learn/eecs/tritwise-operations) first. Those lessons introduce the state and vocabulary that this page assumes. This is the build, trace, and evidence lesson for Numeric model, native operations, lanes, SIMD, and accelerators. The goal is to inspect a real repository transition and report both what the current tests prove and what they do not prove. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

Numeric computation asks what a value means; lane computation asks how corresponding positions interact. SIMD can accelerate independent lanes, but it does not erase the distinction between a packed transport word and a scalar number. Width, sign, carry, saturation, and invalid-code policy all need an explicit contract before a fast path can be trusted. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is whether the repository's files and tests actually carry that invariant through a build or execution step.

## Explanation

The scalar and native-operations headers define positional arithmetic and ternary operators. SIMD and lane headers provide packed transformations, while workload tests compare expected behavior. Accelerator support is optional and is validated only when its toolchain is configured; the CPU path remains the reference for correctness. The phase enters through scalar values, packed lanes, or a vector operation request and leaves through an arithmetic result with width, representation, and overflow semantics recorded. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

Run the native-ops and lane tests, then read the SIMD helper that maps a lane code to a ternary value. Check the benchmark record for which backend and width it measures. If an accelerator is unavailable, record that limitation and compare the CPU oracle instead of substituting a fabricated timing. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

The scalar and native-operations headers define positional arithmetic and ternary operators. SIMD and lane headers provide packed transformations, while workload tests compare expected behavior. Accelerator support is optional and is validated only when its toolchain is configured; the CPU path remains the reference for correctness. The current registry row is tc:layer:phase-03-numeric-operations, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Some accelerator and external workload assets are represented as capabilities or gaps rather than shipped execution. A benchmark result without the target profile, representation, and correctness gate is not a product claim. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

Take two lanes encoded as [-1, +1, 0] and [0, +1, -1]. A lane-wise minimum yields [-1, +1, -1]. Interpreting both packed payloads as base-three scalars and minimizing the aggregate numbers would discard the lane positions and produce a different result. The exercise is to name the operation before choosing the implementation. For this lesson, write the example as a sequence: first identify scalar values, packed lanes, or a vector operation request; next apply the numeric-operations rule; then inspect an arithmetic result with width, representation, and overflow semantics recorded; finally compare the result with the named validation record. The repository investigation should begin at ternary_scalar.h and cross-check the test IDs tc:test:test-native-ops, tc:test:test-numeric-workloads, tc:test:test-ternary-lanes. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

More bits per host word do not automatically mean more numeric range. A 40-trit scalar width and a 40-lane packed width can occupy different layouts and obey different operators. SIMD is an execution strategy, not a new mathematical meaning. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Classify six operations as scalar, lane-wise, reduction, or accelerator-specific. For two of them, write the expected result by hand and identify the exact test or oracle that should falsify your answer. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [ternary_scalar.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_scalar.h) — ternary_scalar.h
- [ternary_native_ops.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_native_ops.h) — ternary_native_ops.h
- [ternary_simd.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_simd.h) — ternary_simd.h

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/numeric-operations) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-native-ops test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_native_ops.cpp) — tests/test_native_ops.cpp
- [tc:test:test-numeric-workloads test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_numeric_workloads.cpp) — tests/test_numeric_workloads.cpp
- [tc:test:test-ternary-lanes test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_ternary_lanes.cpp) — tests/test_ternary_lanes.cpp

The test IDs attached to this lesson are tc:test:test-native-ops, tc:test:test-numeric-workloads, tc:test:test-ternary-lanes. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase also has benchmark records tc:benchmark:test-execution-backends-benchmark; interpret them only with their workload and baseline.

## Next step

[Continue to the next lesson](/learn/eecs/vm-state-memory). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
