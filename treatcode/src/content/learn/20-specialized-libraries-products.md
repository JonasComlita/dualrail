---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "specialized-libraries-products",
  "title": "Specialized libraries and products",
  "module": "Phase 19 · Specialized libraries and developer products",
  "level": "eecs",
  "order": 38,
  "summary": "Build the mental model for Specialized libraries and developer products.",
  "phase_id": "tc:layer:phase-19-specialized-products",
  "phase_slug": "specialized-products",
  "phase_name": "Specialized libraries and developer products",
  "lesson_kind": "mental-model",
  "implementation_status": "partial",
  "canonical_terms": [
    "extension point",
    "specialized library",
    "workload profile",
    "product boundary",
    "capability envelope"
  ],
  "objectives": [
    "Explain libraries and tooling, product boundaries, extension points, and how contributors use the platform.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "release-loading-recovery"
  ],
  "sources": [
    {
      "path": "ternary_montgomery.h",
      "label": "ternary_montgomery.h",
      "kind": "source"
    },
    {
      "path": "ternary_transformer_runtime.h",
      "label": "ternary_transformer_runtime.h",
      "kind": "source"
    },
    {
      "path": "treatcode/src/App.tsx",
      "label": "App.tsx",
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
      "path": "tests/test_numeric_workloads.cpp",
      "label": "tc:test:test-numeric-workloads test source",
      "kind": "test"
    },
    {
      "path": "tests/test_benchmark.cpp",
      "label": "tc:test:test-benchmark test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-numeric-workloads",
    "tc:test:test-benchmark"
  ],
  "benchmark_ids": [
    "tc:benchmark:benchmark-bitnet-os",
    "tc:benchmark:benchmark-doom-os"
  ],
  "gap_ids": [
    "tc:gap:trit-gap-full-doom-assets",
    "tc:gap:trit-gap-full-bitnet-assets"
  ],
  "stack_links": {
    "phase": "/stack/specialized-products",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_montgomery.h",
    "tests": "/stack?focus=tc%3Atest%3Atest-numeric-workloads"
  },
  "next": "developer-extension-workflow",
  "interactive": {
    "kind": "choice",
    "title": "Concept check",
    "prompt": "Which statement keeps the specialized-products boundary honest?",
    "options": [
      "The exact source and validation contract",
      "A nearby concept with no evidence",
      "A UI-only state change"
    ],
    "answer": 1,
    "explanation": "The repository boundary is defined by the current source and validation evidence for Specialized libraries and developer products. Planned and unavailable work remains labelled."
  }
}
---

## Objectives

- Explain libraries and tooling, product boundaries, extension points, and how contributors use the platform in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [release-loading-recovery](/learn/eecs/release-loading-recovery) first. Those lessons introduce the state and vocabulary that this page assumes. This is the mental-model lesson for Specialized libraries and developer products. Begin with the general systems idea, then compare it with the current Trit boundary before you touch a tool. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

Specialized libraries and products are where a general platform meets a demanding workload. Cryptographic arithmetic, transformer runtimes, benchmark tooling, and TreatCode UI each need an extension point and a correctness boundary. A product is not just a library file: it includes the inputs it supports, the performance profile it claims, the tests that protect it, and the gaps that limit distribution. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is not only “what does Trit call this?” but “what invariant would another computer system need at the same boundary?”

## Explanation

ternary_montgomery.h, ternary_transformer_runtime.h, and treatcode/src/App.tsx represent current specialized library and product surfaces. Numeric workload and benchmark tests provide evidence, while BitNet and Doom assets are recorded as gaps where full external inputs are unavailable. The phase enters through a platform extension request or specialized workload and leaves through a product boundary with ownership, evidence, and an honest capability label. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

A Montgomery multiplication helper can be correct for a fixed width and still be unsuitable for an unsupported modulus or accelerator layout. A transformer runtime can expose a tensor operation while its external model assets remain unavailable. The product boundary says which shapes, widths, and artifacts are accepted and what happens outside them. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

ternary_montgomery.h, ternary_transformer_runtime.h, and treatcode/src/App.tsx represent current specialized library and product surfaces. Numeric workload and benchmark tests provide evidence, while BitNet and Doom assets are recorded as gaps where full external inputs are unavailable. The current registry row is tc:layer:phase-19-specialized-products, with implementation coverage marked partial and tested coverage marked partial. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Full external workload assets and some product integrations remain planned. A local benchmark or partial library implementation must carry its scope and cannot be promoted to a complete product claim. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

A Montgomery multiplication helper can be correct for a fixed width and still be unsuitable for an unsupported modulus or accelerator layout. A transformer runtime can expose a tensor operation while its external model assets remain unavailable. The product boundary says which shapes, widths, and artifacts are accepted and what happens outside them. For this lesson, write the example as a sequence: first identify a platform extension request or specialized workload; next apply the specialized-products rule; then inspect a product boundary with ownership, evidence, and an honest capability label; finally compare the result with the named validation record. The conceptual check should explain why the result would be wrong if a neighboring representation or layer were substituted. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

A benchmark name is not a product specification, and a library header is not proof of every width or workload. Specialized code needs a bounded capability envelope and a contributor workflow that preserves evidence. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Write a one-page extension proposal for a new specialized library. Include API boundary, supported representation, correctness test, benchmark profile, source ownership, and explicit unsupported cases. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [ternary_montgomery.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_montgomery.h) — ternary_montgomery.h
- [ternary_transformer_runtime.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_transformer_runtime.h) — ternary_transformer_runtime.h
- [App.tsx](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/treatcode/src/App.tsx) — treatcode/src/App.tsx

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/specialized-products) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-numeric-workloads test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_numeric_workloads.cpp) — tests/test_numeric_workloads.cpp
- [tc:test:test-benchmark test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_benchmark.cpp) — tests/test_benchmark.cpp

The test IDs attached to this lesson are tc:test:test-numeric-workloads, tc:test:test-benchmark. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase also has benchmark records tc:benchmark:benchmark-bitnet-os, tc:benchmark:benchmark-doom-os; interpret them only with their workload and baseline.

## Next step

[Continue to the next lesson](/learn/eecs/developer-extension-workflow). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
