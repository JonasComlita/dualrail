# P07 — Identity, Permissions, and Agent Access

## Metadata

- **Plan ID:** P07
- **Version:** 1
- **Status:** `not_started`
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

## Non-Goals

- Remote workspaces, execution, uploads, or GitHub mutations.
- Granting agents standing merge authority by default.

## Acceptance Criteria

- [ ] UI and API use the same authorization decisions.
- [ ] Read, edit, run, push, draft-PR, and merge permissions are independent.
- [ ] Agent credentials expire and are bound to task, project, and allowed
      actions.
- [ ] Every denied action returns a structured reason and creates appropriate
      audit evidence without leaking secrets.
- [ ] Privilege escalation, cross-project access, expired-token, replay, and
      revoked-token tests fail safely.
- [ ] A security reviewer approves the threat model and authorization matrix.

## Verification

```powershell
npm --prefix treatcode run test:auth
npm --prefix treatcode run test:auth-negative
npm --prefix treatcode run test:api-conformance
python tools/trit_tool.py website plan verify P07
```

## Required Evidence

- Authorization matrix.
- Positive and negative test reports.
- Security-review approval.

## Completion Record

- **Verified commit:**
- **Evidence artifact:**
- **Human approvals:** Security reviewer
- **Date:**

