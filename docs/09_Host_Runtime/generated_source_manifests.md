# Contract snapshots

The root manifests are the reviewable source of truth:

- `ARCHITECTURE_MANIFEST.json` — ISA and executable/function ABI;
- `IMAGE_FORMAT_MANIFEST.json` — tBoot v3 and tDisk v2;
- `SYSCALL_MANIFEST.json` — syscall ABI and compiler wrappers;
- `APP_MANIFEST.json` — bundled applications and guest paths.

The checked-in `architecture_contract.h` and `architecture_contract.trit` files
are snapshots consumed by the current ISA/runtime sources. They are not a
second compatibility contract and are kept at the repository root so the
current platform has one obvious contract surface.

Run the consolidated checker from the repository root:

```text
python tools/trit_tool.py contract-check
```

If a contract changes, update its root manifest, snapshot, and direct producer
or consumer together. Do not recreate the retired manifest generator or an
additional source-manifest registry.
