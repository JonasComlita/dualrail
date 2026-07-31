# P03 — Repository Ingestion and Code Intelligence

## Metadata

- **Plan ID:** P03
- **Version:** 1
- **Status:** `in_progress`
- **Depends on:** P01, P02
- **Scope owner:** Code intelligence

## Objective

Produce a deterministic, commit-addressed index connecting repository files,
symbols, relationships, manifests, documentation, tests, and benchmarks.

## Dependency Evidence Required

- P01 and P02 completion evidence and verified commits.

## Inputs and Authority

- Git-tracked repository contents
- Root and subsystem manifests
- Compiler AST and symbol tools where available
- Graphify output as advisory input only

## Deliverables

1. A versioned ingestion schema and index format.
2. Clean and incremental index builders.
3. File, symbol, source-span, import/include, direct-call, manifest, test,
   benchmark, documentation, and decision relationships.
4. A context-package generator for a bounded task or component.
5. Machine-readable coverage and freshness reports.

## Non-Goals

- Claiming semantic understanding from embeddings alone.
- Indexing ignored build outputs as authoritative source.
- Providing write access or remote workspaces.

## Acceptance Criteria

- [x] Every Git-tracked file is indexed or excluded by a documented rule.
- [x] Every source reference contains repository, commit, path, and source span
      when the parser supports spans.
- [x] Supported-language symbol references resolve back to exact source.
- [x] Manifest, test, benchmark, documentation, and decision edges are queryable.
- [x] A clean index and an incremental index of the same commit are equivalent.
- [x] Generated and downloaded artifacts are labeled and never promoted to
      source authority.
- [x] Context packages contain only requested scope, direct dependencies,
      required contracts, tests, gaps, and baselines.

## Verification

```powershell
python tools/trit_tool.py website index build --clean
python tools/trit_tool.py website index verify
python tools/trit_tool.py website index compare-clean-incremental
python tools/trit_tool.py website plan verify P03
```

## Required Evidence

- Index coverage report.
- Determinism comparison.
- Context-package fixture and hash.
- build/treatcode-plan-evidence/P03/index-coverage.json
- build/treatcode-plan-evidence/P03/index-freshness.json
- build/treatcode-plan-evidence/P03/determinism.v1.json
- docs/11_TreatCode_Platform/fixtures/p03_context_request.v1.json
- build/treatcode-plan-evidence/P03/context-package.v1.json

## Completion Record

- **Verified commit:**
- **Evidence artifact:**
- **Human approvals:** Code-intelligence maintainer; Architecture owner
- **Date:**
