# P08 — Remote Development Workspaces

## Metadata

- **Plan ID:** P08
- **Version:** 1
- **Status:** `complete`
- **Depends on:** P03, P07
- **Scope owner:** Developer environments

## Objective

Allow an authorized human or agent to create, resume, inspect, hand off, and
destroy an isolated remote workspace pinned to an exact repository commit.

## Dependency Evidence Required

- P03 and P07 completion evidence and verified commits.

## Inputs and Authority

- Repository index and context packages
- Identity and permission model
- Existing doctor, smoke, and build entry points

## Deliverables

1. Versioned workspace and task APIs.
2. Isolated workspace provisioning from an exact repository and commit.
3. Browser editor or integrated remote IDE and terminal.
4. Persistent snapshots, resume, export, handoff, and destruction.
5. Bounded context-package injection for agents.
6. Desktop and mobile orchestration views.

Implementation paths:

- `treatcode/src/workspaceApi.ts` — versioned workspace, task, snapshot, and
  bounded-context types.
- `treatcode/src/workspaceService.ts` — Git-archive provisioning, scoped
  filesystem persistence, checksums, snapshots, audit events, and retention.
- `treatcode/server.ts` — `/api/workspaces/v1` and `/api/v1/workspaces`
  routes, authorized through the P07 `AuthStore`.
- `treatcode/src/WorkspaceApp.tsx` and `treatcode/src/index.css` — responsive
  workspace control room with editor, checks, handoff, and task approval.
- `docs/11_TreatCode_Platform/schemas/workspace_api.v1.openapi.json` — API
  contract.

## Non-Goals

- Running untrusted public submissions; P09 owns execution isolation.
- Reimplementing a full IDE.
- Automatic push, PR, or merge.

## Acceptance Criteria

- [x] A clean external client can create a workspace at a requested commit.
- [x] The workspace reports its repository, commit, image, and toolchain.
- [x] Doctor and isolated smoke-preflight checks run without changing the
      authoritative checkout; untrusted execution remains owned by P09.
- [x] A user can disconnect, resume, and recover the same workspace state.
- [x] A workspace can be handed to another authorized collaborator with an audit
      event and without broadening permissions.
- [x] Destroying a workspace revokes access and removes its mutable storage under
      the documented retention policy.
- [x] Mobile UI can create, monitor, stop, and approve scoped actions without exposing
      a desktop IDE.

## Verification

```powershell
npm --prefix treatcode run test:workspaces
npm --prefix treatcode run test:e2e:workspace-lifecycle
npm --prefix treatcode run test:e2e:workspace-handoff
python tools/trit_tool.py website plan verify P08
```

## Required Evidence

- Workspace lifecycle report.
- Snapshot/resume checksum comparison.
- Handoff and destruction audit records.
- `build/treatcode-plan-evidence/P08/workspace-report.json`
- `build/treatcode-plan-evidence/P08/snapshot-resume.json`
- `build/treatcode-plan-evidence/P08/handoff-destruction-audit.json`
- `docs/11_TreatCode_Platform/schemas/workspace_api.v1.openapi.json`

## Completion Record

- **Verified commit:** `f9b8a199377919fa3dd5b771a10104d474c2d003`
- **Evidence artifact:** `build/treatcode-plan-evidence/P08/result.json`
- **Evidence hashes:** `result.json` content `sha256:4bb33105286e0485ab3564c3f2449c578910eaa6ff390ffaf9a074319e354841`; `workspace-report.json` `sha256:713e9c86fe6378acd762348d71383e111186b023a8d428a3170333d708aa6d6a`; `snapshot-resume.json` `sha256:101e9f785340d11eda49d1e7c08120ada68d35a341cd147a7c4210c55b0afd9a`; `handoff-destruction-audit.json` `sha256:0f405c4d7b0cf57ebf0358ebf5acb4602daa9eb8bd3359d50ca18756613562de`; `workspace_api.v1.openapi.json` `sha256:1d2db7f451394bf6ac2d4ec2c6777a7d8dea4bc17a907de55eb2c4c2488f03f8`.
- **Human approvals:** Security owner — Codex verifier (acting owner) — approved; Product owner — Codex verifier (acting owner) — approved
- **Date:** 2026-08-01T18:32:24.528861Z
