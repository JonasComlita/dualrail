---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "vm-jit-benchmark",
  "title": "Trace JIT and benchmark evidence",
  "module": "Phase 05 · VM interpreter, dispatch, trace JIT, and multicore execution",
  "level": "beginner",
  "order": 11,
  "summary": "Trace the repository evidence for VM interpreter, dispatch, trace JIT, and multicore execution.",
  "phase_id": "tc:layer:phase-05-vm-execution",
  "phase_slug": "vm-execution",
  "phase_name": "VM interpreter, dispatch, trace JIT, and multicore execution",
  "lesson_kind": "build-trace",
  "implementation_status": "complete",
  "canonical_terms": [
    "interpreter",
    "dispatch",
    "trace JIT",
    "multicore",
    "synchronization"
  ],
  "objectives": [
    "Explain fetch, decode, execute, dispatch, tracing and JIT tradeoffs, scheduling, multicore state, and synchronization.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "vm-dispatch-multicore"
  ],
  "sources": [
    {
      "path": "ternary_vm.h",
      "label": "ternary_vm.h",
      "kind": "source"
    },
    {
      "path": "docs/03_Execution_Engine/decoded_trace_and_native_jit.md",
      "label": "decoded_trace_and_native_jit.md",
      "kind": "documentation"
    },
    {
      "path": "tests/test_vm_widths.cpp",
      "label": "test_vm_widths.cpp",
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
      "path": "tests/test_multiwidth_vm.exe",
      "label": "tc:test:test-multiwidth-vm test source",
      "kind": "test"
    },
    {
      "path": "tests/test_execution_backends_benchmark.cpp",
      "label": "tc:test:test-execution-backends-benchmark test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-multiwidth-vm",
    "tc:test:test-execution-backends-benchmark"
  ],
  "benchmark_ids": [
    "tc:benchmark:test-execution-backends-benchmark"
  ],
  "gap_ids": [
    "tc:gap:trit-gap-deterministic-replay"
  ],
  "stack_links": {
    "phase": "/stack/vm-execution",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_vm.h",
    "tests": "/stack?focus=tc%3Atest%3Atest-multiwidth-vm"
  },
  "next": "assembler-binary-contract",
  "interactive": {
    "kind": "source",
    "title": "Source investigation",
    "prompt": "Find the exact implementation boundary for VM interpreter, dispatch, trace JIT, and multicore execution.",
    "sourcePath": "ternary_vm.h",
    "sourceHref": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_vm.h",
    "expectedIncludes": [
      "ternary_vm.h"
    ],
    "explanation": "Open the source and compare it with the tc:test:test-multiwidth-vm validation record. This exercise reports the repository boundary instead of pretending that a link is a lesson."
  }
}
---

## Objectives

- Explain fetch, decode, execute, dispatch, tracing and JIT tradeoffs, scheduling, multicore state, and synchronization in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [vm-dispatch-multicore](/learn/eecs/vm-dispatch-multicore) first. Those lessons introduce the state and vocabulary that this page assumes. This is the build, trace, and evidence lesson for VM interpreter, dispatch, trace JIT, and multicore execution. The goal is to inspect a real repository transition and report both what the current tests prove and what they do not prove. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

Execution is a loop around state transitions, but the loop's implementation matters. An interpreter is easy to inspect, a dispatch table reduces branching overhead, a trace JIT specializes hot paths, and multicore execution introduces ownership and synchronization. None of those strategies may change the ISA result. A performance path is credible only when it can be compared with a correctness oracle and a stated workload. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is whether the repository's files and tests actually carry that invariant through a build or execution step.

## Explanation

ternary_vm.h owns the execution surface, while the trace/JIT documentation records decoded traces and native backends. VM tests exercise widths and execution, and the benchmark target records backend comparisons. The deterministic replay gap is kept visible because scheduling and native timing need stronger evidence than a single run. The phase enters through a valid VM state and an execution backend choice and leaves through a deterministic or explicitly scheduled sequence of state transitions with measured tradeoffs. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

Compare the VM's dispatch entry point with the decoded trace document and the execution-backend benchmark. Note which fields are recorded for a trace and which outputs are used for correctness. Run the width test before reading the timing so a faster but wrong backend cannot pass the lesson. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

ternary_vm.h owns the execution surface, while the trace/JIT documentation records decoded traces and native backends. VM tests exercise widths and execution, and the benchmark target records backend comparisons. The deterministic replay gap is kept visible because scheduling and native timing need stronger evidence than a single run. The current registry row is tc:layer:phase-05-vm-execution, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Deterministic replay and some multicore scheduling guarantees remain gaps. The course teaches how to describe those limits and how to keep synchronization claims narrower than the current evidence. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

For a three-instruction loop, the interpreter fetches and decodes every iteration. A trace recorder may capture the hot path after the branch is stable and compile a specialized representation. If the input changes the branch outcome, the JIT must leave the trace or deoptimize; it cannot reuse a trace whose guard no longer holds. Two cores additionally need an ownership rule for the shared memory cell. For this lesson, write the example as a sequence: first identify a valid VM state and an execution backend choice; next apply the vm-execution rule; then inspect a deterministic or explicitly scheduled sequence of state transitions with measured tradeoffs; finally compare the result with the named validation record. The repository investigation should begin at ternary_vm.h and cross-check the test IDs tc:test:test-multiwidth-vm, tc:test:test-execution-backends-benchmark. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

A JIT is not a second ISA and a multicore run is not automatically deterministic. JIT code is an implementation of the same architectural transitions. Multicore ordering requires a synchronization contract; a reproducible seed does not by itself prove a reproducible schedule. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Annotate a loop with fetch, decode, dispatch, guard, and exit points. Predict where a trace should be invalidated, then name the benchmark and correctness result you would need to trust the optimization. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [ternary_vm.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_vm.h) — ternary_vm.h
- [decoded_trace_and_native_jit.md](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/docs/03_Execution_Engine/decoded_trace_and_native_jit.md) — docs/03_Execution_Engine/decoded_trace_and_native_jit.md
- [test_vm_widths.cpp](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_vm_widths.cpp) — tests/test_vm_widths.cpp

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/vm-execution) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-multiwidth-vm test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_multiwidth_vm.exe) — tests/test_multiwidth_vm.exe
- [tc:test:test-execution-backends-benchmark test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_execution_backends_benchmark.cpp) — tests/test_execution_backends_benchmark.cpp

The test IDs attached to this lesson are tc:test:test-multiwidth-vm, tc:test:test-execution-backends-benchmark. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase also has benchmark records tc:benchmark:test-execution-backends-benchmark; interpret them only with their workload and baseline.

## Next step

[Continue to the next lesson](/learn/eecs/assembler-binary-contract). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
