# TreatCode P07 Threat Model

## Boundary

TreatCode serves public, read-only stack knowledge beside authenticated
workspace and execution actions. The browser is an untrusted client. Agents,
services, collaborators, and humans are distinct principals, but all requests
cross the same HTTP trust boundary. The authoritative repository and audit
record are protected resources.

## Assets

- Project source, task workspaces, branches, commits, and artifacts.
- Execution capacity and benchmark results.
- Draft pull requests, reviews, and merge authority.
- Identity credentials and task scope.
- Audit evidence needed to explain an allowed or denied action.

## Threats and controls

| Threat | Control | Verification |
|---|---|---|
| Credential theft or accidental disclosure | Opaque random tokens, hash-only storage, no token logging, `no-store` issuance responses | Positive auth report checks audit and response bodies. |
| Privilege escalation | Exact action membership; no hierarchy between read/edit/run/push/draft-PR/merge | Negative authorization suite. |
| Cross-project or cross-task access | Credential carries exact project/task and every request supplies a scope | Cross-scope negative cases. |
| Expired credential use | Expiry checked before action grant; maximum task lifetime is bounded | Clock-controlled expiry case. |
| Revoked credential use | Revocation state checked on every request | Revocation case. |
| Replay of a mutating request | Required one-time action nonce, stored as a hash per credential | Replay case. |
| Audit tampering or missing denial evidence | Append-only JSONL hash chain; denial fails closed if the sink is unavailable | Audit-chain and denial assertions. |
| Identity enumeration | Login returns the same safe invalid-credentials reason for unknown, inactive, or wrong keys | Invalid-login case. |
| UI-only authorization bypass | UI reads the capability endpoint, but API routes call the same `AuthStore.authorize` decision | API conformance and protected action cases. |
| Standing agent merge authority | Agents cannot interactively log in; task issuance intersects requested actions with agent policy and the default policy excludes merge | Task-credential negative cases. |

## Residual risks

The current implementation uses an in-process identity registry and a local
append-only audit file. Deployment must replace those with a managed identity
provider and a write-once or externally replicated audit sink before accepting
production credentials. The policy contract and denial semantics remain the
same across that migration.

## Review gate

Security owner review must confirm the matrix, threat controls, and negative
test report before P07 is marked complete. Product owner review must confirm
that sign-in and capability discovery are understandable in the TreatCode UI.
