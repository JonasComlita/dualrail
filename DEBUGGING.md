# Debugging Guide

## Fast Triage

```powershell
tools/trit-doctor.ps1
tools/trit-test.ps1 smoke
python tools/trit_tool.py knowledge status
tools/trit-export-diagnostics.ps1 --output build/agent-diagnostics
```

Start with `build/agent-diagnostics/agent_diagnostics.json`. If a runtime smoke run was possible, also inspect `build/agent-diagnostics/runtime/`.

If the failure involves unfamiliar source relationships, refresh the optional
code graph:

```powershell
python tools/trit_tool.py knowledge graph --no-archive
```

The graph is advisory. Use it to find likely callers, wrappers, and related
docs, then confirm against source files, manifests, and tests.

## Diagnostic Bundle Contract

The host runtime exports:

- `manifest.json`: machine-readable image, runtime, disk, and app summary.
- `vm_state.txt`: PC, status, cycles, privilege, trap, cause, and disk path.
- `guest.log` and `kernel_log.txt`: guest console/syscall log text.
- `manifest.txt`: legacy text image manifest.
- `process_table.json`: process slots, states, wait metadata, parent/status/signal fields.
- `syscall_trace.jsonl`: schema-versioned syscall events when capture is
  enabled; disabled/empty captures are explicit markers.
- `input_journal.jsonl`: cycle-stamped keyboard, text, and mouse events.
- `checkpoint.json`: metadata for the last in-memory checkpoint; a
  `TosRuntime::exportCheckpointBundle` directory additionally contains
  `vm_state.bin`, `disk.tdisk`, `boot.tboot`, and binary/JSONL replay streams.
- `crash_report.txt`: crash-oriented status summary.
- `framebuffer_snapshot.txt`: framebuffer mode, dimensions, and color words.

## Common Failure Routes

- Build failure: run the exact target from `TEST_MANIFEST.json`, then inspect compiler diagnostics in stdout/stderr.
- Boot image failure: run `tools/trit-inspect-image.ps1 <image>` and check checksum, entry point, profile, rootfs alignment, and app registry.
- Runtime trap: export diagnostics and inspect `crash_report.txt`, `vm_state.txt`, and `process_table.json`.
- App launch failure: inspect `APP_MANIFEST.json`, image apps from `trit-inspect-image`, and process handoff tests.
- Filesystem or persistence failure: focus `test_os_platform`, `test_production_hardening`, `test_process_handoff`, and `.tdisk` existence/size.
- Documentation or agent tooling failure: run `python tools/trit_tool.py knowledge status --json`, `python tools/trit_tool.py knowledge setup --check --json`, and `ctest --test-dir build -R test_agent_tooling --output-on-failure`.
- `.trit` symbol navigation issue: build `trit_ast_dump`, rerun `python tools/trit_tool.py knowledge graph --no-archive --json`, and inspect `graphify-out/trit-symbols.json`.

## Current Trace Limits

The runtime captures syscall events, VM checkpoints, and cycle-stamped guest
input. `tools/trit-replay.ps1` validates or compares a complete diagnostics
bundle, and `TosRuntime::exportCheckpointBundle` plus
`restoreCheckpointBundle` provide file-backed restore into a fresh runtime
before `replayFromCheckpoint` performs rewind and re-execution. The
`trit_checkpoint_replay` helper (also exposed as
`tools/trit_tool.py replay --execute`) exercises the same bundle in a fresh
process. Run `python tools/trit_tool.py fuzz --skip-tests` for deterministic,
fail-closed malformed `.tboot`/`.tdisk` validation; differential replay and
randomized bad-pointer coverage remain open in `KNOWN_GAPS.md`.

Graphify currently augments `.trit` sources with compiler-AST-derived files,
functions, constants, structs, imports, syscall nodes, and direct call edges. It
does not replace semantic compiler tests, syscall trace capture, or runtime
diagnostics.
