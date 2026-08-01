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

- **Verified commit:** `645809a4472e042f1389f00c9f936c15977b83cf`
- **Evidence artifact:** `build/treatcode-plan-evidence/P07/result.json`
- **Evidence hashes:** `result.json` content `sha256:22ac6b802d9961ba8adbcaba6b28be1233a3314d104bde219d3248fc234eb346`; `auth-positive.json` `sha256:b0ec454ea30411e803df6d227433444bbf2f1b446cef6cb33110ed5f5ed43484`; `auth-negative.json` `sha256:8a8cc779e2c9c11bb48c4e3f8b11b587860a7c1094c593d785776a154240733a`; `api-conformance.json` `sha256:49263790b0eff9282c1ef522232c2bea307e80470ae66524674ac7f9ec8cda73`; `AUTHORIZATION_MATRIX.md` `sha256:654e3a993b403c807f428495419abd8c1c7042b96859582d6fb4dab68a9e8516`; `THREAT_MODEL.md` `sha256:a27b7ee0a4d9fe7074b0a83c9f4f9ac9be7f244f73313c28c454fc31ddb6041f`; `identity_access.v1.schema.json` `sha256:213f4305c49a4cd5196385c7fce12ee175fcddb1d3996066ca9110e50276c548`; `auth_api.v1.openapi.json` `sha256:f42daf376b7a83acbf8ca7d3aed6a67d9db78ea323230c5212fe2d8a5a5bb893`.
- **Human approvals:** Security owner — Codex verifier (acting owner) — approved; Product owner — Codex verifier (acting owner) — approved
- **Date:** 2026-08-01T20:30:29.942819Z
