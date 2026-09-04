---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "assembler-binary-contract",
  "title": "Assembler and binary contracts",
  "module": "Phase 06 · Assembler, disassembler, relocations, and binary contract",
  "level": "beginner",
  "order": 12,
  "summary": "Build the mental model for Assembler, disassembler, relocations, and binary contract.",
  "phase_id": "tc:layer:phase-06-assembler",
  "phase_slug": "assembler",
  "phase_name": "Assembler, disassembler, relocations, and binary contract",
  "lesson_kind": "mental-model",
  "implementation_status": "complete",
  "canonical_terms": [
    "assembler",
    "disassembler",
    "relocation",
    "symbol",
    "binary contract"
  ],
  "objectives": [
    "Explain assembly syntax, encoding and decoding, symbols, relocations, object boundaries, and binary compatibility.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "vm-jit-benchmark"
  ],
  "sources": [
    {
      "path": "ternary_asm.h",
      "label": "ternary_asm.h",
      "kind": "source"
    },
    {
      "path": "docs/04_Binary_Contract/asm_syntax.md",
      "label": "asm_syntax.md",
      "kind": "documentation"
    },
    {
      "path": "tests/test_tcl_asm.cpp",
      "label": "test_tcl_asm.cpp",
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
      "path": "tests/test_tcl_asm.cpp",
      "label": "tc:test:test-tcl-asm test source",
      "kind": "test"
    },
    {
      "path": "tests_next/03_assembler/next_assembler_goldens.cpp",
      "label": "tc:test:next-assembler-goldens test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-tcl-asm",
    "tc:test:next-assembler-goldens"
  ],
  "benchmark_ids": [],
  "gap_ids": [
    "tc:gap:trit-gap-tascii81"
  ],
  "stack_links": {
    "phase": "/stack/assembler",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_asm.h",
    "tests": "/stack?focus=tc%3Atest%3Atest-tcl-asm"
  },
  "next": "assembler-relocation-lab",
  "interactive": {
    "kind": "choice",
    "title": "Concept check",
    "prompt": "Which statement keeps the assembler boundary honest?",
    "options": [
      "The exact source and validation contract",
      "A nearby concept with no evidence",
      "A UI-only state change"
    ],
    "answer": 1,
    "explanation": "The repository boundary is defined by the current source and validation evidence for Assembler, disassembler, relocations, and binary contract. Planned and unavailable work remains labelled."
  }
}
---

## Objectives

- Explain assembly syntax, encoding and decoding, symbols, relocations, object boundaries, and binary compatibility in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [vm-jit-benchmark](/learn/eecs/vm-jit-benchmark) first. Those lessons introduce the state and vocabulary that this page assumes. This is the mental-model lesson for Assembler, disassembler, relocations, and binary contract. Begin with the general systems idea, then compare it with the current Trit boundary before you touch a tool. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

An assembler is a boundary translator, not a spell checker. It maps names and syntax to fields, emits bytes or trits, and records unresolved addresses so a linker can finish the job. A disassembler provides the inverse view, but inverse does not mean lossless: comments, aliases, and symbol names may not survive. Binary compatibility depends on the contract for widths, endianness or packing, relocation records, and error reporting. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is not only “what does Trit call this?” but “what invariant would another computer system need at the same boundary?”

## Explanation

ternary_asm.h, the assembly syntax documentation, and the TCL assembly tests define the current parser and encoder boundary. The next assembler goldens are explicitly referenced work, so a learner can inspect the planned target without calling it shipped. TASCII-81 and symbolic encoding gaps stay separate from core instruction encoding. The phase enters through textual assembly or an object fragment with symbols and leaves through a binary artifact whose encoding, relocation, and compatibility rules are explicit. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

An instruction that calls symbol start cannot know the final address until sections are placed. The assembler emits an opcode plus a relocation entry naming start and the field to patch. The linker resolves start or reports an undefined symbol. If a disassembler prints the patched numeric address without the symbol table, the binary is still executable but the source-level explanation is poorer. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

ternary_asm.h, the assembly syntax documentation, and the TCL assembly tests define the current parser and encoder boundary. The next assembler goldens are explicitly referenced work, so a learner can inspect the planned target without calling it shipped. TASCII-81 and symbolic encoding gaps stay separate from core instruction encoding. The current registry row is tc:layer:phase-06-assembler, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Some symbolic encodings and relocation ergonomics are still represented as next tests or gaps. The current assembler contract is narrower than a complete general-purpose object format. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

An instruction that calls symbol start cannot know the final address until sections are placed. The assembler emits an opcode plus a relocation entry naming start and the field to patch. The linker resolves start or reports an undefined symbol. If a disassembler prints the patched numeric address without the symbol table, the binary is still executable but the source-level explanation is poorer. For this lesson, write the example as a sequence: first identify textual assembly or an object fragment with symbols; next apply the assembler rule; then inspect a binary artifact whose encoding, relocation, and compatibility rules are explicit; finally compare the result with the named validation record. The conceptual check should explain why the result would be wrong if a neighboring representation or layer were substituted. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

Assembly text is not the binary contract, and a successful parse does not prove linkability. A symbol may be syntactically valid but unresolved. Conversely, a binary may decode while violating the version or width contract that another loader expects. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Create a tiny object map with two sections, one symbol, and one relocation. Predict the unresolved error before linking and identify the source and test record that should witness it. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [ternary_asm.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_asm.h) — ternary_asm.h
- [asm_syntax.md](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/docs/04_Binary_Contract/asm_syntax.md) — docs/04_Binary_Contract/asm_syntax.md
- [test_tcl_asm.cpp](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_tcl_asm.cpp) — tests/test_tcl_asm.cpp

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/assembler) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-tcl-asm test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_tcl_asm.cpp) — tests/test_tcl_asm.cpp
- [tc:test:next-assembler-goldens test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/03_assembler/next_assembler_goldens.cpp) — tests_next/03_assembler/next_assembler_goldens.cpp

The test IDs attached to this lesson are tc:test:test-tcl-asm, tc:test:next-assembler-goldens. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase has no benchmark record in the current registry.

## Next step

[Continue to the next lesson](/learn/eecs/assembler-relocation-lab). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
