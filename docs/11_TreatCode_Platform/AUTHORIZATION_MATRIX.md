# TreatCode P07 Authorization Matrix

P07 uses one policy decision function for browser actions and API requests.
The UI may discover capabilities and choose which controls to enable, but the
server always evaluates the presented credential again.

## Identity classes

| Identity | Purpose | Default authority |
|---|---|---|
| Human | Account holder and task owner | Project-scoped grants issued by policy; merge is never implied. |
| Collaborator | Human collaborator on an approved project | Explicit read/workspace/edit/test/benchmark/artifact/branch/commit/draft-PR/review grants. |
| Service | Non-human build or indexing service | Explicit read/test/benchmark/artifact grants only. |
| Agent | Automated task worker | No interactive standing session; receives a short-lived task credential. |

Identity names and display labels are not authorization. Stable IDs, project
scope, task scope, action scope, status, and credential expiry are.

## Independent actions

| Action | Allows | Does not imply |
|---|---|---|
| `read` | Read an authorized project surface | Any write or execution action |
| `workspace` | Create or inspect a scoped workspace or task credential | `edit`, `branch`, or `merge` |
| `edit` | Change files inside the authorized workspace | `commit`, `push`, or `merge` |
| `test` | Run a bounded test/compiler action | `edit`, `benchmark`, or `push` |
| `benchmark` | Run or record benchmark work | `test`, `artifact`, or `merge` |
| `artifact` | Create or inspect a task artifact | `push` or `merge` |
| `branch` | Create or update a branch reference | `commit`, `push`, or `merge` |
| `commit` | Record a commit in the scoped workspace | `push` or `merge` |
| `push` | Push an already reviewed branch | `merge` |
| `draft_pr` | Create or update a draft pull request | `review` or `merge` |
| `review` | Read or record review evidence | `merge` |
| `merge` | Merge an approved change | Any other action |

Mutating actions require a fresh `X-Action-Nonce` (or `Idempotency-Key`). A
nonce is consumed only after every scope check passes, and a reused nonce is a
structured `replay_detected` denial.

## Credential contract

Task credentials contain the identity, exact project, exact task, allowed
actions, issue time, and expiry. The server stores only a hash of the opaque
token. Credentials expire within the configured maximum lifetime and can be
revoked independently. The raw token is never placed in audit records,
responses to later reads, or logs.

## Denial contract

Every denied authenticated action returns `treatcode.auth.api.v1` with an
`error` containing `code`, `reason`, `action`, project/task scope,
`audit_event_id`, and `retryable`. The reason is safe for a client to display;
credential values, access keys, and source contents are never included.

## Policy examples

| Request | Result |
|---|---|
| Agent task token asks to `read` its own project/task | Allowed if `read` was delegated. |
| Agent task token asks to `push` when only `test` was delegated | Denied with `action_not_granted`. |
| Valid task token names another project or task | Denied with a scope reason. |
| Expired or revoked token is presented | Denied with a token-state reason. |
| Same mutating nonce is submitted twice | First request may pass; second is `replay_detected`. |
