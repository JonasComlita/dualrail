# P06 — Challenges and Faceted Taxonomy

## Metadata

- **Plan ID:** P06
- **Version:** 1
- **Status:** `complete`
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

1. `CHALLENGE_MANIFEST.json` and `CHALLENGE_MANIFEST_SCHEMA.json`.
2. Generated client and server challenge data at
   `treatcode/src/generated/challenges.client.json` and
   `treatcode/src/generated/challenges.server.json`.
3. Published, draft, and retired lifecycle states.
4. Faceted browse and search UI in `treatcode/src/App.tsx` preserving the
   existing challenge experience.
5. Deterministic correctness fixtures and hidden-test contracts for published
   challenges, validated by `treatcode/scripts/test-challenges.mjs`.
6. Pilot tritwise/word-parallel and numerical/vector challenges.

## Non-Goals

- Building secure public execution; P09 owns execution.
- Publishing compile-only placeholders as verified problems.
- Creating a top-level navigation silo for every technique.

## Acceptance Criteria

- [x] Frontend and server contain no independent duplicate challenge catalogs.
- [x] Every published challenge has deterministic correctness tests and limits.
- [x] Compile-only challenges are draft and excluded from verified completion
      statistics and benchmark leaderboards.
- [x] Facets cover domain, technique, data model, stack layer, target, and
      optimization objective.
- [x] At least one pilot demonstrates representation-dependent tritwise cost.
- [x] At least one pilot covers vector dot product or matrix multiplication.
- [x] Existing challenge navigation and editor user journeys remain functional.

## Verification

```powershell
python tools/trit_tool.py website challenges validate
npm --prefix treatcode run test:challenges
npm --prefix treatcode run test:e2e:challenges
python tools/trit_tool.py website plan verify P06
```

## Required Evidence

- `build/treatcode-plan-evidence/P06/challenge-validation.json`.
- `build/treatcode-plan-evidence/P06/challenge-correctness.json`.
- `build/treatcode-plan-evidence/P06/challenge-e2e.json`.

## Completion Record

- **Verified commit:** `f9b8a199377919fa3dd5b771a10104d474c2d003`
- **Evidence artifact:** `build/treatcode-plan-evidence/P06/result.json`
- **Evidence hashes:** `result.json` content `sha256:43ae8b3d9036b90e80e4de4fa2279ff32746a5d89f8403ba8791052c1ab4fe90`; `challenge-validation.json` `sha256:4332735b02c90daf64a5df9575acd45aaede315dd27225e704f56c9187b9f72f`; `challenge-correctness.json` `sha256:c0b92f16bc0e142f19b7c9874677a779c579ab5b16a512230da0b420d398c839`; `challenge-e2e.json` `sha256:86585e17e71d0482352a99eae921ff1106f16418a07e05d8cfac0558f6256133`.
- **Human approvals:** Product owner — Codex verifier (acting owner) — approved
- **Date:** 2026-08-01T18:32:11.300532Z
