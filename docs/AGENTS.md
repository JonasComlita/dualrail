# Docs vault agent guide

Open `docs/` as the Obsidian-friendly explanation vault. Root source files and
manifests remain authoritative; these pages explain and link to that source.
Graphify output and the canvas are advisory navigation only.

Start with `README.md`, `INDEX.md`, and `STATUS.md`. For platform changes read
the root `ARCHITECTURE_MANIFEST.json`, `IMAGE_FORMAT_MANIFEST.json`,
`SYSCALL_MANIFEST.json`, `ROADMAP_STATUS.json`, and `KNOWN_GAPS.md`.

The supported validation entry points are:

```text
python ../tools/trit_tool.py doctor
python ../tools/trit_tool.py contract-check
cmake --build ../build --target current_validate
cmake --build ../build --target ci_production
```

The platform is ISA v2, executable/function ABI v3, tBoot v3, and tDisk v2
only. Do not document old image loaders, migration utilities, or deleted test
and wrapper trees as current capabilities. Keep links source-relative and do
not commit Obsidian workspace metadata.
