# P06 — Challenges and Faceted Taxonomy

## Metadata

- **Plan ID:** P06
- **Version:** 1
- **Status:** `not_started`
- **Depends on:** P01, P04
- **Scope owner:** Challenges

## Objective

Move challenges into one validated source of truth and classify them through
orthogonal domain, technique, data-model, stack-layer, target, and objective
facets.

## Dependency Evidence Required

- P01 and P04 completion evidence and verified commits.

## Inputs and Authority

- Existing frontend and server problem catalogs
- TCL compiler and VM tests
- Stack and capability registries

## Deliverables

1. `CHALLENGE_MANIFEST.json` and versioned schema.
2. Generated client and server challenge data from the manifest.
3. Published, draft, and retired lifecycle states.
4. Faceted browse and search UI preserving the existing challenge experience.
5. Full correctness fixtures and hidden-test contracts for published challenges.
6. Pilot tritwise/word-parallel and numerical/vector challenges.

## Non-Goals

- Building secure public execution; P09 owns execution.
- Publishing compile-only placeholders as verified problems.
- Creating a top-level navigation silo for every technique.

## Acceptance Criteria

- [ ] Frontend and server contain no independent duplicate challenge catalogs.
- [ ] Every published challenge has deterministic correctness tests and limits.
- [ ] Compile-only challenges are draft and excluded from verified completion
      statistics and benchmark leaderboards.
- [ ] Facets cover domain, technique, data model, stack layer, target, and
      optimization objective.
- [ ] At least one pilot demonstrates representation-dependent tritwise cost.
- [ ] At least one pilot covers vector dot product or matrix multiplication.
- [ ] Existing challenge navigation and editor user journeys remain functional.

## Verification

```powershell
python tools/trit_tool.py website challenges validate
npm --prefix treatcode run test:challenges
npm --prefix treatcode run test:e2e:challenges
python tools/trit_tool.py website plan verify P06
```

## Required Evidence

- Manifest-validation report.
- Published-challenge correctness report.
- Challenge E2E report.

## Completion Record

- **Verified commit:**
- **Evidence artifact:**
- **Human approvals:** Challenge curator
- **Date:**

