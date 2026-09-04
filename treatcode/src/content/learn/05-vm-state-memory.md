---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "vm-state-memory",
  "title": "VM state and memory",
  "module": "Phase 04 · VM architectural state and memory system",
  "level": "beginner",
  "order": 8,
  "summary": "Build the mental model for VM architectural state and memory system.",
  "phase_id": "tc:layer:phase-04-vm-state",
  "phase_slug": "vm-state",
  "phase_name": "VM architectural state and memory system",
  "lesson_kind": "mental-model",
  "implementation_status": "complete",
  "canonical_terms": [
    "program counter",
    "guest address",
    "architectural state",
    "stack pointer",
    "fault vector"
  ],
  "objectives": [
    "Explain PC and register state, memory and addressing, stack behavior, fetch state, vectors, faults, and architectural invariants.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "simd-accelerator-evidence"
  ],
  "sources": [
    {
      "path": "ternary_vm_state.h",
      "label": "ternary_vm_state.h",
      "kind": "source"
    },
    {
      "path": "docs/03_Execution_Engine/vm_state.md",
      "label": "vm_state.md",
      "kind": "documentation"
    },
    {
      "path": "docs/03_Execution_Engine/memory_model.md",
      "label": "memory_model.md",
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
      "path": "tests/test_multiwidth_vm.exe",
      "label": "tc:test:test-multiwidth-vm test source",
      "kind": "test"
    },
    {
      "path": "tests/test_architecture_v2.cpp",
      "label": "tc:test:test-architecture-v2 test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-multiwidth-vm",
    "tc:test:test-architecture-v2"
  ],
  "benchmark_ids": [
    "tc:benchmark:test-execution-backends-benchmark"
  ],
  "gap_ids": [],
  "stack_links": {
    "phase": "/stack/vm-state",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_vm_state.h",
    "tests": "/stack?focus=tc%3Atest%3Atest-multiwidth-vm"
  },
  "next": "vm-memory-trace",
  "interactive": {
    "kind": "trace",
    "title": "VM architectural state and memory system trace",
    "prompt": "Advance through the vm-state transition and inspect the state at each boundary.",
    "steps": [
      {
        "label": "Input contract",
        "state": "a VM snapshot and a fetch address",
        "explanation": "Start with a VM snapshot and a fetch address."
      },
      {
        "label": "State or artifact check",
        "state": "Start with PC=12, r13=2, and a stack pointer at 100",
        "explanation": "Read the VM state struct and trace a multiwidth test from initialization through fetch."
      },
      {
        "label": "Boundary transition",
        "state": "a next state that preserves the VM invariants or reports a defined fault",
        "explanation": "The transition leaves the vm-state boundary only when its invariant holds."
      },
      {
        "label": "Observable result",
        "state": "Evidence: tc:test:test-multiwidth-vm",
        "explanation": "Use tc:test:test-multiwidth-vm to validate the observed result."
      }
    ]
  }
}
---

## Objectives

- Explain PC and register state, memory and addressing, stack behavior, fetch state, vectors, faults, and architectural invariants in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [simd-accelerator-evidence](/learn/eecs/simd-accelerator-evidence) first. Those lessons introduce the state and vocabulary that this page assumes. This is the mental-model lesson for VM architectural state and memory system. Begin with the general systems idea, then compare it with the current Trit boundary before you touch a tool. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

A virtual machine is a state transition system. Its meaning lives in the relation between program counter, registers, memory, stack, vector tables, and fault state. A useful state model makes illegal addresses, misaligned words, and register-width errors observable. It also gives tests a stable vocabulary: instead of saying that a program 'seems to work', a test can assert the exact next PC and memory cell. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is not only “what does Trit call this?” but “what invariant would another computer system need at the same boundary?”

## Explanation

ternary_vm_state.h and the execution-engine documentation describe architectural state and memory model. Multiwidth VM tests cover state at different widths, and architecture tests exercise invariants. The memory model distinguishes address calculations, stored words, and faults so a host pointer is not confused with a guest address. The phase enters through a VM snapshot and a fetch address and leaves through a next state that preserves the VM invariants or reports a defined fault. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

Start with PC=12, r13=2, and a stack pointer at 100. A load from guest address 100 reads the guest memory cell at 100; it does not read host byte 100. After a two-trit instruction, the PC advances according to the instruction width unless a branch or fault changes it. A read outside the mapped range must produce a fault state that the caller can inspect. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

ternary_vm_state.h and the execution-engine documentation describe architectural state and memory model. Multiwidth VM tests cover state at different widths, and architecture tests exercise invariants. The memory model distinguishes address calculations, stored words, and faults so a host pointer is not confused with a guest address. The current registry row is tc:layer:phase-04-vm-state, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

A full MMU and hardware fault model is taught later as kernel work. The VM lesson uses the state that is implemented and labels any hardware-only assumption as a planned boundary. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

Start with PC=12, r13=2, and a stack pointer at 100. A load from guest address 100 reads the guest memory cell at 100; it does not read host byte 100. After a two-trit instruction, the PC advances according to the instruction width unless a branch or fault changes it. A read outside the mapped range must produce a fault state that the caller can inspect. For this lesson, write the example as a sequence: first identify a VM snapshot and a fetch address; next apply the vm-state rule; then inspect a next state that preserves the VM invariants or reports a defined fault; finally compare the result with the named validation record. The conceptual check should explain why the result would be wrong if a neighboring representation or layer were substituted. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

The host process's pointer and the guest machine's address are different namespaces. The VM can store a guest address in a host integer, but that does not grant the guest access to host memory. Likewise, the stack pointer is a convention inside the guest state, not the host call stack. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Build a four-row state table for fetch, decode, execute, and fault. Include PC, one register, one memory location, and the fault/vector field. Explain which values are architectural and which are implementation details of the host interpreter. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [ternary_vm_state.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_vm_state.h) — ternary_vm_state.h
- [vm_state.md](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/docs/03_Execution_Engine/vm_state.md) — docs/03_Execution_Engine/vm_state.md
- [memory_model.md](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/docs/03_Execution_Engine/memory_model.md) — docs/03_Execution_Engine/memory_model.md

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/vm-state) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-multiwidth-vm test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_multiwidth_vm.exe) — tests/test_multiwidth_vm.exe
- [tc:test:test-architecture-v2 test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_architecture_v2.cpp) — tests/test_architecture_v2.cpp

The test IDs attached to this lesson are tc:test:test-multiwidth-vm, tc:test:test-architecture-v2. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase also has benchmark records tc:benchmark:test-execution-backends-benchmark; interpret them only with their workload and baseline.

## Next step

[Continue to the next lesson](/learn/eecs/vm-memory-trace). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
