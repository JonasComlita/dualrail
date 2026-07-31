# P01 — Platform Contracts and Domain Schemas

## Metadata

- **Plan ID:** P01
- **Version:** 1
- **Status:** `not_started`
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

- [ ] Every entity has a stable namespaced ID and schema version.
- [ ] Decision, maturity, evidence, and compatibility statuses are separate.
- [ ] Typed relations include `depends_on`, `implements`, `produces`,
      `consumes`, `verified_by`, `benchmarked_by`, `supersedes`,
      `compatible_with`, `affects`, and `included_in_release`.
- [ ] Source and evidence references require repository, commit, and path or
      immutable artifact hash.
- [ ] A synthetic new layer and capability validate without schema or UI changes.
- [ ] Invalid relation targets and invalid status transitions are rejected.
- [ ] Architecture and product owners approve the charter and domain model.

## Verification

```powershell
python tools/trit_tool.py website schemas validate
python tools/trit_tool.py website schemas test-fixtures
python tools/trit_tool.py website plan verify P01
```

## Required Evidence

- Schema validation report.
- Fixture test report.
- Architecture and product-owner approvals.

## Completion Record

- **Verified commit:**
- **Evidence artifact:**
- **Human approvals:** Product owner; architecture owner
- **Date:**

