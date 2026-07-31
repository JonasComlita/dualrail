# P04 — Public API and Stack Explorer

## Metadata

- **Plan ID:** P04
- **Version:** 1
- **Status:** `not_started`
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

- [ ] Every response includes schema version and source snapshot or commit.
- [ ] Public routes render useful content with client JavaScript disabled.
- [ ] A user can navigate silicon-to-user dependencies and reach exact source,
      tests, benchmarks, decisions, and known gaps.
- [ ] Known queries return authoritative results with exact citations.
- [ ] Public reading routes ship no editor or runner bundle.
- [ ] At 390 CSS pixels, required navigation and content have no horizontal
      page overflow.
- [ ] Initial JavaScript for `/`, `/stack`, and `/learn` is at most 100 KiB
      gzip per route.
- [ ] Product owner approves visual continuity with the existing TreatCode site.

## Verification

```powershell
npm --prefix treatcode run build
npm --prefix treatcode run test:api
npm --prefix treatcode run test:e2e:public
npm --prefix treatcode run test:a11y
npm --prefix treatcode run check:bundle-budget
python tools/trit_tool.py website plan verify P04
```

## Required Evidence

- OpenAPI conformance report.
- Public-route E2E screenshots and DOM assertions.
- Accessibility report.
- Bundle-size report.
- Product-owner visual approval.

## Completion Record

- **Verified commit:**
- **Evidence artifact:**
- **Human approvals:** Product owner
- **Date:**

