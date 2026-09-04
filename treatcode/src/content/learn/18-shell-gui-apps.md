---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "shell-gui-apps",
  "title": "Trace shell, GUI, and app launch",
  "module": "Phase 17 · User space, shell, GUI, and bundled applications",
  "level": "programmer",
  "order": 35,
  "summary": "Trace the repository evidence for User space, shell, GUI, and bundled applications.",
  "phase_id": "tc:layer:phase-17-user-space-apps",
  "phase_slug": "user-space-apps",
  "phase_name": "User space, shell, GUI, and bundled applications",
  "lesson_kind": "build-trace",
  "implementation_status": "complete",
  "canonical_terms": [
    "user space",
    "shell",
    "GUI",
    "application manifest",
    "permission"
  ],
  "objectives": [
    "Explain processes, shell and GUI composition, app SDKs, bundled apps, permissions, and user-facing failure paths.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "applications-and-validation"
  ],
  "sources": [
    {
      "path": "apps/desktop.trit",
      "label": "desktop.trit",
      "kind": "source"
    },
    {
      "path": "apps/shell.trit",
      "label": "shell.trit",
      "kind": "source"
    },
    {
      "path": "APP_MANIFEST.json",
      "label": "APP_MANIFEST.json",
      "kind": "manifest"
    }
  ],
  "evidence": [
    {
      "path": "TEST_MANIFEST.json",
      "label": "Authoritative test manifest",
      "kind": "manifest"
    },
    {
      "path": "tests/test_all_apps.cpp",
      "label": "tc:test:test-all-apps test source",
      "kind": "test"
    },
    {
      "path": "tests/test_native_apps.cpp",
      "label": "tc:test:test-native-apps test source",
      "kind": "test"
    },
    {
      "path": "tests/test_consumer_shell_productization.cpp",
      "label": "tc:test:test-consumer-shell-productization test source",
      "kind": "test"
    },
    {
      "path": "tests_next/12_apps/next_apps_consumer_bundle.cpp",
      "label": "tc:test:next-apps-consumer-bundle test source",
      "kind": "test"
    },
    {
      "path": "tests_next/12_apps/next_apps_desktop_shell.cpp",
      "label": "tc:test:next-apps-desktop-shell test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-all-apps",
    "tc:test:test-native-apps",
    "tc:test:test-consumer-shell-productization",
    "tc:test:next-apps-consumer-bundle",
    "tc:test:next-apps-desktop-shell"
  ],
  "benchmark_ids": [],
  "gap_ids": [
    "tc:gap:trit-gap-app-golden"
  ],
  "stack_links": {
    "phase": "/stack/user-space-apps",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/apps/desktop.trit",
    "tests": "/stack?focus=tc%3Atest%3Atest-all-apps"
  },
  "next": "images-packaging-distribution",
  "interactive": {
    "kind": "code",
    "title": "Run a bounded compiler check",
    "starter": "import ulib;\n\nfn first_value() -> t40 {\n    return 1 + 2;\n}",
    "expectedIncludes": [
      "fn first_value",
      "return 1 + 2"
    ],
    "exercise_id": "shell-gui-apps",
    "runner": "treatcode-learning-compiler",
    "explanation": "Submit the bounded TCL example to the repository-backed learning runner. It may pass or report a real compiler failure; either result is evidence about the User space, shell, GUI, and bundled applications exercise, not a local text-only success."
  }
}
---

## Objectives

- Explain processes, shell and GUI composition, app SDKs, bundled apps, permissions, and user-facing failure paths in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [applications-and-validation](/learn/eecs/applications-and-validation) first. Those lessons introduce the state and vocabulary that this page assumes. This is the build, trace, and evidence lesson for User space, shell, GUI, and bundled applications. The goal is to inspect a real repository transition and report both what the current tests prove and what they do not prove. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

User space is where the stack becomes a product for a person. A shell composes commands and services, a GUI composes events and surfaces, and applications use SDKs rather than reaching through arbitrary kernel state. Permissions and process boundaries determine what a program may do. A polished interface still needs a failure path for missing files, denied services, and partial capabilities. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is whether the repository's files and tests actually carry that invariant through a build or execution step.

## Explanation

apps/desktop.trit, apps/shell.trit, and APP_MANIFEST.json define the bundled application route. App, native-app, consumer-shell, and next desktop tests provide evidence. The app manifest is a contract for image registration and guest paths, not merely a list used to populate a card. The phase enters through runtime services and an application manifest and leaves through a user-facing program whose process, permissions, and failure paths are testable. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

Read the shell and desktop sources with APP_MANIFEST.json. Trace one app from source to SDK call, process creation, image registration, and focused test. Then choose a failure such as a missing guest path and identify where it should be caught. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

apps/desktop.trit, apps/shell.trit, and APP_MANIFEST.json define the bundled application route. App, native-app, consumer-shell, and next desktop tests provide evidence. The app manifest is a contract for image registration and guest paths, not merely a list used to populate a card. The current registry row is tc:layer:phase-17-user-space-apps, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Golden app snapshots and some consumer assets remain gaps. A bundled app can be compiled and registered while its visual or full-device behavior remains explicitly unverified. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

A shell command starts a process through the runtime, the app receives only its permitted handles, and the GUI event loop delivers an input. If an app requests a missing capability, the user-facing result should be a clear error or disabled action. The shell's process handoff is part of the feature because a binary that cannot be launched is not a useful bundle. For this lesson, write the example as a sequence: first identify runtime services and an application manifest; next apply the user-space-apps rule; then inspect a user-facing program whose process, permissions, and failure paths are testable; finally compare the result with the named validation record. The repository investigation should begin at apps/desktop.trit and cross-check the test IDs tc:test:test-all-apps, tc:test:test-native-apps, tc:test:test-consumer-shell-productization, tc:test:next-apps-consumer-bundle, tc:test:next-apps-desktop-shell. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

A GUI label is not an application capability, and a manifest entry is not proof of launch. User space depends on process, permission, runtime, image, and test contracts that can fail independently. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Write a launch trace with five checkpoints: manifest lookup, process creation, permission check, first service call, and user-visible error. Attach exact evidence to each checkpoint. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [desktop.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/apps/desktop.trit) — apps/desktop.trit
- [shell.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/apps/shell.trit) — apps/shell.trit
- [APP_MANIFEST.json](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/APP_MANIFEST.json) — APP_MANIFEST.json

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/user-space-apps) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-all-apps test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_all_apps.cpp) — tests/test_all_apps.cpp
- [tc:test:test-native-apps test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_native_apps.cpp) — tests/test_native_apps.cpp
- [tc:test:test-consumer-shell-productization test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_consumer_shell_productization.cpp) — tests/test_consumer_shell_productization.cpp
- [tc:test:next-apps-consumer-bundle test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/12_apps/next_apps_consumer_bundle.cpp) — tests_next/12_apps/next_apps_consumer_bundle.cpp
- [tc:test:next-apps-desktop-shell test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/12_apps/next_apps_desktop_shell.cpp) — tests_next/12_apps/next_apps_desktop_shell.cpp

The test IDs attached to this lesson are tc:test:test-all-apps, tc:test:test-native-apps, tc:test:test-consumer-shell-productization, tc:test:next-apps-consumer-bundle, tc:test:next-apps-desktop-shell. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase has no benchmark record in the current registry.

## Next step

[Continue to the next lesson](/learn/eecs/images-packaging-distribution). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
