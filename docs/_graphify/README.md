# Graphify Runs

This directory documents optional Graphify integration.

Graphify output is generated analysis, not a source of truth. The default raw
output directory is the repo-root `graphify-out/`, which is ignored by git.
Archived report snapshots may be written under `_graphify/runs/` by:

```powershell
python ..\tools\trit_tool.py knowledge graph
```

If Graphify is not installed, the command exits with guidance and leaves the
repo unchanged.

By default, `.graphifyignore` keeps Markdown, images, and the Obsidian vault out
of Graphify so extraction can run without an LLM API key. Obsidian remains the
docs/wiki layer; Graphify is the code graph layer.

Graphify does not natively parse `.trit` sources yet. Trit's `knowledge graph`
command therefore runs Graphify first, then augments `graphify-out/graph.json`
with a deterministic Trit adapter that extracts `.trit` file, function,
constant, syscall, and call edges. When the CMake `trit_ast_dump` target is
available, the adapter uses the compiler parser's `ModuleAst`; otherwise it
falls back to a lightweight text scan.
