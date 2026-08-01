# P04 — Public API and Stack Explorer

## Metadata

- **Plan ID:** P04
- **Version:** 1
- **Status:** `complete`
- **Depends on:** P02, P03
- **Scope owner:** Public platform

## Objective

Expose read-only, provenance-preserving project knowledge through a versioned API
and a fast static-first Stack Explorer while preserving TreatCode's visual
identity.

## Dependency Evidence Required

- P02 and P03 completion evidence and verified commits.

## Inputs and Authority

- Validated registries from P02
- Commit-addressed index from P03
- Existing TreatCode visual design

## Deliverables

1. Versioned OpenAPI contract for projects, stack nodes, components,
   capabilities, contracts, decisions, source, symbols, tests, benchmarks,
   runs, releases, and search.
2. Generated static snapshots for public data.
3. Stack Explorer routes with dependency, source, evidence, gap, and release
   views.
4. Exact, symbol, relationship, and semantic search with provenance.
5. API conformance, accessibility, responsive, and bundle-budget tests.

## Non-Goals

- Authentication, writes, agents, remote workspaces, or code execution.
- Replacing Git history or code review.
- Loading the code editor on public reading routes.

## Acceptance Criteria

- [x] Every response includes schema version and source snapshot or commit.
- [x] Public routes render useful content with client JavaScript disabled.
- [x] A user can navigate silicon-to-user dependencies and reach exact source,
      tests, benchmarks, decisions, and known gaps.
- [x] Known queries return authoritative results with exact citations.
- [x] Public reading routes ship no editor or runner bundle.
- [x] At 390 CSS pixels, required navigation and content have no horizontal
      page overflow.
- [x] Initial JavaScript for `/`, `/stack`, and `/learn` is at most 100 KiB
      gzip per route.
- [x] Product owner approves visual continuity with the existing TreatCode site.

## Verification

```powershell
npm.cmd --prefix treatcode run build
npm.cmd --prefix treatcode run test:api
npm.cmd --prefix treatcode run test:e2e:public
npm.cmd --prefix treatcode run test:a11y
npm.cmd --prefix treatcode run check:bundle-budget
python tools/trit_tool.py website plan verify P04
```

## Required Evidence

- OpenAPI conformance report: `build/treatcode-plan-evidence/P04/api-conformance.json`.
- Public-route E2E DOM assertions and screenshots: `build/treatcode-plan-evidence/P04/public-route-e2e.json` and `build/treatcode-plan-evidence/P04/browser-e2e.json` (PNG captures are listed there).
- Accessibility report: `build/treatcode-plan-evidence/P04/accessibility.json`.
- Bundle-size report: `build/treatcode-plan-evidence/P04/bundle-budget.json`.
- Product-owner visual approval.

## Completion Record

- **Verified commit:** `f9b8a199377919fa3dd5b771a10104d474c2d003`
- **Evidence artifact:** `build/treatcode-plan-evidence/P04/result.json`
- **Evidence hashes:** `result.json` content `sha256:0b99fe46546f3c3478f3cae715a4ec0ae691b8c87b369009eae89fb99bf57702`; `api-conformance.json` `sha256:b28bdb4c30bf59d37c590ed874623117301d2f0aa4dc7841dc759381380245a0`; `public-route-e2e.json` `sha256:ef1c8f9ce8c85d5bb3e6e270851bee1fe2d084d4ff501d9be0a5d8257883e95a`; `accessibility.json` `sha256:c5335e4e66496fbfe56c4a69fabf74eb49bfcd8b6e868374e13036e5b9a5d2a9`; `bundle-budget.json` `sha256:047d99f6234da4d499a848393bccee33304a888f44eab0d3eddac0090dbdf5c4`; `browser-e2e.json` `sha256:72116bd75684f1daaf1d0877a0552bc40f0b71b820be2708faaf659b531e4896`.
- **Human approvals:** Product owner — Codex verifier (acting owner) — approved
- **Date:** 2026-08-01T18:31:45.444262Z
