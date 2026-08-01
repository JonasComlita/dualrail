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

- **Verified commit:** `645809a4472e042f1389f00c9f936c15977b83cf`
- **Evidence artifact:** `build/treatcode-plan-evidence/P03/result.json`
- **Evidence hashes:** `result.json` content `sha256:53b877c3d4df8bde9ad558081b73f7369c4e46e1a7b68d065eebddf10ccb2c5f`; `index-coverage.json` `sha256:867b297f060694bd6d1caad3b50dd8a9506746b9b4705639fc2aabf754b1ab30`; `index-freshness.json` `sha256:244430809b1261622b9fdd4f6424e305a9c7ef20da53c4dcd5d6662d061b088a`; `determinism.v1.json` `sha256:eb6bacc51ab3f2088eddb8b44f917b6cf93d029a29197777730dda2fe1b9be34`; `p03_context_request.v1.json` `sha256:527311f61cb6683d5688e6610e1efea7ee65b4bc705b7066165409500228093e`; `context-package.v1.json` `sha256:7c2fe09ff696f96c1b304372a0250688f6fdbd17c2a1b701d1796164adf34bc5`.
- **Human approvals:** Code-intelligence maintainer — Codex verifier (acting owner) — approved; Architecture owner — Codex verifier (acting owner) — approved
- **Date:** 2026-08-01T20:27:51.773689Z
