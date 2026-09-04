---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "first-executable-boot",
  "title": "Diagnose the first executable",
  "module": "Phase 12 · Reset, boot, trap entry, and first executable programs",
  "level": "programmer",
  "order": 25,
  "summary": "Trace the repository evidence for Reset, boot, trap entry, and first executable programs.",
  "phase_id": "tc:layer:phase-12-boot-trap",
  "phase_slug": "boot-trap",
  "phase_name": "Reset, boot, trap entry, and first executable programs",
  "lesson_kind": "build-trace",
  "implementation_status": "complete",
  "canonical_terms": [
    "reset state",
    "bootloader",
    "trap entry",
    "image header",
    "first executable"
  ],
  "objectives": [
    "Explain reset state, image loading, boot stages, trap entry, first user program, and failure diagnosis.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "boot-and-traps"
  ],
  "sources": [
    {
      "path": "bootloader.tasm",
      "label": "bootloader.tasm",
      "kind": "source"
    },
    {
      "path": "native_kernel_boot.tasm",
      "label": "native_kernel_boot.tasm",
      "kind": "source"
    },
    {
      "path": "native_kernel_trap_stub.tasm",
      "label": "native_kernel_trap_stub.tasm",
      "kind": "source"
    },
    {
      "path": "v2_first_silicon_bringup.tasm",
      "label": "v2_first_silicon_bringup.tasm",
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
      "path": "tests/test_architecture_v2.cpp",
      "label": "tc:test:test-architecture-v2 test source",
      "kind": "test"
    },
    {
      "path": "tests/test_first_silicon_bringup.cpp",
      "label": "tc:test:test-first-silicon-bringup test source",
      "kind": "test"
    },
    {
      "path": "tests/test_os_platform.cpp",
      "label": "tc:test:test-os-platform test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-architecture-v2",
    "tc:test:test-first-silicon-bringup",
    "tc:test:test-os-platform"
  ],
  "benchmark_ids": [],
  "gap_ids": [],
  "stack_links": {
    "phase": "/stack/boot-trap",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/bootloader.tasm",
    "tests": "/stack?focus=tc%3Atest%3Atest-architecture-v2"
  },
  "next": "kernel-syscalls",
  "interactive": {
    "kind": "source",
    "title": "Source investigation",
    "prompt": "Find the exact implementation boundary for Reset, boot, trap entry, and first executable programs.",
    "sourcePath": "bootloader.tasm",
    "sourceHref": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/bootloader.tasm",
    "expectedIncludes": [
      "bootloader.tasm"
    ],
    "explanation": "Open the source and compare it with the tc:test:test-architecture-v2 validation record. This exercise reports the repository boundary instead of pretending that a link is a lesson."
  }
}
---

## Objectives

- Explain reset state, image loading, boot stages, trap entry, first user program, and failure diagnosis in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [boot-and-traps](/learn/eecs/boot-and-traps) first. Those lessons introduce the state and vocabulary that this page assumes. This is the build, trace, and evidence lesson for Reset, boot, trap entry, and first executable programs. The goal is to inspect a real repository transition and report both what the current tests prove and what they do not prove. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

Boot is a sequence of ownership transfers. Reset establishes a small architectural state, a loader finds and verifies an image, assembly establishes runtime and trap entry, and the kernel eventually admits the first executable. Each handoff narrows the unknowns. A boot failure is easier to diagnose when the evidence says whether reset, image parsing, instruction decode, trap entry, or process launch failed. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is whether the repository's files and tests actually carry that invariant through a build or execution step.

## Explanation

bootloader.tasm, native_kernel_boot.tasm, native_kernel_trap_stub.tasm, and the first-silicon bring-up fixture cover the current boot boundary. Architecture, bring-up, and OS tests provide evidence. The image format manifest is read with the loader so a valid file and a valid boot sequence are not conflated. The phase enters through reset state and a bootable image and leaves through a first executable with a diagnosable path from reset to service call. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

Read the bootloader and trap stub together, then open the bring-up and OS tests. Build a timeline with reset, image check, load, trap vector, kernel entry, process creation, and first instruction. For one negative test, record the evidence that distinguishes a malformed image from a malformed instruction. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

bootloader.tasm, native_kernel_boot.tasm, native_kernel_trap_stub.tasm, and the first-silicon bring-up fixture cover the current boot boundary. Architecture, bring-up, and OS tests provide evidence. The image format manifest is read with the loader so a valid file and a valid boot sequence are not conflated. The current registry row is tc:layer:phase-12-boot-trap, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Some hardware reset and physical device behavior remains simulated or host-provided. The route labels those assumptions and teaches a failure taxonomy instead of promising a universal board bring-up. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

At reset, the PC and privilege state are known. The loader reads a header, checks the format version and section bounds, and places code at the expected guest address. The boot stub installs a trap target, then jumps to kernel initialization. If the first user program faults, the correct question is whether the fault happened before process state, during a syscall, or after a user instruction. For this lesson, write the example as a sequence: first identify reset state and a bootable image; next apply the boot-trap rule; then inspect a first executable with a diagnosable path from reset to service call; finally compare the result with the named validation record. The repository investigation should begin at bootloader.tasm and cross-check the test IDs tc:test:test-architecture-v2, tc:test:test-first-silicon-bringup, tc:test:test-os-platform. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

Boot is not just copying bytes and jumping to an address. Privilege, image version, memory layout, and trap entry all participate. A file that has the right extension but the wrong header is not bootable evidence. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Design a boot checklist with a stop condition and artifact for each stage. Use it to classify three hypothetical failures and identify which source and test record a learner should open next. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [bootloader.tasm](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/bootloader.tasm) — bootloader.tasm
- [native_kernel_boot.tasm](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/native_kernel_boot.tasm) — native_kernel_boot.tasm
- [native_kernel_trap_stub.tasm](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/native_kernel_trap_stub.tasm) — native_kernel_trap_stub.tasm
- [v2_first_silicon_bringup.tasm](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/v2_first_silicon_bringup.tasm) — v2_first_silicon_bringup.tasm

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/boot-trap) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-architecture-v2 test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_architecture_v2.cpp) — tests/test_architecture_v2.cpp
- [tc:test:test-first-silicon-bringup test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_first_silicon_bringup.cpp) — tests/test_first_silicon_bringup.cpp
- [tc:test:test-os-platform test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_os_platform.cpp) — tests/test_os_platform.cpp

The test IDs attached to this lesson are tc:test:test-architecture-v2, tc:test:test-first-silicon-bringup, tc:test:test-os-platform. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase has no benchmark record in the current registry.

## Next step

[Continue to the next lesson](/learn/eecs/kernel-syscalls). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
