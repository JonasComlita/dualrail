---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "authority-claims",
  "title": "Authority and safe claims",
  "module": "Phase 00 · Authority, build graph, and review controls",
  "level": "beginner",
  "order": 0,
  "summary": "Build the mental model for Authority, build graph, and review controls.",
  "phase_id": "tc:layer:phase-00-authority",
  "phase_slug": "authority",
  "phase_name": "Authority, build graph, and review controls",
  "lesson_kind": "mental-model",
  "implementation_status": "complete",
  "canonical_terms": [
    "source of truth",
    "reproducible build",
    "evidence record",
    "coverage matrix",
    "provenance"
  ],
  "objectives": [
    "Explain source of truth, reproducible builds, tests, evidence, and safe reading of project claims.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [],
  "sources": [
    {
      "path": "AGENTS.md",
      "label": "AGENTS.md",
      "kind": "source"
    },
    {
      "path": "TEST_MANIFEST.json",
      "label": "Authoritative test manifest",
      "kind": "manifest"
    },
    {
      "path": "tools/trit_tool.py",
      "label": "trit_tool.py",
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
      "path": "tests/test_agent_tooling.py",
      "label": "tc:test:test-agent-tooling test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-agent-tooling"
  ],
  "benchmark_ids": [],
  "gap_ids": [
    "tc:gap:trit-gap-benchmark-baselines"
  ],
  "stack_links": {
    "phase": "/stack/authority",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/AGENTS.md",
    "tests": "/stack?focus=tc%3Atest%3Atest-agent-tooling"
  },
  "next": "authority-build-evidence",
  "interactive": {
    "kind": "choice",
    "title": "Concept check",
    "prompt": "Which statement keeps the authority boundary honest?",
    "options": [
      "The exact source and validation contract",
      "A nearby concept with no evidence",
      "A UI-only state change"
    ],
    "answer": 1,
    "explanation": "The repository boundary is defined by the current source and validation evidence for Authority, build graph, and review controls. Planned and unavailable work remains labelled."
  }
}
---

## Objectives

- Explain source of truth, reproducible builds, tests, evidence, and safe reading of project claims in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

No earlier Learn page is required. The Stack Explorer phase still records repository dependencies, so use the source and phase links if a term is unfamiliar. This is the mental-model lesson for Authority, build graph, and review controls. Begin with the general systems idea, then compare it with the current Trit boundary before you touch a tool. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

Systems work is easier to trust when a claim names the artifact that owns it, the transformation that produced it, and the test that can falsify it. A README can explain intent, but a manifest, source span, generated index, or test result carries a different kind of authority. Reproducibility is the habit of recording enough context that a second reader can repeat the same check rather than relying on memory or an attractive demo. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is not only “what does Trit call this?” but “what invariant would another computer system need at the same boundary?”

## Explanation

TreatCode keeps these distinctions in AGENTS.md, TEST_MANIFEST.json, ROADMAP_STATUS.json, and tools/trit_tool.py. The public snapshot also carries a repository, commit, generation time, and source label. The stack registry points each phase at source references, test IDs, benchmarks, and gaps. A green command is evidence for the command's contract; it is not permission to claim that an unrelated future feature exists. The phase enters through a repository claim, a manifest row, or a test result and leaves through a bounded, commit-addressed statement that another learner can reproduce. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

Suppose a learner reads that a release image can boot. The defensible trail starts with IMAGE_FORMAT_MANIFEST.json for the format, build_tos_image.cpp for construction, a release test for the observed behavior, and a snapshot commit for freshness. If one link is absent, the conclusion becomes partial rather than complete. The same method applies to a compiler claim, a kernel claim, and a benchmark claim. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

TreatCode keeps these distinctions in AGENTS.md, TEST_MANIFEST.json, ROADMAP_STATUS.json, and tools/trit_tool.py. The public snapshot also carries a repository, commit, generation time, and source label. The stack registry points each phase at source references, test IDs, benchmarks, and gaps. A green command is evidence for the command's contract; it is not permission to claim that an unrelated future feature exists. The current registry row is tc:layer:phase-00-authority, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Benchmark baselines remain a recorded gap, so a lesson may teach how to compare a baseline without inventing a performance number. The safe label for unavailable evidence is not_available or missing, and the Learn route carries that label through to the Stack Explorer. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

Suppose a learner reads that a release image can boot. The defensible trail starts with IMAGE_FORMAT_MANIFEST.json for the format, build_tos_image.cpp for construction, a release test for the observed behavior, and a snapshot commit for freshness. If one link is absent, the conclusion becomes partial rather than complete. The same method applies to a compiler claim, a kernel claim, and a benchmark claim. For this lesson, write the example as a sequence: first identify a repository claim, a manifest row, or a test result; next apply the authority rule; then inspect a bounded, commit-addressed statement that another learner can reproduce; finally compare the result with the named validation record. The conceptual check should explain why the result would be wrong if a neighboring representation or layer were substituted. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

A passing test does not certify the entire dependency stack. It certifies the assertions and inputs named by that test at a particular source state. A source link without a test is a design pointer, while a test name without a readable source or result is an incomplete trail. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Write a three-column note for one stack phase: claim, authoritative artifact, falsifying check. Include the snapshot commit and mark every planned or unavailable item explicitly. Your note is finished only when a reader who did not watch your work can follow the same path. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [AGENTS.md](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/AGENTS.md) — AGENTS.md
- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [trit_tool.py](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tools/trit_tool.py) — tools/trit_tool.py

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/authority) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-agent-tooling test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_agent_tooling.py) — tests/test_agent_tooling.py

The test IDs attached to this lesson are tc:test:test-agent-tooling. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase has no benchmark record in the current registry.

## Next step

[Continue to the next lesson](/learn/eecs/authority-build-evidence). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
