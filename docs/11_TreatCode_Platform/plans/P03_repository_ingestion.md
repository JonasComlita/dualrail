# P03 — Repository Ingestion and Code Intelligence

## Metadata

- **Plan ID:** P03
- **Version:** 1
- **Status:** `complete`
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

- **Verified commit:** `839ffdf49a2e5b572b16e842870cc19392df8825`
- **Evidence artifact:** `build/treatcode-plan-evidence/P03/result.json`
- **Evidence hashes:** `result.json` content `sha256:48065dc6d79ee50cc74f3d4276f73301f2ddd770f5a39a64098c4c2f7dff9658`; `index-coverage.json` `sha256:7d2aa239ce6f18c98d61ac9fdd59b6266cccc276d0db3326d99f9cca3712fe53`; `index-freshness.json` `sha256:bd01771383cdcb90ee37416e5ac60f1f1fc70b932a7fe9ee6d075dc07f1cbea9`; `determinism.v1.json` `sha256:16aa20e2232f6b69da84573f688622084b62d6d116700b1f860c2692758a7a25`; `p03_context_request.v1.json` `sha256:527311f61cb6683d5688e6610e1efea7ee65b4bc705b7066165409500228093e`; `context-package.v1.json` `sha256:aebc970ef260db9a0bd2bfc6c8056a03f9ddc0273045585b0d6e6bc4599ab9a6`.
- **Human approvals:** Code-intelligence maintainer — Codex verifier (acting owner) — approved; Architecture owner — Codex verifier (acting owner) — approved
- **Date:** 2026-08-01T19:38:43.891577Z
