# Known Gaps

These are intentionally visible so agents can pick useful work without asking for direction.

## Highest Priority

- Add VM checkpoints and guest-input journaling for execution replay. The
  runtime now emits validated syscall events in `syscall_trace.jsonl`, and
  `tools/trit-replay.ps1` validates or compares those captures, but it does
  not yet rewind and re-execute a guest.
- Add guest `/bin/doctor`, `/bin/test`, `/bin/sysinfo`, `/bin/log`, and richer diagnostics for existing `/bin/ps`, `/bin/fsck`, and `/bin/sync`.
- Add fuzz harnesses for malformed image files and bad syscall pointers.
- Add crash/power-loss scenarios for VFS and WAL recovery.
- Lower supported micro-ops directly to x86-64 instead of the current
  helper-backed W^X thunk. The three-workload wall-time gate is now enforced by
  `test_execution_backends_benchmark` and currently fails on the host until
  direct lowering is fast enough; native JIT remains disabled by default.
- Complete the IR-first compiler transition: frontend stack locals must feed
  real `mem2reg`, optimized IR must drive target emission, and spills must be
  rewritten until the interference graph is colorable.
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
