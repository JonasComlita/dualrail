---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "hardware-gates",
  "title": "From gates to hardware evidence",
  "module": "Phase 01 · Trit representation, gates, and hardware realization",
  "level": "beginner",
  "order": 3,
  "summary": "Trace the repository evidence for Trit representation, gates, and hardware realization.",
  "phase_id": "tc:layer:phase-01-representation",
  "phase_slug": "representation",
  "phase_name": "Trit representation, gates, and hardware realization",
  "lesson_kind": "build-trace",
  "implementation_status": "complete",
  "canonical_terms": [
    "trit",
    "balanced ternary",
    "lane representation",
    "dual rail",
    "invalid sentinel"
  ],
  "objectives": [
    "Explain trits, ternary encodings, balanced and unbalanced choices, gates, dual-rail realization, timing, and the hardware boundary.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "representation-boundaries"
  ],
  "sources": [
    {
      "path": "ternary_backend.h",
      "label": "ternary_backend.h",
      "kind": "source"
    },
    {
      "path": "ternary_lanes.h",
      "label": "ternary_lanes.h",
      "kind": "source"
    },
    {
      "path": "docs/01_Logic_Level/contract_hdl.md",
      "label": "contract_hdl.md",
      "kind": "documentation"
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
      "path": "tests/test_ternary_lanes.cpp",
      "label": "tc:test:test-ternary-lanes test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-native-ops",
    "tc:test:test-ternary-lanes"
  ],
  "benchmark_ids": [],
  "gap_ids": [],
  "stack_links": {
    "phase": "/stack/representation",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_backend.h",
    "tests": "/stack?focus=tc%3Atest%3Atest-native-ops"
  },
  "next": "isa-vm-execution",
  "interactive": {
    "kind": "trace",
    "title": "Trit representation, gates, and hardware realization trace",
    "prompt": "Advance through the representation transition and inspect the state at each boundary.",
    "steps": [
      {
        "label": "Input contract",
        "state": "a logical trit or a packed word crossing a representation boundary",
        "explanation": "Start with a logical trit or a packed word crossing a representation boundary."
      },
      {
        "label": "State or artifact check",
        "state": "Encode the balanced value sequence [-1, 0, +1] as lanes",
        "explanation": "Open ternary_scalar."
      },
      {
        "label": "Boundary transition",
        "state": "a value whose numeric, lane, and physical meanings are not confused",
        "explanation": "The transition leaves the representation boundary only when its invariant holds."
      },
      {
        "label": "Observable result",
        "state": "Evidence: tc:test:test-native-ops",
        "explanation": "Use tc:test:test-native-ops to validate the observed result."
      }
    ]
  }
}
---

## Objectives

- Explain trits, ternary encodings, balanced and unbalanced choices, gates, dual-rail realization, timing, and the hardware boundary in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [representation-boundaries](/learn/eecs/representation-boundaries) first. Those lessons introduce the state and vocabulary that this page assumes. This is the build, trace, and evidence lesson for Trit representation, gates, and hardware realization. The goal is to inspect a real repository transition and report both what the current tests prove and what they do not prove. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

A representation is a promise about meaning and layout at the same time. Balanced ternary uses digits -1, 0, and +1 for arithmetic; an unbalanced encoding may use 0, 1, and 2 for storage or transport. A gate is a relation over states, while a circuit realization chooses signals, timing, and error behavior. Keeping these layers separate lets a compiler use a convenient host container without pretending that two host bits are themselves a physical ternary device. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is whether the repository's files and tests actually carry that invariant through a build or execution step.

## Explanation

Trit headers define scalar and lane operations, while the logic-level documentation describes dual-rail realization. The packed lane contract uses two bits per trit with 00 for -1, 01 for 0, 10 for +1, and 11 invalid. Numeric widths such as T40 describe positional arithmetic; lane widths such as l40 describe packed tritwise transport. Native operation and lane tests exercise conversions and boundary behavior. The phase enters through a logical trit or a packed word crossing a representation boundary and leaves through a value whose numeric, lane, and physical meanings are not confused. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

Open ternary_scalar.h beside ternary_lanes.h and trace one conversion in the lane tests. Record which function interprets a word numerically and which function preserves lane positions. Then inspect the hardware contract page for timing language; that is where an abstract truth table stops and a signal-level claim begins. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

Trit headers define scalar and lane operations, while the logic-level documentation describes dual-rail realization. The packed lane contract uses two bits per trit with 00 for -1, 01 for 0, 10 for +1, and 11 invalid. Numeric widths such as T40 describe positional arithmetic; lane widths such as l40 describe packed tritwise transport. Native operation and lane tests exercise conversions and boundary behavior. The current registry row is tc:layer:phase-01-representation, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

The repository records the hardware-facing contract and simulation, but a software header is not a manufactured silicon timing proof. Any physical gate, fabrication target, or accelerator claim must remain tied to the evidence status in the registry. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

Encode the balanced value sequence [-1, 0, +1] as lanes. The transport payload is 00 01 10, but the numeric sum is -1 + 0 + 1 = 0. Treating 000110 as a binary integer and adding one to it would answer a different question. The correct workflow decodes each lane, applies the trit operation, and re-encodes the result. For this lesson, write the example as a sequence: first identify a logical trit or a packed word crossing a representation boundary; next apply the representation rule; then inspect a value whose numeric, lane, and physical meanings are not confused; finally compare the result with the named validation record. The repository investigation should begin at ternary_backend.h and cross-check the test IDs tc:test:test-native-ops, tc:test:test-ternary-lanes. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

Balanced ternary is not the same thing as a two-bit lane encoding. The former describes numeric digit values; the latter is a transport convention with an invalid code. A trit enum can name a state without being safe to pass to an arithmetic routine. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Make a table for five values containing the balanced digit, numeric value, lane bits, and physical signal pair. Mark invalid lane code 11 and explain why it must not silently become +1. Use the table to predict a conversion test before running it. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [ternary_backend.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_backend.h) — ternary_backend.h
- [ternary_lanes.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_lanes.h) — ternary_lanes.h
- [contract_hdl.md](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/docs/01_Logic_Level/contract_hdl.md) — docs/01_Logic_Level/contract_hdl.md

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/representation) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-native-ops test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_native_ops.cpp) — tests/test_native_ops.cpp
- [tc:test:test-ternary-lanes test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_ternary_lanes.cpp) — tests/test_ternary_lanes.cpp

The test IDs attached to this lesson are tc:test:test-native-ops, tc:test:test-ternary-lanes. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase has no benchmark record in the current registry.

## Next step

[Continue to the next lesson](/learn/eecs/isa-vm-execution). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
