# P08 — Remote Development Workspaces

## Metadata

- **Plan ID:** P08
- **Version:** 1
- **Status:** `not_started`
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

## Non-Goals

- Running untrusted public submissions; P09 owns execution isolation.
- Reimplementing a full IDE.
- Automatic push, PR, or merge.

## Acceptance Criteria

- [ ] A clean external client can create a workspace at a requested commit.
- [ ] The workspace reports its repository, commit, image, and toolchain.
- [ ] Doctor and smoke commands run without changing the authoritative checkout.
- [ ] A user can disconnect, resume, and recover the same workspace state.
- [ ] A workspace can be handed to another authorized collaborator with an audit
      event and without broadening permissions.
- [ ] Destroying a workspace revokes access and removes its mutable storage under
      the documented retention policy.
- [ ] Mobile UI can create, monitor, stop, and approve actions without exposing
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

## Completion Record

- **Verified commit:**
- **Evidence artifact:**
- **Human approvals:** Developer-experience owner
- **Date:**

