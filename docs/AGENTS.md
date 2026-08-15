# Docs Vault Agent Guide

Open `docs/` as the Obsidian vault for Trit.

## Authority

- Root manifests, source files, and tests remain authoritative.
- Pages in this vault are curated navigation and explanation.
- Generated Graphify artifacts are advisory and must not override source code,
  manifests, or failing tests.

## Workflow

1. Start at `README.md`, then follow `INDEX.md` and `STATUS.md`.
2. Use `python ../tools/trit_tool.py knowledge status` to validate the vault.
3. Use `python ../tools/trit_tool.py knowledge canvas` after changing the docs map.
4. Use `python ../tools/trit_tool.py knowledge graph` only when Graphify is installed
   and a structural code report would help.
5. Treat freshness warnings as advisory contract checks. Regenerate the canvas
   after source-contract edits, and rerun Graphify when its archived summary is
   stale or legacy/unverified.
6. Keep manually written docs concise and source-linked; put generated Graphify
   runs under `_graphify/runs/`.

## Obsidian Conventions

- Prefer stable Markdown links for repo portability.
- Use wikilinks sparingly for important concepts that benefit from graph view.
- Keep `trit-stack.canvas` as the high-level navigation canvas.
- Do not commit Obsidian workspace layout files.
