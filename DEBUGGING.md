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
- `input_journal.jsonl`: cycle-stamped keyboard, text, and mouse events. Each
  record retains the v1 schema and adds deterministic external provenance
  (`source`, `channel`, and `external`) under `provenance`.
- `checkpoint.json`: v2 metadata retaining the original v1 fields. Every
  diagnostics export also contains a self-contained `checkpoint/` directory
  with `boot.tboot`, `vm_state.bin`, `disk.tdisk`, `checkpoint.bin`, and the
  binary/JSONL replay streams. Paths in the root manifest and metadata are
  relative to the diagnostics directory; the nested directory can be passed
  directly to `TosRuntime::restoreCheckpointBundle`.
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
bundle, and each export now carries a file-backed checkpoint under
`checkpoint/`. `TosRuntime::exportCheckpointBundle` plus
`restoreCheckpointBundle` provide file-backed restore into a fresh runtime
before `replayFromCheckpoint` performs rewind and re-execution. The
`trit_checkpoint_replay` helper (also exposed as
`tools/trit_tool.py replay --execute`) exercises the same bundle in a fresh
process. Run `python tools/trit_tool.py fuzz --skip-tests` for deterministic,
fail-closed malformed `.tboot`/`.tdisk` and checkpoint-restore validation;
differential replay and randomized bad-pointer coverage remain open in
`KNOWN_GAPS.md`.

Replay schemas use `trit.<family>.v<major>[.<minor>]` identifiers. The current
syscall trace and input journal adapters support major v1; checkpoint metadata
supports the existing v1 and v2 majors. A future minor (for example
`trit.syscall_trace.v1.1`) is accepted only when all current required fields
and value shapes still validate. Unknown fields are ignored for deterministic
comparison and counted in the JSON `schema_capabilities` report. An unknown
or unsupported major fails closed with an explicit diagnostic; the replay tool
never guesses the meaning of a changed major. Use `--json` to inspect the
adapter, observed versions, future-minor status, and ignored fields.

Graphify currently augments `.trit` sources with compiler-AST-derived files,
functions, constants, structs, imports, syscall nodes, and direct call edges. It
does not replace semantic compiler tests, syscall trace capture, or runtime
diagnostics.
