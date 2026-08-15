# Generated source-contract manifests

`tools/generate_source_manifests.py` derives the mechanically checkable parts
of the syscall, bundled-app, and image-format contracts.  It writes the
canonical sidecar `generated/source_contract_manifest.json`.

Run the safe check from the repository root:

```powershell
python tools/generate_source_manifests.py --check
```

Refresh the sidecar only after reviewing source changes:

```powershell
python tools/generate_source_manifests.py --write
```

`--write` never rewrites `SYSCALL_MANIFEST.json`, `APP_MANIFEST.json`, or
`IMAGE_FORMAT_MANIFEST.json`.  Those root files remain authoritative for
policy and explanatory fields that cannot be derived safely: service groups,
statuses, notes, calling-convention prose, root layout, default user,
registry/validation metadata, payload field descriptions, migration policy,
and tool descriptions.  The checker compares only stable source-derived
values (IDs, compiler wrappers, app ownership/paths/stack classes, and image
magic/version/block constants), so reordering or editing hand-authored prose
does not create false drift.

The legacy `SYSCALL_*` block in `ternary_vm_state.h` is retained for its older
host surface.  The generator records its values and reports the four known
ID conflicts with the v2 compiler runtime namespace as warnings; it does not
rewrite or treat that legacy block as the current syscall ABI.
