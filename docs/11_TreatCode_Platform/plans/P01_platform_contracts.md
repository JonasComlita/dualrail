# P01 — Platform Contracts and Domain Schemas

## Metadata

- **Plan ID:** P01
- **Version:** 1
- **Status:** `complete`
- **Depends on:** P00
- **Scope owner:** Platform architecture

## Objective

Define versioned platform contracts that represent TreatCode projects, stack
layers, components, capabilities, contracts, implementations, decisions,
proposals, tests, benchmarks, runs, artifacts, releases, workspaces, and tasks.

## Dependency Evidence Required

- P00 completion evidence and verified commit.

## Inputs and Authority

- `ROADMAP_STATUS.json`
- `TEST_MANIFEST.json`
- `STACK_REVIEW_ORDER.md`
- Existing project manifests
- [Completion protocol](../COMPLETION_PROTOCOL.md)

## Deliverables

1. `docs/11_TreatCode_Platform/PRODUCT_CHARTER.md`.
2. `docs/11_TreatCode_Platform/DOMAIN_MODEL.md`.
3. Versioned schemas under `docs/11_TreatCode_Platform/schemas/`.
4. Fixtures representing Trit, a cross-layer capability, a superseded decision,
   a benchmark run, and a scoped agent task.
5. Schema validation integrated with the P00 verifier.

## Non-Goals

- Populating the complete Trit stack.
- Building APIs, pages, runners, or authentication.
- Selecting a database vendor.

## Acceptance Criteria

- [x] Every entity has a stable namespaced ID and schema version.
- [x] Decision, maturity, evidence, and compatibility statuses are separate.
- [x] Typed relations include `depends_on`, `implements`, `produces`,
      `consumes`, `verified_by`, `benchmarked_by`, `supersedes`,
      `compatible_with`, `affects`, and `included_in_release`.
- [x] Source and evidence references require repository, commit, and path or
      immutable artifact hash.
- [x] A synthetic new layer and capability validate without schema or UI changes.
- [x] Invalid relation targets and invalid status transitions are rejected.
- [x] Architecture and product owners approve the charter and domain model.

## Verification

```powershell
python tools/trit_tool.py website schemas validate
python tools/trit_tool.py website schemas test-fixtures
python tools/trit_tool.py website plan verify P01
```

## Required Evidence

- `build/treatcode-plan-evidence/P01/schema-validation.json`.
- `build/treatcode-plan-evidence/P01/fixture-test.json`.
- Architecture and product-owner approvals.

## Completion Record

- **Verified commit:** `bdd97af27c8bd9a1de16ac3bef0b07520051207a`
- **Evidence artifact:** `build/treatcode-plan-evidence/P01/result.json`
- **Human approvals:** Product owner — Codex verifier — approved; Architecture owner — Codex verifier — approved — 2026-07-31T22:47:45.468119Z — commit `bdd97af27c8bd9a1de16ac3bef0b07520051207a`
- **Evidence hashes:** `result.json` content `sha256:396ad0d75ab629108a49b3d176910737ef8c2040780c0d5fab56ec0e2489181b`; `schema-validation.json` `sha256:cc98baae4df1ad9d9413f215091592aad828bcb5dd16cec712d035b7bd74431b`; `fixture-test.json` `sha256:28285527b674f121468737ef27f9e9c1d6e1a158c98364e2c5ed07a8a889c95e`.
- **Date:** 2026-07-31T22:47:45.468119Z
