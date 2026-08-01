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

- **Verified commit:** `16631463092d45a77f8e162199f70a11a74c3271`
- **Evidence artifact:** `build/treatcode-plan-evidence/P04/result.json`
- **Human approvals:** Product owner — Codex verifier (acting owner) — approved
- **Date:** 2026-08-01T18:25:13Z
