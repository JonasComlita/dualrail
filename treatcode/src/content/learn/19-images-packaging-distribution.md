---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "images-packaging-distribution",
  "title": "Images, packaging, and distribution",
  "module": "Phase 18 · Images, loading, packaging, and distribution",
  "level": "eecs",
  "order": 36,
  "summary": "Build the mental model for Images, loading, packaging, and distribution.",
  "phase_id": "tc:layer:phase-18-images-release",
  "phase_slug": "images-release",
  "phase_name": "Images, loading, packaging, and distribution",
  "lesson_kind": "mental-model",
  "implementation_status": "complete",
  "canonical_terms": [
    "image format",
    "loader",
    "packaging manifest",
    "release channel",
    "compatibility"
  ],
  "objectives": [
    "Explain image formats, loaders, manifests, packaging, release channels, compatibility, and recovery.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "shell-gui-apps"
  ],
  "sources": [
    {
      "path": "IMAGE_FORMAT_MANIFEST.json",
      "label": "IMAGE_FORMAT_MANIFEST.json",
      "kind": "manifest"
    },
    {
      "path": "build_tos_image.cpp",
      "label": "build_tos_image.cpp",
      "kind": "source"
    },
    {
      "path": "tools/trit-inspect-image.ps1",
      "label": "trit-inspect-image.ps1",
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
      "path": "tests/test_migration_v2.cpp",
      "label": "tc:test:test-migration-v2 test source",
      "kind": "test"
    },
    {
      "path": "tests_next/17_full_system/next_full_system_release.cpp",
      "label": "tc:test:next-full-system-release test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-host-runtime",
    "tc:test:test-migration-v2",
    "tc:test:next-full-system-release"
  ],
  "benchmark_ids": [],
  "gap_ids": [],
  "stack_links": {
    "phase": "/stack/images-release",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/IMAGE_FORMAT_MANIFEST.json",
    "tests": "/stack?focus=tc%3Atest%3Atest-host-runtime"
  },
  "next": "release-loading-recovery",
  "interactive": {
    "kind": "choice",
    "title": "Release check",
    "prompt": "Which statement keeps the images-release boundary honest?",
    "options": [
      "The exact source and validation contract",
      "A nearby concept with no evidence",
      "A UI-only state change"
    ],
    "answer": 1,
    "explanation": "The repository boundary is defined by the current source and validation evidence for Images, loading, packaging, and distribution. Planned and unavailable work remains labelled."
  }
}
---

## Objectives

- Explain image formats, loaders, manifests, packaging, release channels, compatibility, and recovery in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [shell-gui-apps](/learn/eecs/shell-gui-apps) first. Those lessons introduce the state and vocabulary that this page assumes. This is the mental-model lesson for Images, loading, packaging, and distribution. Begin with the general systems idea, then compare it with the current Trit boundary before you touch a tool. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

Packaging turns separately built pieces into a distributable promise. An image format defines headers, sections, offsets, sizes, and version compatibility. A loader checks those invariants before executing. A manifest explains what is included, while a release channel says which audience and compatibility policy applies. Recovery matters because a failed update should not leave an ambiguous artifact or half-installed state. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is not only “what does Trit call this?” but “what invariant would another computer system need at the same boundary?”

## Explanation

IMAGE_FORMAT_MANIFEST.json, build_tos_image.cpp, and tools/trit-inspect-image.ps1 define the current image route. Host-runtime, migration, and full-system release tests validate parts of loading and compatibility. The generated image remains distinct from the source manifest and from the public snapshot. The phase enters through compiled artifacts, app metadata, and a release format contract and leaves through a package or image that can be inspected, loaded, and recovered under a stated version policy. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

A loader reads a versioned header, validates section bounds against the file size, checks the declared architecture, and maps code and data into guest regions. The app manifest then supplies guest paths. If a section extends beyond the file, the loader must reject it before writing memory. A migration path may accept an older version only with an explicit conversion record. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

IMAGE_FORMAT_MANIFEST.json, build_tos_image.cpp, and tools/trit-inspect-image.ps1 define the current image route. Host-runtime, migration, and full-system release tests validate parts of loading and compatibility. The generated image remains distinct from the source manifest and from the public snapshot. The current registry row is tc:layer:phase-18-images-release, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Some external assets and recovery scenarios are limited by availability. The course labels a format contract as implemented only where its parser or test evidence exists, not because a file was produced once. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

A loader reads a versioned header, validates section bounds against the file size, checks the declared architecture, and maps code and data into guest regions. The app manifest then supplies guest paths. If a section extends beyond the file, the loader must reject it before writing memory. A migration path may accept an older version only with an explicit conversion record. For this lesson, write the example as a sequence: first identify compiled artifacts, app metadata, and a release format contract; next apply the images-release rule; then inspect a package or image that can be inspected, loaded, and recovered under a stated version policy; finally compare the result with the named validation record. The conceptual check should explain why the result would be wrong if a neighboring representation or layer were substituted. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

A successful build is not a compatible release, and a file extension is not a loader contract. Packaging can preserve invalid metadata unless inspection and load tests reject it. A migration tool is not permission to silently reinterpret every future format. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Design a release manifest table with version, sections, guest paths, checksums, and rollback point. Add one invalid row and explain exactly which validator should catch it. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [IMAGE_FORMAT_MANIFEST.json](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/IMAGE_FORMAT_MANIFEST.json) — IMAGE_FORMAT_MANIFEST.json
- [build_tos_image.cpp](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/build_tos_image.cpp) — build_tos_image.cpp
- [trit-inspect-image.ps1](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tools/trit-inspect-image.ps1) — tools/trit-inspect-image.ps1

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/images-release) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-host-runtime test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_host_runtime.cpp) — tests/test_host_runtime.cpp
- [tc:test:test-migration-v2 test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_migration_v2.cpp) — tests/test_migration_v2.cpp
- [tc:test:next-full-system-release test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/17_full_system/next_full_system_release.cpp) — tests_next/17_full_system/next_full_system_release.cpp

The test IDs attached to this lesson are tc:test:test-host-runtime, tc:test:test-migration-v2, tc:test:next-full-system-release. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase has no benchmark record in the current registry.

## Next step

[Continue to the next lesson](/learn/eecs/release-loading-recovery). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
