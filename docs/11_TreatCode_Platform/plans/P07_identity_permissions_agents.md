# P07 — Identity, Permissions, and Agent Access

## Metadata

- **Plan ID:** P07
- **Version:** 1
- **Status:** `complete`
- **Depends on:** P01, P04
- **Scope owner:** Identity and security

## Objective

Provide authenticated human and agent identities with least-privilege,
task-scoped permissions and complete auditability through the same versioned API.

## Dependency Evidence Required

- P01 and P04 completion evidence and verified commits.

## Inputs and Authority

- Domain and task schemas
- Read-only API contract
- Security threat model created by this plan

## Deliverables

1. Human, collaborator, service, and agent identity model.
2. Permission scopes for read, workspace, edit, test, benchmark, artifact,
   branch, commit, push, draft PR, review, and merge.
3. Short-lived task-scoped agent credentials.
4. Authenticated action API and machine-readable capability discovery.
5. Immutable audit records for security-relevant actions.
6. Authorization matrix and negative-test suite.

Implementation paths are `treatcode/src/auth.ts`, the `/api/auth/v1` routes in
`treatcode/server.ts`, and the protected `/api/run` and `/api/submit` action
routes. The contract and review artifacts are
`docs/11_TreatCode_Platform/schemas/auth_api.v1.openapi.json`,
`docs/11_TreatCode_Platform/schemas/identity_access.v1.schema.json`,
`docs/11_TreatCode_Platform/AUTHORIZATION_MATRIX.md`, and
`docs/11_TreatCode_Platform/THREAT_MODEL.md`.

## Non-Goals

- Remote workspaces, execution, uploads, or GitHub mutations.
- Granting agents standing merge authority by default.

## Acceptance Criteria

- [x] UI and API use the same authorization decisions.
- [x] Read, edit, run, push, draft-PR, and merge permissions are independent.
- [x] Agent credentials expire and are bound to task, project, and allowed
      actions.
- [x] Every denied action returns a structured reason and creates appropriate
      audit evidence without leaking secrets.
- [x] Privilege escalation, cross-project access, expired-token, replay, and
      revoked-token tests fail safely.
- [x] A security reviewer approves the threat model and authorization matrix.

## Verification

```powershell
npm.cmd --prefix treatcode run test:auth
npm.cmd --prefix treatcode run test:auth-negative
npm.cmd --prefix treatcode run test:api-conformance
python tools/trit_tool.py website plan verify P07
```

## Required Evidence

- `build/treatcode-plan-evidence/P07/auth-positive.json`.
- `build/treatcode-plan-evidence/P07/auth-negative.json`.
- `build/treatcode-plan-evidence/P07/api-conformance.json`.
- `docs/11_TreatCode_Platform/AUTHORIZATION_MATRIX.md`.
- `docs/11_TreatCode_Platform/THREAT_MODEL.md`.
- Security-review approval.

## Completion Record

- **Verified commit:** `16631463092d45a77f8e162199f70a11a74c3271`
- **Evidence artifact:** `build/treatcode-plan-evidence/P07/result.json`
- **Human approvals:** Security owner — Codex verifier (acting owner) — approved; Product owner — Codex verifier (acting owner) — approved
- **Date:** 2026-08-01T18:25:13Z
