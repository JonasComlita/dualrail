---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "runtime-streams-errors",
  "title": "Trace a stream and error path",
  "module": "Phase 11 · User runtime, libraries, streams, and SDK ABI",
  "level": "programmer",
  "order": 23,
  "summary": "Trace the repository evidence for User runtime, libraries, streams, and SDK ABI.",
  "phase_id": "tc:layer:phase-11-runtime-sdk",
  "phase_slug": "runtime-sdk",
  "phase_name": "User runtime, libraries, streams, and SDK ABI",
  "lesson_kind": "build-trace",
  "implementation_status": "complete",
  "canonical_terms": [
    "runtime",
    "library",
    "stream",
    "handle",
    "SDK ABI"
  ],
  "objectives": [
    "Explain runtime services, libraries, streams, handles, SDK wrappers, ABI stability, and error propagation.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "runtime-sdk-abi"
  ],
  "sources": [
    {
      "path": "ulib.trit",
      "label": "ulib.trit",
      "kind": "source"
    },
    {
      "path": "apps/os_sdk.trit",
      "label": "os_sdk.trit",
      "kind": "source"
    },
    {
      "path": "apps/libwidget.trit",
      "label": "libwidget.trit",
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
      "path": "tests/test_native_apps.cpp",
      "label": "tc:test:test-native-apps test source",
      "kind": "test"
    },
    {
      "path": "tests_next/06_runtime_ulib/next_runtime_ulib_sdk.cpp",
      "label": "tc:test:next-runtime-ulib-sdk test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-native-apps",
    "tc:test:next-runtime-ulib-sdk"
  ],
  "benchmark_ids": [],
  "gap_ids": [],
  "stack_links": {
    "phase": "/stack/runtime-sdk",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ulib.trit",
    "tests": "/stack?focus=tc%3Atest%3Atest-native-apps"
  },
  "next": "boot-and-traps",
  "interactive": {
    "kind": "code",
    "title": "Run a bounded compiler check",
    "starter": "import ulib;\n\nfn first_value() -> t40 {\n    return 1 + 2;\n}",
    "expectedIncludes": [
      "fn first_value",
      "return 1 + 2"
    ],
    "exercise_id": "runtime-streams-errors",
    "runner": "treatcode-learning-compiler",
    "explanation": "Submit the bounded TCL example to the repository-backed learning runner. It may pass or report a real compiler failure; either result is evidence about the User runtime, libraries, streams, and SDK ABI exercise, not a local text-only success."
  }
}
---

## Objectives

- Explain runtime services, libraries, streams, handles, SDK wrappers, ABI stability, and error propagation in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [runtime-sdk-abi](/learn/eecs/runtime-sdk-abi) first. Those lessons introduce the state and vocabulary that this page assumes. This is the build, trace, and evidence lesson for User runtime, libraries, streams, and SDK ABI. The goal is to inspect a real repository transition and report both what the current tests prove and what they do not prove. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

The runtime is the first layer that makes an operating environment feel useful. It turns low-level calls into conventions for streams, memory, handles, errors, and libraries. An SDK is not decoration: it is an ABI-preserving boundary that lets application code use services without manually rebuilding register and CSR conventions. The boundary is successful only when success and failure are both representable. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is whether the repository's files and tests actually carry that invariant through a build or execution step.

## Explanation

ulib.trit, apps/os_sdk.trit, and apps/libwidget.trit implement the current library and SDK surfaces. Native app and runtime tests exercise wrappers, while the syscall manifest remains the ABI authority. The lesson traces a call from a user wrapper to a service ID and back through status, payload, and detail values. The phase enters through a compiled program that needs services beyond arithmetic and leaves through a library or SDK call whose handle, ABI, and failure path are visible. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

Read the SDK wrapper beside SYSCALL_MANIFEST.json and the focused OS tests. Record the service ID, argument registers, return registers, and error propagation. Follow one widget or stream helper into its underlying runtime call and mark the layer where the contract changes. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

ulib.trit, apps/os_sdk.trit, and apps/libwidget.trit implement the current library and SDK surfaces. Native app and runtime tests exercise wrappers, while the syscall manifest remains the ABI authority. The lesson traces a call from a user wrapper to a service ID and back through status, payload, and detail values. The current registry row is tc:layer:phase-11-runtime-sdk, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Some higher-level services and richer tracing are still gaps. A wrapper can be implemented while a backing device or persistence feature remains planned; the status label must follow the evidence. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

A file-open wrapper may load a service ID into CSR syscall_id, pass a path handle or pointer in an argument register, and return a status plus handle. If the path is invalid, the status must distinguish failure from a valid handle. A caller that ignores the status can turn a service error into an unrelated memory fault. For this lesson, write the example as a sequence: first identify a compiled program that needs services beyond arithmetic; next apply the runtime-sdk rule; then inspect a library or SDK call whose handle, ABI, and failure path are visible; finally compare the result with the named validation record. The repository investigation should begin at ulib.trit and cross-check the test IDs tc:test:test-native-apps, tc:test:next-runtime-ulib-sdk. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

A library function is not automatically a kernel syscall, and a handle is not a raw pointer. The SDK may validate, translate, or preserve ownership before crossing the kernel boundary. Returning a zero-like value is not sufficient if the ABI requires a separate status. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Create an ABI table for one successful and one failing runtime call. Include input ownership, service ID, registers, output handle, and the exact test that validates the wrapper. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [ulib.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ulib.trit) — ulib.trit
- [os_sdk.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/apps/os_sdk.trit) — apps/os_sdk.trit
- [libwidget.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/apps/libwidget.trit) — apps/libwidget.trit

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/runtime-sdk) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-native-apps test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_native_apps.cpp) — tests/test_native_apps.cpp
- [tc:test:next-runtime-ulib-sdk test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/06_runtime_ulib/next_runtime_ulib_sdk.cpp) — tests_next/06_runtime_ulib/next_runtime_ulib_sdk.cpp

The test IDs attached to this lesson are tc:test:test-native-apps, tc:test:next-runtime-ulib-sdk. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase has no benchmark record in the current registry.

## Next step

[Continue to the next lesson](/learn/eecs/boot-and-traps). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
