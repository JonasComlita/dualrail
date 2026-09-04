---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "full-system-validation",
  "title": "Build a full-system validation trail",
  "module": "Phase 20 · Security, fuzzing, performance, and full-system closure",
  "level": "eecs",
  "order": 41,
  "summary": "Trace the repository evidence for Security, fuzzing, performance, and full-system closure.",
  "phase_id": "tc:layer:phase-20-closure-evidence",
  "phase_slug": "closure-evidence",
  "phase_name": "Security, fuzzing, performance, and full-system closure",
  "lesson_kind": "build-trace",
  "implementation_status": "partial",
  "canonical_terms": [
    "threat model",
    "fuzzing",
    "benchmark baseline",
    "performance tradeoff",
    "system closure"
  ],
  "objectives": [
    "Explain threat model, isolation, fuzzing, benchmark interpretation, performance tradeoffs, and end-to-end validation.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "security-fuzzing-performance-closure"
  ],
  "sources": [
    {
      "path": "tests_next/manifests/coverage_matrix.json",
      "label": "coverage_matrix.json",
      "kind": "manifest"
    },
    {
      "path": "tests_next/manifests/acceptance_gates.json",
      "label": "acceptance_gates.json",
      "kind": "manifest"
    },
    {
      "path": "KNOWN_GAPS.md",
      "label": "KNOWN_GAPS.md",
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
    },
    {
      "path": "tests/test_production_layers.cpp",
      "label": "tc:test:test-production-layers test source",
      "kind": "test"
    },
    {
      "path": "tests/test_production_hardening.cpp",
      "label": "tc:test:test-production-hardening test source",
      "kind": "test"
    },
    {
      "path": "tests_next/17_full_system/next_full_system_release.cpp",
      "label": "tc:test:next-full-system-release test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-agent-tooling",
    "tc:test:test-production-layers",
    "tc:test:test-production-hardening",
    "tc:test:next-full-system-release"
  ],
  "benchmark_ids": [
    "tc:benchmark:benchmark-doom-os",
    "tc:benchmark:benchmark-bitnet-os"
  ],
  "gap_ids": [
    "tc:gap:trit-gap-fuzz-harnesses",
    "tc:gap:trit-gap-benchmark-baselines"
  ],
  "stack_links": {
    "phase": "/stack/closure-evidence",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/manifests/coverage_matrix.json",
    "tests": "/stack?focus=tc%3Atest%3Atest-agent-tooling"
  },
  "next": null,
  "interactive": {
    "kind": "code",
    "title": "Run a bounded compiler check",
    "starter": "import ulib;\n\nfn first_value() -> t40 {\n    return 1 + 2;\n}",
    "expectedIncludes": [
      "fn first_value",
      "return 1 + 2"
    ],
    "exercise_id": "full-system-validation",
    "runner": "treatcode-learning-compiler",
    "explanation": "Submit the bounded TCL example to the repository-backed learning runner. It may pass or report a real compiler failure; either result is evidence about the Security, fuzzing, performance, and full-system closure exercise, not a local text-only success."
  }
}
---

## Objectives

- Explain threat model, isolation, fuzzing, benchmark interpretation, performance tradeoffs, and end-to-end validation in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [security-fuzzing-performance-closure](/learn/eecs/security-fuzzing-performance-closure) first. Those lessons introduce the state and vocabulary that this page assumes. This is the build, trace, and evidence lesson for Security, fuzzing, performance, and full-system closure. The goal is to inspect a real repository transition and report both what the current tests prove and what they do not prove. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

Closure is not a final screenshot; it is the discipline of checking the stack as a connected system. Security asks what can go wrong and across which boundary. Fuzzing searches malformed or surprising inputs. Benchmarks quantify a workload under a profile and require correctness first. Full-system validation then ties boot, kernel, storage, devices, user space, packaging, and observability into one acceptance story. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is whether the repository's files and tests actually carry that invariant through a build or execution step.

## Explanation

tests_next/manifests/coverage_matrix.json and acceptance_gates.json define future-facing closure structure, while KNOWN_GAPS.md records unresolved work. Production-layer, hardening, agent-tooling, and full-system release tests provide current evidence; Doom and BitNet benchmark records remain bounded by their assets and baselines. The phase enters through a release candidate and a set of correctness, security, and performance claims and leaves through a full-system conclusion with scoped evidence and explicit remaining gaps. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

Read the acceptance gates and coverage matrix, then compare their rows with production test commands and known gaps. Trace one full-system failure backwards from its user-visible symptom to the first violated contract. Record which parts are current, planned, experimental, or unavailable. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

tests_next/manifests/coverage_matrix.json and acceptance_gates.json define future-facing closure structure, while KNOWN_GAPS.md records unresolved work. Production-layer, hardening, agent-tooling, and full-system release tests provide current evidence; Doom and BitNet benchmark records remain bounded by their assets and baselines. The current registry row is tc:layer:phase-20-closure-evidence, with implementation coverage marked partial and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Fuzz harnesses, benchmark baselines, crash recovery, and external assets are not all complete. The correct closure result can therefore be 'verified slice with gaps' rather than a universal green claim. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

A release test boots an image, launches a process, writes a file, sends a service request, and records the result. A security check asks whether an untrusted pointer or malformed image can cross each boundary. A benchmark repeats the same workload with a correctness oracle and reports p50 or another declared statistic. If the workload asset is missing, the result is unavailable, not zero. For this lesson, write the example as a sequence: first identify a release candidate and a set of correctness, security, and performance claims; next apply the closure-evidence rule; then inspect a full-system conclusion with scoped evidence and explicit remaining gaps; finally compare the result with the named validation record. The repository investigation should begin at tests_next/manifests/coverage_matrix.json and cross-check the test IDs tc:test:test-agent-tooling, tc:test:test-production-layers, tc:test:test-production-hardening, tc:test:next-full-system-release. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

A passing smoke test is not full-system closure, and a faster run is not a better implementation if correctness changed. Fuzzing finds examples but does not prove all inputs safe; a benchmark without a baseline and workload profile cannot support a general performance claim. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Create a closure report for one end-to-end scenario. Include threat, oracle, workload, timing statistic, evidence paths, and an explicit residual-gap section. Do not use the word complete unless every required gate has a named result. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [coverage_matrix.json](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/manifests/coverage_matrix.json) — tests_next/manifests/coverage_matrix.json
- [acceptance_gates.json](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/manifests/acceptance_gates.json) — tests_next/manifests/acceptance_gates.json
- [KNOWN_GAPS.md](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/KNOWN_GAPS.md) — KNOWN_GAPS.md

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/closure-evidence) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-agent-tooling test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_agent_tooling.py) — tests/test_agent_tooling.py
- [tc:test:test-production-layers test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_production_layers.cpp) — tests/test_production_layers.cpp
- [tc:test:test-production-hardening test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_production_hardening.cpp) — tests/test_production_hardening.cpp
- [tc:test:next-full-system-release test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/17_full_system/next_full_system_release.cpp) — tests_next/17_full_system/next_full_system_release.cpp

The test IDs attached to this lesson are tc:test:test-agent-tooling, tc:test:test-production-layers, tc:test:test-production-hardening, tc:test:next-full-system-release. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase also has benchmark records tc:benchmark:benchmark-doom-os, tc:benchmark:benchmark-bitnet-os; interpret them only with their workload and baseline.

## Next step

the course closure report. The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
