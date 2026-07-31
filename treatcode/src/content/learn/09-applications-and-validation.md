---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "applications-and-validation",
  "title": "Applications and validation",
  "module": "9. User layer",
  "level": "eecs",
  "order": 90,
  "summary": "Finish the path at bundled apps, image registration, process handoff, and repeatable release checks.",
  "prerequisites": ["storage-and-images"],
  "sources": [
    { "path": "apps/os_sdk.trit", "label": "Application SDK", "kind": "source" },
    { "path": "apps/libwidget.trit", "label": "Widget library", "kind": "source" },
    { "path": "APP_MANIFEST.json", "label": "Bundled app manifest", "kind": "manifest" },
    { "path": "build_tos_image.cpp", "label": "Application image registration", "kind": "source" },
    { "path": "tools/trit-test.ps1", "label": "Release test entry point", "kind": "tool" }
  ],
  "evidence": [
    { "path": "tests/test_native_apps.cpp", "label": "Bundled app tests", "kind": "test" },
    { "path": "tests/test_process_handoff.cpp", "label": "Process handoff tests", "kind": "test" }
  ],
  "next": null,
  "interactive": {
    "kind": "choice",
    "title": "Release check",
    "prompt": "What must change when a new bundled app is added?",
    "options": ["Only the UI label", "The app source, image builder, manifest, and focused tests", "Only a Graphify snapshot"],
    "answer": 1,
    "explanation": "A bundled app is a release contract. Its source, image registration, APP_MANIFEST entry, and focused tests must stay aligned."
  }
}
---

## The user-facing end of the stack

Apps compile against the SDK and shared widget/library code. The image builder
registers them, `APP_MANIFEST.json` records their guest paths and metadata, and
process-handoff tests verify that a release can actually launch them.

## Validation loop

For a source or manifest change, use the smallest focused test first, then run
the release checks. The repository's doctor, smoke, production, and image
inspection tools are the evidence trail; Graphify remains advisory navigation.

## Prerequisites

Complete storage and images. You should now be able to name the source,
contract, and test that sit at each layer of the path.

## You reached implementation

The EECS path ends at actual app registration and validation evidence. From here
you can choose a challenge, inspect a source symbol, or pick a known gap from
the repository roadmap instead of treating the guide as a replacement for the
implementation.
