# Known Gaps

These are intentionally visible so agents can pick useful work without asking for direction.

## Highest Priority

- Add guest `/bin/doctor`, `/bin/test`, `/bin/sysinfo`, `/bin/log`, and richer diagnostics for existing `/bin/ps`, `/bin/fsck`, and `/bin/sync`.
- Add a dedicated syscall harness for randomized malformed pointers; the
  structural image validator harness does not exercise guest pointer faults.
- Add crash/power-loss scenarios for VFS and WAL recovery.
- Extend the x86-64 backend's direct lowering beyond hot internal branch loops.
  NOP/MOV/COPY and internal branch control now execute as emitted x86-64, and
  the three-workload wall-time gate passes on the measured host; arithmetic,
  guarded memory, and unsupported operations still use precise helper side
  exits, so native JIT remains disabled by default until those operations are
  lowered inline as well.
- Complete the IR-first compiler transition for the remaining unsupported
  cases: optimized SSA now drives promoted and address-taken local frame
  memory (including loop-carried accesses), small scalar external-memory
  regions beside calls, calls with outgoing stack arguments, branch-preserving
  side-effectful matches, branch-free `TSEL` matches, scalar tuple swaps, and
  the allocator reserves target scratch registers across spill rewrites.
  Aggregate/vector values and several complex kernel control/data-flow regions
  still need memory-SSA aliasing, aggregate lowering, and call-clobber proofs.
  A fail-closed `--strict-ssa` mode rejects unsupported target lowering; the
  default transition build still permits only the explicitly reported replay
  set until those functions are lowered.
- Keep root manifests and the Obsidian vault synchronized as source contracts change.

## Medium Priority

- Export framebuffer PNGs in addition to `framebuffer_snapshot.txt`.
- Add structured package validation for installed apps and `/apps/registry`.
- Add app-level golden output/snapshot tests for desktop, terminal, file manager, and settings.
- Add performance baselines for boot time, app launch time, frame time, disk IO, and context switches.
- Expand Graphify/Trit extraction beyond direct AST symbols and call edges, for example syscall ID cross-links, app bundle ownership, image-section producers, and test-to-source coverage.
- Import or adapt full external Doom-class and BitNet 1.58B-class assets after
  the deterministic `benchmark_doom_os` and `benchmark_bitnet_os` synthetic OS
  portfolio workloads establish stable capability and metric baselines.
- Formalize and implement ternary-native symbolic encodings, including `TASCII-81`, exact trit literals, compact base-27/base-81 dump notation, and explicit ASCII/UTF-8/hex conversion tests.

## Lower Priority

- Make `tools/trit-replay.ps1` understand future trace formats.
- Add Linux/macOS shell wrappers for the host tools.
- Generate JSON manifests from source constants after the contracts stabilize.
- Add automated freshness checks that warn when `docs/trit-stack.canvas` or generated Graphify summaries are stale relative to source edits.
