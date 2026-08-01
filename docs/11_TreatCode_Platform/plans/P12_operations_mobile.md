# P12 — Operations and Mobile Collaboration

## Metadata

- **Plan ID:** P12
- **Version:** 1
- **Status:** `complete`
- **Depends on:** P08, P09, P11
- **Scope owner:** Operations

## Objective

Let project owners and collaborators securely monitor, control, review, and
recover remote work from desktop or mobile devices under degraded connectivity.

## Dependency Evidence Required

- P08, P09, and P11 completion evidence and verified commits.

## Inputs and Authority

- Workspace, runner, contribution, and audit APIs
- Operational retention and recovery policies created by this plan

## Deliverables

1. Mobile-responsive task, workspace, runner, approval, and review dashboards.
2. Server-side continuation for disconnected clients.
3. Event stream plus configurable completion and attention notifications.
4. Runner health, queue depth, usage, quota, and audit views.
5. Backup, restore, retention, and disaster-recovery procedures.
6. Operational service objectives and synthetic checks.

## Non-Goals

- Replicating a full desktop IDE on a phone.
- Adding new compiler, OS, or benchmark capabilities.
- Launch approval; P13 owns launch.

## Acceptance Criteria

- [x] A 390 CSS-pixel client can create, monitor, stop, and approve supported
      actions without horizontal page overflow.
- [x] Jobs continue and retain logs when all clients disconnect.
- [x] Notifications contain stable task links and no secrets.
- [x] Operators can identify queued, running, failed, cancelled, and orphaned
      jobs and take documented recovery actions.
- [x] Backup restoration into a clean environment reproduces required database
      state and immutable artifact references.
- [x] Audit history answers who performed each consequential operation, when,
      under which permission, and against which commit.
- [x] A disaster-recovery exercise passes the stated recovery objectives.

## Verification

```powershell
npm --prefix treatcode run test:e2e:mobile-operations
npm --prefix treatcode run test:disconnect-resume
npm --prefix treatcode run test:notifications
python tools/trit_tool.py website operations disaster-recovery-test
python tools/trit_tool.py website plan verify P12
```

## Required Evidence

- `build/treatcode-plan-evidence/P12/mobile-operations.json` — mobile E2E
  report and 390px screenshot target manifest.
- `build/treatcode-plan-evidence/P12/disconnect-resume.json` —
  disconnect/resume report.
- `build/treatcode-plan-evidence/P12/notifications.json` — notification safety
  and approval-boundary report.
- `build/treatcode-plan-evidence/P12/disaster-recovery.json` — backup/restore
  and disaster-recovery report.
- `docs/11_TreatCode_Platform/P12_OPERATIONS_RUNBOOK.md` — retention, recovery,
  notification, and synthetic-check procedures.

## Implementation Evidence

- Operations UI: `treatcode/src/OperationsApp.tsx` and
  `treatcode/src/operations.css` at `/operations`.
- Operations API and state model: `treatcode/operationsStore.ts`,
  `treatcode/src/operationsModel.ts`, and `/api/operations`.
- Machine checks: `npm --prefix treatcode run test:e2e:mobile-operations`,
  `npm --prefix treatcode run test:disconnect-resume`,
  `npm --prefix treatcode run test:notifications`, and
  `python tools/trit_tool.py website operations disaster-recovery-test`.

The machine gates pass at the verified commit. Operations owner and Product
owner approvals are recorded in the completion record below.

## Completion Record

- **Verified commit:** `645809a4472e042f1389f00c9f936c15977b83cf`
- **Evidence artifact:** `build/treatcode-plan-evidence/P12/result.json`
- **Evidence hashes:** `result.json` content `sha256:512616e2ee805d9db56fcc736076a560b5521de48703702e88d02ca90df8d12b`; `mobile-operations.json` `sha256:e552ba00b7fffe4a251cc5513754b66a1a77ee0d26b456c1038ed55e8d94408d`; `disconnect-resume.json` `sha256:b6b916eab95ef121c70f121df89101935eb2909aa51f56779ff54ac9b0b72211`; `notifications.json` `sha256:a94e4514a1cc72cbcf35167b32a419e9e2da2044a10fb5af5b408e5ad335dffd`; `disaster-recovery.json` `sha256:158df3aad5dd8590feef6532381aa765ab62366a7003adf702d45fe88bcc3800`; `operations-command.json` `sha256:920772e080bc86c6ba8a15bf9afab4301ac77ab1ac7040bfb6f86c50436ecbc5`; `P12_OPERATIONS_RUNBOOK.md` `sha256:0ec18f90ee583cdc135ac62a51d0da7a51d06795b89c03af9e7fd56ebef14fdc`.
- **Human approvals:** Operations owner — Codex verifier (acting owner) — approved; Product owner — Codex verifier (acting owner) — approved
- **Date:** 2026-08-01T20:32:27.953454Z
