# TreatCode P13 Launch Readiness Matrix

## Decision state

**Current decision: GO.** The matrix is a release-control artifact, not a
marketing checklist. A row is launch-ready only when its authoritative source,
contract, test result, immutable evidence, and required human approval agree at
the same launch candidate commit.

The machine-readable companion is
`build/treatcode-plan-evidence/P13/launch-readiness.json`.

## Bounded public-claim inventory

The inventory is bounded by the public routes and API in
`treatcode/src/PublicApp.tsx`, the practice surface in `treatcode/src/App.tsx`,
the versioned snapshot under `treatcode/public/api/v1/`, and the acceptance
criteria of P04-P12. New public claims require a new row and a new evidence
reference before release.

| ID | Claim or user journey | Authoritative source and contract | Required evidence gate | Current release state |
|---|---|---|---|---|
| LR-01 | A reader can inspect the Trit stack, dependencies, source, tests, benchmarks, decisions, releases, and gaps. | `treatcode/src/PublicApp.tsx`; `docs/11_TreatCode_Platform/schemas/public_api.v1.openapi.json`; root registries. | P04 result, API conformance, public E2E, accessibility, responsive, and bundle reports. | READY at the verified candidate commit. |
| LR-02 | Public statistics and status labels describe repository-backed data rather than seeded demo values. | `treatcode/scripts/generate-public-snapshot.mjs`; generated snapshot; `CAPABILITY_MANIFEST.json`; `CONTRACT_MANIFEST.json`; `DECISION_MANIFEST.json`. | Snapshot count/provenance check and launch test; no seeded leaderboard or fallback statistic. | READY; machine and launch checks pass. |
| LR-03 | A learner can follow ordered, prerequisite-aware, source-linked learning content. | `treatcode/src/content/learn/learning-catalog.json`; Markdown pages; P05 rubric. | P05 content, learning-flow, accessibility, and human review evidence. | READY at the verified candidate commit. |
| LR-04 | Published challenges have correctness contracts and reproducible validation. | P06 challenge catalog and correctness contracts. | P06 validation and challenge E2E evidence. | READY at the verified candidate commit. |
| LR-05 | Human and agent identities receive least-privilege access with auditable authorization. | P07 identity, permission, and API contracts. | P07 positive, negative, conformance, and security-review evidence. | READY at the verified candidate commit. |
| LR-06 | An authorized collaborator can create, resume, hand off, and destroy an isolated remote workspace. | P08 workspace lifecycle and handoff contracts. | P08 lifecycle, handoff, disconnect, and audit evidence. | READY at the verified candidate commit. |
| LR-07 | Public code execution is isolated, cancellable, resource-bounded, and evidence-producing. | P09 runner and immutable-evidence protocol. | P09 isolation, adversarial, deterministic-rerun, and security-review evidence. | READY at the verified candidate commit. |
| LR-08 | Benchmark comparisons use controlled workloads and reproducible runner profiles. | P10 benchmark protocol and reference verification. | P10 benchmark and implementation-arena evidence. | READY at the verified candidate commit. |
| LR-09 | Uploads and remote contributions remain quarantined until validation and explicit review. | P11 upload, provenance, and GitHub review boundary. | P11 adversarial upload and draft-PR evidence with human approval. | READY at the verified candidate commit. |
| LR-10 | Desktop and 390 CSS-pixel mobile operators can monitor, recover, and approve supported work. | P12 operations, notification, backup, and restore procedures. | P12 mobile, disconnect/resume, notification, and disaster-recovery evidence. | READY at the verified candidate commit. |
| LR-11 | The release can be deployed, rolled back, supported, and investigated without mutating authoritative source. | `DEPLOYMENT_ROLLBACK_INCIDENT_SUPPORT.md`; `POLICIES.md`; `RELEASE_RECORD.md`. | P13 full verification bundle, operational review, and release-authority decision. | GO at the verified candidate commit. |

## Evidence rules

- The plan manifest and its completion records are the dependency authority.
- A passing command without its required artifact, commit, hash, or human gate
  is not launch evidence.
- `planned`, `partial`, `missing`, `experimental`, and `not_available` remain
  visible labels; they are never silently promoted to `released`.
- Evidence generated from a dirty checkout is diagnostic only until the release
  candidate commit is recorded and the artifact hashes are preserved.
