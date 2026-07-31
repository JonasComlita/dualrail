# P02 — Stack, Capability, and Decision Registry

## Metadata

- **Plan ID:** P02
- **Version:** 1
- **Status:** `not_started`
- **Depends on:** P01
- **Scope owner:** Architecture knowledge

## Objective

Represent the current Trit stack, known and missing capabilities, and canonical
architecture decisions as validated data rather than hard-coded website content.

## Dependency Evidence Required

- P01 completion evidence and verified commit.

## Inputs and Authority

- `docs/STACK_REVIEW_ORDER.md`
- `ROADMAP_STATUS.json`
- `KNOWN_GAPS.md`
- Root manifests
- Accepted architecture specifications
- Historical OS3 proposals as non-authoritative inputs

## Deliverables

1. `STACK_MANIFEST.json`.
2. `CAPABILITY_MANIFEST.json`.
3. `CONTRACT_MANIFEST.json`.
4. `DECISION_MANIFEST.json`.
5. A coverage report connecting each stack node to source, contracts, tests,
   benchmarks, gaps, and releases where available.

## Non-Goals

- Implementing capabilities such as encrypted volumes or TASCII-81.
- Treating historical pseudocode as current production truth.
- Building the public explorer.

## Acceptance Criteria

- [ ] All 21 dependency phases have stable IDs and ordered dependency edges.
- [ ] Every referenced repository path is resolved or explicitly marked missing.
- [ ] ASCII/UTF-8/hex compatibility and ternary-native encodings are separate
      capability records.
- [ ] Encrypted-volume compatibility and a possible ternary-native design are
      separate capability records with cross-layer dependencies.
- [ ] Known OS3 contract conflicts have accepted, superseded, rejected, or open
      decision records; none are silently presented as simultaneous truth.
- [ ] Adding a synthetic capability to any layer requires data changes only.
- [ ] The coverage report distinguishes specified, implemented, integrated,
      tested, benchmarked, and released.

## Verification

```powershell
python tools/trit_tool.py website registry validate
python tools/trit_tool.py website registry coverage --strict
python tools/trit_tool.py website plan verify P02
```

## Required Evidence

- Registry validation report.
- Coverage report.
- Architecture-owner decision-ledger approval.

## Completion Record

- **Verified commit:**
- **Evidence artifact:**
- **Human approvals:** Architecture owner
- **Date:**

