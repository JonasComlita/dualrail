---
{
  "schema": "trit.treatcode_learning_page.v2",
  "id": "storage-and-images",
  "title": "Storage and images",
  "module": "Phase 14 · Kernel storage: BIO, buffer pool, WAL, relational state, and VFS",
  "level": "programmer",
  "order": 28,
  "summary": "Build the mental model for Kernel storage: BIO, buffer pool, WAL, relational state, and VFS.",
  "phase_id": "tc:layer:phase-14-storage-vfs",
  "phase_slug": "storage-vfs",
  "phase_name": "Kernel storage: BIO, buffer pool, WAL, relational state, and VFS",
  "lesson_kind": "mental-model",
  "implementation_status": "complete",
  "canonical_terms": [
    "BIO",
    "buffer pool",
    "WAL",
    "VFS",
    "recovery point"
  ],
  "objectives": [
    "Explain block I/O, caching, write-ahead logging, recovery, relational state, filesystem and VFS operations, and consistency.",
    "Distinguish the general concept from the current Trit implementation and planned gaps.",
    "Use a worked example and repository evidence to validate one claim."
  ],
  "prerequisites": [
    "kernel-memory-process"
  ],
  "sources": [
    {
      "path": "kernel/bio.trit",
      "label": "bio.trit",
      "kind": "source"
    },
    {
      "path": "kernel/vfs.trit",
      "label": "vfs.trit",
      "kind": "source"
    },
    {
      "path": "ternary_redo_wal.h",
      "label": "ternary_redo_wal.h",
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
      "path": "tests/test_production_layers.cpp",
      "label": "tc:test:test-production-layers test source",
      "kind": "test"
    },
    {
      "path": "tests/test_migration_v2.cpp",
      "label": "tc:test:test-migration-v2 test source",
      "kind": "test"
    },
    {
      "path": "tests/test_redo_wal_v2.cpp",
      "label": "tc:test:test-redo-wal-v2 test source",
      "kind": "test"
    },
    {
      "path": "tests_next/08_filesystem_vfs/next_vfs_persistence.cpp",
      "label": "tc:test:next-vfs-persistence test source",
      "kind": "test"
    },
    {
      "path": "tests_next/08_filesystem_vfs/next_vfs_recovery.cpp",
      "label": "tc:test:next-vfs-recovery test source",
      "kind": "test"
    }
  ],
  "test_ids": [
    "tc:test:test-production-layers",
    "tc:test:test-migration-v2",
    "tc:test:test-redo-wal-v2",
    "tc:test:next-vfs-persistence",
    "tc:test:next-vfs-recovery"
  ],
  "benchmark_ids": [
    "tc:benchmark:test-scaling-profile-compact"
  ],
  "gap_ids": [
    "tc:gap:trit-gap-crash-recovery",
    "tc:gap:trit-gap-encrypted-volumes"
  ],
  "stack_links": {
    "phase": "/stack/storage-vfs",
    "source": "https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/kernel/bio.trit",
    "tests": "/stack?focus=tc%3Atest%3Atest-production-layers"
  },
  "next": "storage-recovery-vfs",
  "interactive": {
    "kind": "choice",
    "title": "Concept check",
    "prompt": "Which boundary protects a storage update after interruption?",
    "options": [
      "A write-ahead log before durable metadata",
      "A UI label",
      "A benchmark name"
    ],
    "answer": 1,
    "explanation": "The repository boundary is defined by the current source and validation evidence for Kernel storage: BIO, buffer pool, WAL, relational state, and VFS. Planned and unavailable work remains labelled."
  }
}
---

## Objectives

- Explain block I/O, caching, write-ahead logging, recovery, relational state, filesystem and VFS operations, and consistency in general computer-systems terms.
- Identify the current Trit implementation, its entry and exit artifacts, and its explicit status.
- Use a worked example and the linked interaction to make one falsifiable claim.
- Leave with a next step that keeps prerequisites, source, and validation visible.

## Prerequisites

Read [kernel-memory-process](/learn/eecs/kernel-memory-process) first. Those lessons introduce the state and vocabulary that this page assumes. This is the mental-model lesson for Kernel storage: BIO, buffer pool, WAL, relational state, and VFS. Begin with the general systems idea, then compare it with the current Trit boundary before you touch a tool. A prerequisite is a reasoning dependency, not a UI gate: a direct link to this page must still explain what is assumed.

## Why this topic exists

Storage is a consistency problem across volatile memory, durable blocks, metadata, and interruption. BIO moves requests, a buffer pool avoids repeated I/O, a WAL records intent before a state change, relational state organizes metadata, and a VFS gives callers a stable naming and operation surface. Recovery is part of correctness because a power loss can occur between any two writes. In a real system, the boundary exists because two parts need to cooperate without sharing every implementation detail. It makes a failure local enough to diagnose and a change narrow enough to review. The useful question here is not only “what does Trit call this?” but “what invariant would another computer system need at the same boundary?”

## Explanation

kernel/bio.trit, kernel/vfs.trit, and ternary_redo_wal.h implement the current storage surfaces. Production-layer, migration, WAL, and next VFS persistence/recovery tests provide the evidence trail. Image formats are related but distinct: a release image packages an environment, while a WAL protects updates inside a storage system. The phase enters through a logical storage request and a block or volume boundary and leaves through a durable or recoverable operation with stated consistency and failure behavior. The distinction between the ideal concept and the repository implementation matters: a textbook may describe a complete mechanism, while the current code may implement a bounded slice, a host adapter, or an explicit stub. TreatCode's labels are therefore part of the explanation. “Implemented” means the cited source and validation support a current behavior; “planned” means the roadmap or gap records a future behavior; “unavailable” means the evidence or required asset is not present.

To update a directory entry and its inode, append a redo record describing the intended new values, flush the log according to its contract, then update the durable structures. On restart, recovery replays complete records or discards incomplete ones. If the log is written after the metadata, a torn update can leave a name pointing at the wrong object. The same reasoning scales beyond the example. Name the input, the state that changes, the owner of that state, the output, and the test that would catch a regression. If one of those is missing, say so. A source link can help a reader continue, but it cannot substitute for the prose that explains why the source matters.

## Current implementation and planned work

kernel/bio.trit, kernel/vfs.trit, and ternary_redo_wal.h implement the current storage surfaces. Production-layer, migration, WAL, and next VFS persistence/recovery tests provide the evidence trail. Image formats are related but distinct: a release image packages an environment, while a WAL protects updates inside a storage system. The current registry row is tc:layer:phase-14-storage-vfs, with implementation coverage marked complete and tested coverage marked complete. The lesson is anchored to the current public snapshot; a later snapshot may change paths or statuses and must be regenerated rather than silently inferred.

Crash recovery and encrypted volumes remain recorded gaps. A passing WAL fixture demonstrates the tested recovery contract, not every filesystem failure mode or production disk guarantee. Keep the wording scoped. A planned driver, missing benchmark asset, or unavailable hardware trace is valuable information for a contributor because it describes the next evidence needed. It is not a failure of the concept, and it is not permission to call a partial path complete.

## Worked example

To update a directory entry and its inode, append a redo record describing the intended new values, flush the log according to its contract, then update the durable structures. On restart, recovery replays complete records or discards incomplete ones. If the log is written after the metadata, a torn update can leave a name pointing at the wrong object. For this lesson, write the example as a sequence: first identify a logical storage request and a block or volume boundary; next apply the storage-vfs rule; then inspect a durable or recoverable operation with stated consistency and failure behavior; finally compare the result with the named validation record. The conceptual check should explain why the result would be wrong if a neighboring representation or layer were substituted. This sequence is deliberately small enough to execute or trace, yet concrete enough to expose a wrong assumption.

## Common misconception

A cache is not durability and a filesystem path is not a block address. WAL ordering protects a recovery story, but it does not replace a volume format or encryption policy. Returning success before the specified durable point changes the contract. Another common shortcut is to treat a green UI response as proof that the underlying compiler, VM, kernel, or device completed the work. The interaction on this page names its limits and points back to the exact source and test records. When the result is unavailable, the correct learner response is to report the limitation and preserve the evidence trail.

## Learner action

Write a two-write failure table for a rename. For each interruption point, state what recovery should see, what record proves it, and whether the current gap list covers the case. Record your result in four sentences: what you expected, what state or artifact you inspected, which source/test evidence supports it, and what remains uncertain. If you are working in the code exercise, keep the program bounded and observe the returned compiler or VM result. If you are working in the trace, advance one state at a time and do not skip the invariant. This action turns reading into a reproducible investigation that another learner can review.

## Source links

- [bio.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/kernel/bio.trit) — kernel/bio.trit
- [vfs.trit](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/kernel/vfs.trit) — kernel/vfs.trit
- [ternary_redo_wal.h](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/ternary_redo_wal.h) — ternary_redo_wal.h

These are authoritative repository references for the claim. The [Stack Explorer phase](/stack/storage-vfs) provides the full relationship view, including related components, contracts, decisions, tests, benchmarks, releases, and gaps.

## Validation and evidence

- [Authoritative test manifest](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/TEST_MANIFEST.json) — TEST_MANIFEST.json
- [tc:test:test-production-layers test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_production_layers.cpp) — tests/test_production_layers.cpp
- [tc:test:test-migration-v2 test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_migration_v2.cpp) — tests/test_migration_v2.cpp
- [tc:test:test-redo-wal-v2 test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests/test_redo_wal_v2.cpp) — tests/test_redo_wal_v2.cpp
- [tc:test:next-vfs-persistence test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/08_filesystem_vfs/next_vfs_persistence.cpp) — tests_next/08_filesystem_vfs/next_vfs_persistence.cpp
- [tc:test:next-vfs-recovery test source](https://github.com/JonasComlita/dualrail/blob/02f764a96b6a9ce98b3b63a07a0771b04d1a627d/tests_next/08_filesystem_vfs/next_vfs_recovery.cpp) — tests_next/08_filesystem_vfs/next_vfs_recovery.cpp

The test IDs attached to this lesson are tc:test:test-production-layers, tc:test:test-migration-v2, tc:test:test-redo-wal-v2, tc:test:next-vfs-persistence, tc:test:next-vfs-recovery. Run the smallest focused check first, then the broader suite named by the plan. Evidence is commit-addressed and may be marked active, referenced, planned, or unavailable. Read the status before repeating the conclusion. This phase also has benchmark records tc:benchmark:test-scaling-profile-compact; interpret them only with their workload and baseline.

## Next step

[Continue to the next lesson](/learn/eecs/storage-recovery-vfs). The next page should make the dependency explicit and keep the same distinction between general concept, current Trit behavior, and planned work. When you reach the terminal lesson, write the closure report and list residual gaps instead of assuming that navigation itself proves completion.
