# P02 — Stack, Capability, and Decision Registry

## Metadata

- **Plan ID:** P02
- **Version:** 1
- **Status:** `complete`
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

- [x] All 21 dependency phases have stable IDs and ordered dependency edges.
- [x] Every referenced repository path is resolved or explicitly marked missing.
- [x] ASCII/UTF-8/hex compatibility and ternary-native encodings are separate
      capability records.
- [x] Encrypted-volume compatibility and a possible ternary-native design are
      separate capability records with cross-layer dependencies.
- [x] Known OS3 contract conflicts have accepted, superseded, rejected, or open
      decision records; none are silently presented as simultaneous truth.
- [x] Adding a synthetic capability to any layer requires data changes only.
- [x] The coverage report distinguishes specified, implemented, integrated,
      tested, benchmarked, and released.

## Verification

```powershell
python tools/trit_tool.py website registry validate
python tools/trit_tool.py website registry coverage --strict
python tools/trit_tool.py website plan verify P02
```

## Required Evidence

- `build/treatcode-plan-evidence/P02/registry-validation.json`.
- `build/treatcode-plan-evidence/P02/registry-coverage.json`.
- `build/treatcode-plan-evidence/P02/result.json`.
- Architecture-owner decision-ledger approval.

## Completion Record

- **Verified commit:** `d168bc845babad7d6031bed98a16e3a471d200c5`
- **Evidence artifact:** `build/treatcode-plan-evidence/P02/result.json`
- **Human approvals:** Architecture owner — Codex verifier — approved — 2026-07-31T22:28:44Z — commit `d168bc845babad7d6031bed98a16e3a471d200c5`
- **Evidence hashes:** `result.json` content `sha256:3f2ec56c00a532cc5b40a5d761af529d3d7d9539d7e9e60743781bc163455af0`; `registry-validation.json` `sha256:edc539a02957324c5c95b8ddf84e3a280e167ee7b313a75b460d8eb1fc6d4288`; `registry-coverage.json` `sha256:4d19b13cb047967bc2d57a8a9d45047c5454c1cf77eefbee4b97eec0ecce7d70`.
- **Date:** 2026-07-31T22:28:44Z
