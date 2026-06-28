# Known Gaps

These are intentionally visible so agents can pick useful work without asking for direction.

## Highest Priority

- Implement real syscall tracing behind `syscall_trace.jsonl`.
- Implement deterministic replay traces for `tools/trit-replay.ps1`.
- Add guest `/bin/doctor`, `/bin/test`, `/bin/sysinfo`, `/bin/log`, and richer diagnostics for existing `/bin/ps`, `/bin/fsck`, and `/bin/sync`.
- Add fuzz harnesses for malformed image files and bad syscall pointers.
- Add crash/power-loss scenarios for VFS and WAL recovery.
- Keep root manifests and the Obsidian vault synchronized as source contracts change.

## Medium Priority

- Export framebuffer PNGs in addition to `framebuffer_snapshot.txt`.
- Add structured package validation for installed apps and `/apps/registry`.
- Add app-level golden output/snapshot tests for desktop, terminal, file manager, and settings.
- Add performance baselines for boot time, app launch time, frame time, disk IO, and context switches.
- Expand Graphify/Trit extraction beyond direct AST symbols and call edges, for example syscall ID cross-links, app bundle ownership, image-section producers, and test-to-source coverage.

## Lower Priority

- Make `tools/trit-replay.ps1` understand future trace formats.
- Add Linux/macOS shell wrappers for the host tools.
- Generate JSON manifests from source constants after the contracts stabilize.
- Add automated freshness checks that warn when `docs/trit-stack.canvas` or generated Graphify summaries are stale relative to source edits.
