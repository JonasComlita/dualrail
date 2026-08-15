# Replay schema fixtures

These JSONL fixtures exercise the versioned replay adapter:

- `current_trace.jsonl` is the shipped `trit.syscall_trace.v1` shape.
- `future_minor_trace.jsonl` uses compatible `v1.1` metadata and an unknown
  optional field. The adapter accepts it, ignores the field, and canonicalizes
  the event to v1 for deterministic comparison.
- `unsupported_major_trace.jsonl` uses `v2`, which must fail closed with an
  explicit unsupported-major diagnostic. A major version is never guessed.
