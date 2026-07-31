---
{
  "schema": "trit.treatcode_learning_page.v1",
  "id": "storage-and-images",
  "title": "Storage and images",
  "module": "8. Persistence",
  "level": "eecs",
  "order": 80,
  "summary": "Connect word-addressed storage, write-ahead logging, recovery, and release image formats.",
  "prerequisites": ["kernel-syscalls"],
  "sources": [
    { "path": "ternary_redo_wal.h", "label": "Redo write-ahead log", "kind": "source" },
    { "path": "docs/09_Host_Runtime/image_format.md", "label": "Image format explanation", "kind": "documentation" },
    { "path": "IMAGE_FORMAT_MANIFEST.json", "label": "Image format contract", "kind": "manifest" },
    { "path": "build_tos_image.cpp", "label": "Release image builder", "kind": "source" }
  ],
  "evidence": [
    { "path": "tests/test_redo_wal_v2.cpp", "label": "WAL recovery tests", "kind": "test" },
    { "path": "tests/test_formats.cpp", "label": "Image format tests", "kind": "test" }
  ],
  "next": "applications-and-validation",
  "interactive": {
    "kind": "choice",
    "title": "Recovery check",
    "prompt": "Why does the storage layer use a write-ahead log?",
    "options": ["To make every word a lane", "To recover a consistent update after interruption", "To replace the image manifest"],
    "answer": 1,
    "explanation": "The WAL records the intended change before the durable state is rewritten, so recovery can distinguish committed, incomplete, and stale records."
  }
}
---

## Two related contracts

The runtime stores data in words and the release builder packages bootable
sections into `.tboot` and `.tdisk` images. The image manifest explains the
format; the WAL protects updates within the storage system. They solve different
failure boundaries.

## Recovery is part of the feature

A write that works only when power never fails is not a complete storage
contract. The redo log records enough information to replay or discard an
interrupted update. The focused recovery tests are the evidence to read after
the header and WAL implementation.

## Prerequisites

Know the syscall boundary and the difference between a kernel service and a
release artifact. Then compare the format manifest, image builder, and WAL.

## Next step

Continue to **Applications and validation** to see how apps, manifests, and
process-handoff tests turn the stack into a user-facing release.
