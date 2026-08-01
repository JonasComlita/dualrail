# P12 Operations Runbook

This runbook is the operational authority for the TreatCode operations control
plane. It covers the `/operations` dashboard and `/api/operations` service. The
console is intentionally smaller than a desktop IDE: it is for task control,
workspace and runner health, approvals, reviews, notifications, audit, and
recovery.

## Service objectives

| Objective | Target | Synthetic check |
| --- | ---: | --- |
| Recovery point objective (RPO) | 15 minutes | `ops-recovery` |
| Recovery time objective (RTO) | 30 minutes | `ops-recovery` |
| Operations API health | fresh runner heartbeat | `ops-api` |
| Disconnected task continuation | retained cursor and logs | `ops-task` |

Jobs are server-owned after they are accepted. A client disconnect does not
cancel a task, release its runner, or discard its logs. Clients reconnect through
`POST /api/operations/sessions/:clientId/resume` and receive the same task ID,
log cursor, stable task link, and audit trail.

## Operator actions

1. Check `/api/operations/health` and the runner heartbeat before changing a
   task.
2. Use the task board to distinguish `queued`, `running`, `failed`,
   `cancelled`, and `orphaned` work. A failed task can be retried; an orphaned
   task should be recovered only after its runner heartbeat and workspace state
   are understood.
3. Use the approval panel for consequential operations. An approval is tied to
   a permission, exact commit, requester, and expiry; starting an unapproved
   task returns `APPROVAL_REQUIRED`.
4. Review `/api/operations/audit` after stop, retry, recover, approval, backup,
   restore, or notification-preference actions.
5. Use the recovery panel to run a clean restore exercise. Do not overwrite an
   authoritative checkout during a restore test.

## Retention and backup

- Task logs are retained for 30 days.
- Audit events are retained for 365 days.
- Mutable workspace storage is retained for 14 days after destruction or
  replacement, subject to the owning workspace policy.
- Result, log, trace, and diagnostic references are immutable and content
  addressed. A backup records the database digest and every immutable artifact
  reference.
- The configured durable deployment path is `TREATCODE_OPERATIONS_STATE`.
  Local development defaults to an in-process store so a checkout does not gain
  an unreviewed mutable data file.

## Disaster-recovery exercise

Run:

```powershell
python tools/trit_tool.py website operations disaster-recovery-test
```

The exercise creates a versioned backup, verifies its database digest, restores
it into a clean store, compares the restored digest and immutable artifact
count, and checks the RPO/RTO policy. The machine-readable report is written to
`build/treatcode-plan-evidence/P12/disaster-recovery.json`.

If the exercise fails, preserve the report, stop destructive workspace cleanup,
check the latest backup and runner health, and page the Operations owner. A
restore is not complete until both the database digest and immutable artifact
references match.

## Notification policy

Notifications are in-app by default and may be configured for `email` or
`web_push`. Completion, attention, approval, and recovery notifications include
only a stable `/operations?task=...` or recovery link. They never contain
credentials, bearer tokens, passwords, private keys, or authorization values.
