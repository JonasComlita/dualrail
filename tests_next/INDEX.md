# Verification Index

This index is a human-readable companion to the manifests.

## Current State

The new suite is scaffolded but intentionally not authoritative yet. The
historical `tests/` tree remains authoritative until `tests_next` reaches
coverage parity.

## Improvement Lens

Use `tests_next` to identify safe improvements as coverage grows. Differential
tests, fault-locality tests, cache/JIT counters, queue-depth checks, and focused
benchmarks should make optimization candidates visible without turning fast
correctness gates into flaky timing tests.

## First Porting Targets

1. ISA and assembler golden encoding tests.
2. VM arithmetic, traps, MMU, and sparse memory tests.
3. Compiler golden program tests.
4. Kernel syscall and VFS persistence tests.
5. GUI six-layer tests with framebuffer golden snapshots.
6. Full-system desktop boot, app launch, file write, reboot, readback.

## Risk Areas To Add Edge Cases For

- zero PTE state faults;
- bad user pointers that must not mutate kernel state;
- crash during WAL phases;
- VFS writes crossing extent windows;
- app launch from disk-backed executable images;
- compositor input routing and close events;
- text rendering boundaries and missing glyphs;
- `.tboot/.tdisk` corruption and version mismatch;
- branch/profile/JIT backend equivalence once those features land;
- optimization signals that need follow-up, such as redundant decode work,
  avoidable lane conversions, excessive state copying, or unstable cache
  invalidation boundaries.
