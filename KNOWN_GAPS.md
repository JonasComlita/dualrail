# Known Gaps

These are intentionally visible so agents can pick useful work without asking for direction.

## Highest Priority

- Complete the remaining x86-64 dynamic/non-local control lowering (`RET`,
  `CALLR`, and `JMPR`) and archive controlled-host timing evidence before
  enabling the native JIT by default. Immediate `CALL` is now directly lowered
  with wide-register guards, and deterministic seven-repeat fingerprints pass;
  wall-clock CV still exceeds the global gate on this host.
- Complete the VM/image side of first-class vector and function ABI v3.
  Compiler ABI v3 now supports nested caller-owned `sret` aggregate returns
  across register and stack arguments, while executable headers/loaders remain
  ABI v2 and vector boundaries fail closed pending VLEN, fault, accumulator,
  and spill-state ownership.

## Medium Priority

- Materialize the provenance-locked external benchmark payloads when repository
  storage policy permits: two Freedoom 0.13.0 WADs and the official ~1.18 GB
  BitNet safetensors/GGUF artifacts. Offline staged import, hashes, licenses,
  WAD/model validation, and metadata-only fixtures are implemented; no full
  payload is currently committed or claimed.
- Add guest app-SDK conversions and broader image/memory dump integration for
  the implemented TASCII-81, `0t`, `0z27:`, and `0z81:` host/assembler/TCL
  contract.
- Complete production integration and cryptographic review for encrypted
  volumes. The versioned host `TRITENC1` AES-256-GCM envelope and optional
  OpenSSL provider are implemented and tested; default remains fail-closed,
  and the separately documented ternary-native format remains non-selected.

## Lower Priority

No open lower-priority tooling items are currently recorded. Replay schema
adapters, POSIX wrappers, source-derived manifest checks, and canvas/Graphify
freshness warnings now have focused coverage.
