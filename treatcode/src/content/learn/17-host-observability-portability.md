---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "host-observability-portability",
  "title": "Inspect native integration and portability",
  "module": "Phase 16 · Host OS model and native integration",
  "level": "programmer",
  "order": 33,
  "summary": "Trace the repository evidence for Host OS model and native integration.",
  "phase_id": "tc:layer:phase-16-host-runtime",
  "phase_slug": "host-runtime",
  "phase_name": "Host OS model and native integration",
  "lesson_kind": "build-trace",
  "implementation_status": "complete",
  "canonical_terms": [
    "host",
    "guest",
    "native integration",
    "virtualization",
    "observability"
  ],
  "objectives": [
    "Explain host and guest boundaries, native runtime integration, portability, virtualization assumptions, and observability.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "host-runtime-integration"
  ],
  "sources": [
    {
      "path": "ternary_host_runtime.h",
      "label": "ternary_host_runtime.h",
      "kind": "source"
    },
    {
      "path": "run_tos_sdl.cpp",
      "label": "run_tos_sdl.cpp",
      "kind": "source"
    },
    {
      "path": "ternary_consumer_shell.h",
      "label": "ternary_consumer_shell.h",
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
      "path": "tests/test_host_runtime.cpp",
      "label": "tc:test:test-host-runtime test source",
      "kind": "test"
    },
    {
      "path": "tests/test_consumer_shell_productization.cpp",
      "label": "tc:test:test-consumer-shell-productization test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-host-runtime",
    "tc:test:test-consumer-shell-productization"
  ],
  "benchmark_ids": [
    "tc:benchmark:test-benchmark"
  ],
  "gap_ids": [
    "tc:gap:trit-gap-deterministic-replay",
    "tc:gap:trit-gap-framebuffer-png"
  ],
  "stack_links": {
    "phase": "/stack/host-runtime",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_host_runtime.h",
    "tests": "/stack?focus=tc%3Atest%3Atest-host-runtime"
  },
  "next": "applications-and-validation",
  "interactive": {
    "kind": "source",
    "title": "Source investigation",
    "prompt": "Find the exact implementation boundary for Host OS model and native integration.",
    "sourcePath": "ternary_host_runtime.h",
    "sourceHref": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_host_runtime.h",
    "expectedIncludes": [
      "ternary_host_runtime.h"
    ],
    "explanation": "Open the source and compare it with the tc:test:test-host-runtime validation record. This exercise reports the repository boundary instead of pretending that a link is a lesson."
  }
}
---

## Objectives

- Explain host and guest boundaries, native runtime integration, portability, virtualization assumptions, and observability in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [host-runtime-integration](/learn/eecs/host-runtime-integration) first. Those lessons introduce the state and vocabulary that this page assumes. This is the build, trace, and evidence lesson for Host OS model and native integration. The goal is to inspect a real repository transition and report both what the current tests prove and what they do not prove. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

A hosted OS or VM borrows resources from a host operating system. The host owns threads, files, windows, clocks, and process isolation; the guest owns its architectural model and guest resources. Integration is valuable because it makes the system runnable, but it can also hide assumptions. A sound model names which state is guest state, which timing is host timing, and which failures are translation failures. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is whether the repository's files and tests actually carry that invariant through a build or execution step.

## Explanation

ternary_host_runtime.h, run_tos_sdl.cpp, and ternary_consumer_shell.h implement the current native integration and consumer shell boundaries. Host-runtime and productization tests provide evidence. Deterministic replay and framebuffer PNG are recorded gaps, so observability claims are narrower than a full emulator trace. The phase enters through a guest operation executed by a host process and leaves through a host integration claim that names the boundary, portability assumptions, and observability. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

Read the host runtime adapter beside the host-runtime test and shell productization test. Annotate every crossing: thread, file, window, clock, and memory. Then check which gap describes missing replay or screenshot evidence. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

ternary_host_runtime.h, run_tos_sdl.cpp, and ternary_consumer_shell.h implement the current native integration and consumer shell boundaries. Host-runtime and productization tests provide evidence. Deterministic replay and framebuffer PNG are recorded gaps, so observability claims are narrower than a full emulator trace. The current registry row is tc:layer:phase-16-host-runtime, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Portability across hosts, deterministic replay, and artifact-level framebuffer capture need additional evidence. A host run can be current while a portable or replayable run remains planned. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

A guest write to a virtual display becomes a host buffer update, and the host window toolkit presents it later on its own event loop. The guest may report a deterministic frame index, but host presentation time can vary. If a host file operation fails, the error must be translated into a guest status without leaking a host pointer or path assumption. For this lesson, write the example as a sequence: first identify a guest operation executed by a host process; next apply the host-runtime rule; then inspect a host integration claim that names the boundary, portability assumptions, and observability; finally compare the result with the named validation record. The repository investigation should begin at ternary_host_runtime.h and cross-check the test IDs tc:test:test-host-runtime, tc:test:test-consumer-shell-productization. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

Running on a host is not the same as running on bare hardware, and a host timestamp is not a guest architectural clock. Portability requires more than compiling the same source; it requires the boundary assumptions to be tested. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Make a host/guest ownership table for one frame and one file request. Include lifecycle, error translation, and the observable evidence available at each side. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [ternary_host_runtime.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_host_runtime.h) — ternary_host_runtime.h
- [run_tos_sdl.cpp](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/run_tos_sdl.cpp) — run_tos_sdl.cpp
- [ternary_consumer_shell.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_consumer_shell.h) — ternary_consumer_shell.h

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/host-runtime) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-host-runtime test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_host_runtime.cpp) — tests/test_host_runtime.cpp
- [tc:test:test-consumer-shell-productization test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_consumer_shell_productization.cpp) — tests/test_consumer_shell_productization.cpp

The test IDs attached to this lesson are tc:test:test-host-runtime, tc:test:test-consumer-shell-productization. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase also has benchmark records tc:benchmark:test-benchmark; interpret them only with their workload and baseline.

## Next step

[Continue to the next lesson](/learn/eecs/applications-and-validation). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
