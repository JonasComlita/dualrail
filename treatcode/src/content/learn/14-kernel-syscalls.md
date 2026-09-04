---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "kernel-syscalls",
  "title": "Kernel foundations and syscalls",
  "module": "Phase 13 · Kernel foundations: HAL, memory, MMU/TLB, process, traps, and IPC",
  "level": "programmer",
  "order": 26,
  "summary": "Build the mental model for Kernel foundations: HAL, memory, MMU/TLB, process, traps, and IPC.",
  "phase_id": "tc:layer:phase-13-kernel-foundations",
  "phase_slug": "kernel-foundations",
  "phase_name": "Kernel foundations: HAL, memory, MMU/TLB, process, traps, and IPC",
  "lesson_kind": "mental-model",
  "implementation_status": "complete",
  "canonical_terms": [
    "HAL",
    "address space",
    "MMU",
    "TLB",
    "IPC"
  ],
  "objectives": [
    "Explain hardware abstraction, allocation, address translation, TLBs, process and context state, scheduling, traps, IPC, and isolation.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "first-executable-boot"
  ],
  "sources": [
    {
      "path": "kernel/hal.trit",
      "label": "hal.trit",
      "kind": "source"
    },
    {
      "path": "kernel/process.trit",
      "label": "process.trit",
      "kind": "source"
    },
    {
      "path": "kernel.trit",
      "label": "kernel.trit",
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
      "path": "tests/test_layer1_hal.cpp",
      "label": "tc:test:test-layer1-hal test source",
      "kind": "test"
    },
    {
      "path": "tests/test_kernel.cpp",
      "label": "tc:test:test-kernel test source",
      "kind": "test"
    },
    {
      "path": "tests/test_os_platform.cpp",
      "label": "tc:test:test-os-platform test source",
      "kind": "test"
    },
    {
      "path": "tests/test_phase_d_kernel.cpp",
      "label": "tc:test:test-phase-d-kernel test source",
      "kind": "test"
    },
    {
      "path": "tests/test_process_handoff.cpp",
      "label": "tc:test:test-process-handoff test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-layer1-hal",
    "tc:test:test-kernel",
    "tc:test:test-os-platform",
    "tc:test:test-phase-d-kernel",
    "tc:test:test-process-handoff"
  ],
  "benchmark_ids": [
    "tc:benchmark:test-scaling-profile-compact"
  ],
  "gap_ids": [
    "tc:gap:trit-gap-fuzz-harnesses",
    "tc:gap:trit-gap-crash-recovery"
  ],
  "stack_links": {
    "phase": "/stack/kernel-foundations",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/kernel/hal.trit",
    "tests": "/stack?focus=tc%3Atest%3Atest-layer1-hal"
  },
  "next": "kernel-memory-process",
  "interactive": {
    "kind": "choice",
    "title": "Concept check",
    "prompt": "Which statement keeps the kernel-foundations boundary honest?",
    "options": [
      "The exact source and validation contract",
      "A nearby concept with no evidence",
      "A UI-only state change"
    ],
    "answer": 1,
    "explanation": "The repository boundary is defined by the current source and validation evidence for Kernel foundations: HAL, memory, MMU/TLB, process, traps, and IPC. Planned and unavailable work remains labelled."
  }
}
---

## Objectives

- Explain hardware abstraction, allocation, address translation, TLBs, process and context state, scheduling, traps, IPC, and isolation in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [first-executable-boot](/learn/eecs/first-executable-boot) first. Those lessons introduce the state and vocabulary that this page assumes. This is the mental-model lesson for Kernel foundations: HAL, memory, MMU/TLB, process, traps, and IPC. Begin with the general systems idea, then compare it with the current Trit boundary before you touch a tool. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

A kernel turns shared hardware into controlled abstractions. The HAL gives higher layers stable operations, memory management names ownership and address spaces, the MMU/TLB translates accesses, process state records context, scheduling chooses who runs, traps regain control, and IPC moves data under an isolation rule. These are separate mechanisms that meet at carefully defined boundaries. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is not only “what does Trit call this?” but “what invariant would another computer system need at the same boundary?”

## Explanation

kernel/hal.trit, kernel/process.trit, and kernel.trit define the current foundation. Layer-one, kernel, OS-platform, phase-D, process-handoff, and scaling tests provide evidence. The public registry also records crash-recovery and fuzz-harness gaps, which are not hidden by the presence of basic process code. The phase enters through a booted machine state and a request from a process and leaves through an isolated kernel transition with ownership, scheduling, and fault evidence. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

A user load first names a virtual address. The MMU checks the page mapping or TLB entry, the kernel enforces permissions, and the process resumes or takes a page fault. A context switch saves the old PC and registers, selects another runnable process, and restores its state. IPC then copies or shares data under an explicit capability rule rather than exposing the entire address space. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

kernel/hal.trit, kernel/process.trit, and kernel.trit define the current foundation. Layer-one, kernel, OS-platform, phase-D, process-handoff, and scaling tests provide evidence. The public registry also records crash-recovery and fuzz-harness gaps, which are not hidden by the presence of basic process code. The current registry row is tc:layer:phase-13-kernel-foundations, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Crash recovery, fuzz harnesses, and some hardware isolation claims remain incomplete. The lesson treats the current allocator, process, and trap tests as implemented slices and labels the broader production guarantee as planned where appropriate. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

A user load first names a virtual address. The MMU checks the page mapping or TLB entry, the kernel enforces permissions, and the process resumes or takes a page fault. A context switch saves the old PC and registers, selects another runnable process, and restores its state. IPC then copies or shares data under an explicit capability rule rather than exposing the entire address space. For this lesson, write the example as a sequence: first identify a booted machine state and a request from a process; next apply the kernel-foundations rule; then inspect an isolated kernel transition with ownership, scheduling, and fault evidence; finally compare the result with the named validation record. The conceptual check should explain why the result would be wrong if a neighboring representation or layer were substituted. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

A process is not just a function call and a TLB is not the page table. The TLB caches translations; the MMU and kernel policy decide whether an access is permitted. Switching stacks without switching address-space or privilege state would not provide isolation. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Trace one page fault through hardware abstraction, translation, trap, scheduler, and process resumption. Write the invariant that prevents a faulting process from silently reading another process's page. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [hal.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/kernel/hal.trit) — kernel/hal.trit
- [process.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/kernel/process.trit) — kernel/process.trit
- [kernel.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/kernel.trit) — kernel.trit

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/kernel-foundations) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-layer1-hal test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_layer1_hal.cpp) — tests/test_layer1_hal.cpp
- [tc:test:test-kernel test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_kernel.cpp) — tests/test_kernel.cpp
- [tc:test:test-os-platform test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_os_platform.cpp) — tests/test_os_platform.cpp
- [tc:test:test-phase-d-kernel test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_phase_d_kernel.cpp) — tests/test_phase_d_kernel.cpp
- [tc:test:test-process-handoff test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_process_handoff.cpp) — tests/test_process_handoff.cpp

The test IDs attached to this lesson are tc:test:test-layer1-hal, tc:test:test-kernel, tc:test:test-os-platform, tc:test:test-phase-d-kernel, tc:test:test-process-handoff. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase also has benchmark records tc:benchmark:test-scaling-profile-compact; interpret them only with their workload and baseline.

## Next step

[Continue to the next lesson](/learn/eecs/kernel-memory-process). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
