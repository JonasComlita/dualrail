# Repository ingestion and code intelligence

The TreatCode repository index is a generated, commit-addressed snapshot of
the Git-tracked repository. Its format is
"treatcode.repository-index.v1"; generated output lives under
build/treatcode-index/ and is not source authority.

Build and verify it from the repository root:

~~~
python tools/trit_tool.py website index build --clean
python tools/trit_tool.py website index verify
python tools/trit_tool.py website index compare-clean-incremental
~~~

The inventory is exactly "git ls-files". Every tracked file receives a
metadata record and SHA-256 hash. Unsupported text and binary files are
metadata-only; generated, downloaded, and fixture files remain visible but
carry source_authority: false. Untracked build output is outside the
authoritative index.

Freshness is checked against the recorded HEAD commit, not only against the
current worktree bytes. If a tracked path differs from HEAD, whether staged or
unstaged, verification reports `fresh: false` and lists it in
`dirty_tracked_files`; an index built from that dirty worktree cannot be a
fresh commit-addressed index.

Supported source parsers currently extract declarations and source spans for
C/C++, Trit, assembly, Python, JavaScript, and TypeScript. Include/import and
direct-call edges record the source span of the reference and distinguish
resolved, external, and unresolved targets. JSON manifests become queryable
record entities. Test, benchmark, documentation, and decision references are
materialized as typed relationships.

Create a bounded context package for a task or component:

~~~
python tools/trit_tool.py website index context --scope kernel.trit --output build/context/kernel.json
~~~

The package contains the requested scope, one-hop direct dependencies, and
only the associated contracts, tests, gaps, benchmark baselines, documents,
symbols, and relationships. It contains provenance and a stable package hash,
but not source text copied outside the selected scope.
