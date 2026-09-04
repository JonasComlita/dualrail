---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "devices-services",
  "title": "Devices and services",
  "module": "Phase 15 · Kernel networking, graphics, devices, and services",
  "level": "programmer",
  "order": 30,
  "summary": "Build the mental model for Kernel networking, graphics, devices, and services.",
  "phase_id": "tc:layer:phase-15-devices-services",
  "phase_slug": "devices-services",
  "phase_name": "Kernel networking, graphics, devices, and services",
  "lesson_kind": "mental-model",
  "implementation_status": "partial",
  "canonical_terms": [
    "driver",
    "interrupt",
    "DMA",
    "network service",
    "device lifecycle"
  ],
  "objectives": [
    "Explain device discovery and lifecycle, drivers, interrupts and DMA boundaries, networking, graphics, services, and current gaps.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "storage-recovery-vfs"
  ],
  "sources": [
    {
      "path": "kernel/net.trit",
      "label": "net.trit",
      "kind": "source"
    },
    {
      "path": "apps/service_stub.trit",
      "label": "service_stub.trit",
      "kind": "source"
    },
    {
      "path": "ternary_os.h",
      "label": "ternary_os.h",
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
      "path": "tests/test_os_platform.cpp",
      "label": "tc:test:test-os-platform test source",
      "kind": "test"
    },
    {
      "path": "tests_next/10_drivers_hal/next_drivers_hal.cpp",
      "label": "tc:test:next-drivers-hal test source",
      "kind": "test"
    },
    {
      "path": "tests_next/11_graphics_gui/next_graphics_gui.cpp",
      "label": "tc:test:next-graphics-gui test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-os-platform",
    "tc:test:next-drivers-hal",
    "tc:test:next-graphics-gui"
  ],
  "benchmark_ids": [],
  "gap_ids": [
    "tc:gap:trit-gap-syscall-tracing"
  ],
  "stack_links": {
    "phase": "/stack/devices-services",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/kernel/net.trit",
    "tests": "/stack?focus=tc%3Atest%3Atest-os-platform"
  },
  "next": "network-graphics-boundary",
  "interactive": {
    "kind": "choice",
    "title": "Concept check",
    "prompt": "Which statement keeps the devices-services boundary honest?",
    "options": [
      "The exact source and validation contract",
      "A nearby concept with no evidence",
      "A UI-only state change"
    ],
    "answer": 1,
    "explanation": "The repository boundary is defined by the current source and validation evidence for Kernel networking, graphics, devices, and services. Planned and unavailable work remains labelled."
  }
}
---

## Objectives

- Explain device discovery and lifecycle, drivers, interrupts and DMA boundaries, networking, graphics, services, and current gaps in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [storage-recovery-vfs](/learn/eecs/storage-recovery-vfs) first. Those lessons introduce the state and vocabulary that this page assumes. This is the mental-model lesson for Kernel networking, graphics, devices, and services. Begin with the general systems idea, then compare it with the current Trit boundary before you touch a tool. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

Devices are asynchronous, stateful participants. Discovery names what exists, initialization establishes ownership and capabilities, drivers translate requests, interrupts report events, and DMA crosses a memory boundary that needs permission and lifetime rules. Networking, graphics, and services build on those primitives but have different timing and failure behavior. A useful device lesson explains the unavailable case as carefully as the happy path. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is not only “what does Trit call this?” but “what invariant would another computer system need at the same boundary?”

## Explanation

kernel/net.trit, apps/service_stub.trit, and ternary_os.h show the current networking and service-facing boundary. OS-platform, drivers/HAL, and graphics/GUI targets provide evidence or explicit future work. Syscall tracing is recorded as a gap, so service observability remains distinct from service existence. The phase enters through a device or service request at the kernel boundary and leaves through a lifecycle and failure story that does not overclaim hardware support. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

A network send enters through an API, becomes a kernel request, and is queued for a driver. The driver owns a descriptor and may receive an interrupt when the device completes. If DMA writes into a buffer after the caller releases it, the bug is a lifetime violation even if the packet contents look correct. A service stub can demonstrate the message shape without proving hardware delivery. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

kernel/net.trit, apps/service_stub.trit, and ternary_os.h show the current networking and service-facing boundary. OS-platform, drivers/HAL, and graphics/GUI targets provide evidence or explicit future work. Syscall tracing is recorded as a gap, so service observability remains distinct from service existence. The current registry row is tc:layer:phase-15-devices-services, with implementation coverage marked partial and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Physical drivers, full graphics integration, and syscall tracing may be planned or referenced rather than shipped. The public registry carries these labels to the lesson so a stub cannot be mistaken for a production device. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

A network send enters through an API, becomes a kernel request, and is queued for a driver. The driver owns a descriptor and may receive an interrupt when the device completes. If DMA writes into a buffer after the caller releases it, the bug is a lifetime violation even if the packet contents look correct. A service stub can demonstrate the message shape without proving hardware delivery. For this lesson, write the example as a sequence: first identify a device or service request at the kernel boundary; next apply the devices-services rule; then inspect a lifecycle and failure story that does not overclaim hardware support; finally compare the result with the named validation record. The conceptual check should explain why the result would be wrong if a neighboring representation or layer were substituted. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

An API that accepts a device request is not proof that a physical driver exists. Interrupts are not polling, DMA is not a free memory copy, and a graphics surface without frame evidence is not a display guarantee. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Create a device lifecycle state diagram from absent to discovered, initialized, busy, completed, and failed. Attach one source, one test, and one gap or capability to every nontrivial transition. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [net.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/kernel/net.trit) — kernel/net.trit
- [service_stub.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/apps/service_stub.trit) — apps/service_stub.trit
- [ternary_os.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_os.h) — ternary_os.h

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/devices-services) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-os-platform test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_os_platform.cpp) — tests/test_os_platform.cpp
- [tc:test:next-drivers-hal test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/10_drivers_hal/next_drivers_hal.cpp) — tests_next/10_drivers_hal/next_drivers_hal.cpp
- [tc:test:next-graphics-gui test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/11_graphics_gui/next_graphics_gui.cpp) — tests_next/11_graphics_gui/next_graphics_gui.cpp

The test IDs attached to this lesson are tc:test:test-os-platform, tc:test:next-drivers-hal, tc:test:next-graphics-gui. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase has no benchmark record in the current registry.

## Next step

[Continue to the next lesson](/learn/eecs/network-graphics-boundary). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
