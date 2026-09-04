---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "isa-trap-abi-trace",
  "title": "Decode, traps, and ABI evidence",
  "module": "Phase 02 · ISA, registers, privilege, CSRs, traps, and ABI",
  "level": "beginner",
  "order": 5,
  "summary": "Trace the repository evidence for ISA, registers, privilege, CSRs, traps, and ABI.",
  "phase_id": "tc:layer:phase-02-isa-binary",
  "phase_slug": "isa-binary",
  "phase_name": "ISA, registers, privilege, CSRs, traps, and ABI",
  "lesson_kind": "build-trace",
  "implementation_status": "complete",
  "canonical_terms": [
    "opcode",
    "register file",
    "privilege",
    "CSR",
    "ABI"
  ],
  "objectives": [
    "Explain instruction shape, opcodes, register state, privilege, CSRs, trap causes and entry, calling convention, and ABI boundaries.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "isa-vm-execution"
  ],
  "sources": [
    {
      "path": "ternary_isa.h",
      "label": "ternary_isa.h",
      "kind": "source"
    },
    {
      "path": "ARCHITECTURE_MANIFEST.json",
      "label": "ARCHITECTURE_MANIFEST.json",
      "kind": "manifest"
    },
    {
      "path": "docs/02_Hardware_ISA/encoding.md",
      "label": "encoding.md",
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
      "path": "tests/test_architecture_v2.cpp",
      "label": "tc:test:test-architecture-v2 test source",
      "kind": "test"
    },
    {
      "path": "tests/test_ternary_ir.cpp",
      "label": "tc:test:test-ternary-ir test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-architecture-v2",
    "tc:test:test-ternary-ir"
  ],
  "benchmark_ids": [],
  "gap_ids": [],
  "stack_links": {
    "phase": "/stack/isa-binary",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_isa.h",
    "tests": "/stack?focus=tc%3Atest%3Atest-architecture-v2"
  },
  "next": "tritwise-operations",
  "interactive": {
    "kind": "source",
    "title": "Source investigation",
    "prompt": "Find the exact implementation boundary for ISA, registers, privilege, CSRs, traps, and ABI.",
    "sourcePath": "ternary_isa.h",
    "sourceHref": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_isa.h",
    "expectedIncludes": [
      "ternary_isa.h"
    ],
    "explanation": "Open the source and compare it with the tc:test:test-architecture-v2 validation record. This exercise reports the repository boundary instead of pretending that a link is a lesson."
  }
}
---

## Objectives

- Explain instruction shape, opcodes, register state, privilege, CSRs, trap causes and entry, calling convention, and ABI boundaries in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [isa-vm-execution](/learn/eecs/isa-vm-execution) first. Those lessons introduce the state and vocabulary that this page assumes. This is the build, trace, and evidence lesson for ISA, registers, privilege, CSRs, traps, and ABI. The goal is to inspect a real repository transition and report both what the current tests prove and what they do not prove. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

An instruction set architecture is the boundary between software expectations and machine state. It defines not only arithmetic opcodes but also registers, program-counter movement, privilege, control/status registers, traps, and the calling convention used at an ABI boundary. Encoding is a separate concern: a decoder must know how bits or trits become fields before an executor can interpret those fields. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is whether the repository's files and tests actually carry that invariant through a build or execution step.

## Explanation

The Trit ISA and architecture manifests describe the instruction word and ABI. The VM and compiler share register and CSR names, and tests exercise encoding, decode, privilege, and trap behavior. Syscall arguments and returns are documented as register and CSR conventions; wrappers are useful because they preserve those conventions for user code. The phase enters through an encoded instruction and the architectural state before execution and leaves through a decoded operation whose effects and privilege checks are explicit. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

Compare ternary_isa.h, ARCHITECTURE_MANIFEST.json, and the encoding documentation. Pick one opcode, write down its fields, then locate the test that rejects an invalid field. Follow the trap cause into the VM or kernel entry point and note where privilege changes are checked. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

The Trit ISA and architecture manifests describe the instruction word and ABI. The VM and compiler share register and CSR names, and tests exercise encoding, decode, privilege, and trap behavior. Syscall arguments and returns are documented as register and CSR conventions; wrappers are useful because they preserve those conventions for user code. The current registry row is tc:layer:phase-02-isa-binary, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

A complete hardware privilege implementation may be broader than the current host model. The lesson therefore separates architectural rules from the exact runtime surface that is currently tested. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

For a call that returns one value, imagine the caller placing an argument in r13, setting the call target, and observing the return in r13. If the same numeric payload is placed in a CSR field, that is not automatically an argument: the field's role comes from the ABI. A malformed opcode should produce a trap record and a defined PC or cause update rather than accidentally executing a nearby instruction. For this lesson, write the example as a sequence: first identify an encoded instruction and the architectural state before execution; next apply the isa-binary rule; then inspect a decoded operation whose effects and privilege checks are explicit; finally compare the result with the named validation record. The repository investigation should begin at ternary_isa.h and cross-check the test IDs tc:test:test-architecture-v2, tc:test:test-ternary-ir. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

An ABI is not just a function signature, and a register name is not self-explanatory. The ABI includes where arguments, returns, service IDs, and failure details live. A bitwise copy that preserves a payload can still violate the ABI if it crosses the wrong field. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Draw the before/decode/after state for one instruction: PC, opcode, source registers, destination register, and trap cause. Repeat it for an illegal encoding. Explain which parts are ISA guarantees and which parts are implementation evidence. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [ternary_isa.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_isa.h) — ternary_isa.h
- [ARCHITECTURE_MANIFEST.json](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ARCHITECTURE_MANIFEST.json) — ARCHITECTURE_MANIFEST.json
- [encoding.md](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/docs/02_Hardware_ISA/encoding.md) — docs/02_Hardware_ISA/encoding.md

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/isa-binary) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-architecture-v2 test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_architecture_v2.cpp) — tests/test_architecture_v2.cpp
- [tc:test:test-ternary-ir test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_ternary_ir.cpp) — tests/test_ternary_ir.cpp

The test IDs attached to this lesson are tc:test:test-architecture-v2, tc:test:test-ternary-ir. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase has no benchmark record in the current registry.

## Next step

[Continue to the next lesson](/learn/eecs/tritwise-operations). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
