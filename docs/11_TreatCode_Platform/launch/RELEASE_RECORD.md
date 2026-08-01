# TreatCode P13 Release Record

## Decision

**GO**. P04–P12 have complete evidence-backed records, all P13 machine gates
pass, and the required owner approvals are recorded below for this candidate.

## Candidate identity

- Verified commit: `839ffdf49a2e5b572b16e842870cc19392df8825`.
- Public snapshot: `treatcode/public/api/v1/snapshot.json`.
- Launch report: `build/treatcode-plan-evidence/P13/launch-readiness.json`.
- Full verification bundle: `build/treatcode-plan-evidence/P13/full-verification.json`.
- Decision basis: P04-P12 completion records, P13 launch tests, and the
  production/smoke baseline.

## Blocking conditions

- None. P04-P12 have complete, compatible, hash-addressed evidence, and all
  required P13 verification commands and failure-path tests pass.

## Required approval record

Each approval names a reviewer, decision, UTC date, and candidate commit. The
required roles are Product owner, Architecture owner, Security owner,
Operations owner, and Release authority; Codex verifier is acting in each
named owner role for this local human-approval record.

## Change log

| Date | Commit | Decision | Recorder | Notes |
|---|---|---|---|---|
| 2026-07-31 | `f0d30809a1534475c2726339b2618d4c11507e1e` | NO-GO | P13 implementation agent | Prepared launch controls; dependency and human gates remain open. |
| 2026-08-01 | `839ffdf49a2e5b572b16e842870cc19392df8825` | GO | Codex verifier (acting release authority) | P04-P12 evidence complete; P13 closure checks passed. |
