# Known Gaps

These are intentionally visible so agents can pick useful work without asking for direction.

## Highest Priority

No highest-priority implementation gaps are currently recorded. The dynamic
control JIT, executable/vector ABI v3 rollout, symbolic guest/dump integration,
and real external benchmark acceptance have passed their focused or manual
gates. NativeX64Jit is now the host-runtime default on accepted x86-64 hosts;
portable hosts retain CachedBlockInterpreter.

## Medium Priority

- External payload acceptance is complete in the ignored, provenance-locked
  local cache: Freedoom slice/full frame hashes and official BitNet slice/full
  token/reference hashes pass. Payloads, converted tensors, and inventories
  remain intentionally untracked; a fresh machine must acquire the cache, and
  the normal offline benchmark profile remains synthetic.
- Complete the independent cryptographic review for the production host-only
  `TRITENC1` lifecycle. The strict key provider, OpenSSL 3 AES-256-GCM path,
  parser, CLI, managed plaintext lifecycle, atomic replacement, and focused
  tamper/cleanup tests are implemented. The review gate still has unresolved
  high-severity nonce-uniqueness, replay/rollback, crash-cleanup, key-file
  race, provider-policy, and fuzz-bound findings. Any format-affecting
  remediation requires a new envelope version; the ternary-native proposal
  remains explicitly non-selected.

## Lower Priority

No open lower-priority tooling items are currently recorded. Replay schema
adapters, POSIX wrappers, source-derived manifest checks, and canvas/Graphify
freshness warnings now have focused coverage.
