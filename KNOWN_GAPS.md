# Known Gaps

These are intentionally visible so agents can pick useful work without asking for direction.

## Highest Priority

- Add guest `/bin/doctor`, `/bin/test`, `/bin/sysinfo`, `/bin/log`, and richer diagnostics for existing `/bin/ps`, `/bin/fsck`, and `/bin/sync`.
- Add a dedicated syscall harness for randomized malformed pointers; the
  structural image validator harness does not exercise guest pointer faults.
- Complete VFS allocator compaction and atomic rename. First-fit inode, dirent,
  extent, and data-hole reclamation, open-unlink lifetime rules, inode
  generation checks, inode-scoped `fsync`, torn-record recovery,
  namespace/quota policy home pages, and the existing crash matrix are covered.
- Extend the x86-64 backend's direct lowering beyond the current scalar subset
  and hot internal branch loops. NOP/MOV/COPY, integral T40
  Add/Sub/TCmp/Mul, raw-valid T40 Neg/Abs, guarded dense identity Load/Store,
  and internal branches execute as emitted x86-64. MMU/sparse/tagged memory,
  non-local control, and unsupported operations use precise helper side exits.
  Native JIT remains disabled by default because the final candidate produced
  one CV failure followed by one clean pass; require repeatable controlled-host
  stability before enabling it globally.
- Complete first-class vector and aggregate-return language/ABI design.
  Optimized SSA is the sole target-code source and reports zero AST replay;
  graph coloring/coalescing and iterative spill rewriting are active, and the
  deterministic corpus records a 23.29% median instruction reduction with no
  workload regression. Aggregate parameters use one-word caller-owned pointers
  across both argument registers and the outgoing stack, and live external
  addresses have call-clobber coverage. ABI v2 deliberately rejects aggregate
  returns and first-class vector boundaries until an authoritative contract is
  versioned instead of silently miscompiling them.
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
