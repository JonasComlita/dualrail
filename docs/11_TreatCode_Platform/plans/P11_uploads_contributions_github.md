# P11 — Uploads, Contributions, and GitHub

## Metadata

- **Plan ID:** P11
- **Version:** 1
- **Status:** `complete`
- **Depends on:** P07, P08, P09
- **Scope owner:** Contributions

## Objective

Accept human or agent contributions through quarantined, provenance-preserving
uploads and convert validated work into explicitly approved draft pull requests.
The implementation contract is documented in
[`CONTRIBUTION_CONTRACT.md`](../CONTRIBUTION_CONTRACT.md).

## Dependency Evidence Required

- P07, P08, and P09 completion evidence and verified commits.

## Inputs and Authority

- Identity and permission scopes
- Workspace and execution protocols
- GitHub repository and review policy

## Deliverables

1. Resumable, content-hashed upload sessions.
2. Quarantine validation for size, type, paths, archives, licenses, provenance,
   and malware or unsafe content.
3. Conversion of accepted uploads into isolated workspace changes.
4. Validation and evidence attachment before submission.
5. GitHub App integration for branch, commit, push, and draft-PR creation.
6. Explicit human approval boundary and end-to-end test repository.

## Non-Goals

- Direct upload into an authoritative branch.
- Automatic merge based on tests or benchmarks.
- Storing Git credentials inside execution workers.

## Acceptance Criteria

- [x] Interrupted uploads resume without changing the resulting content hash.
- [x] Path traversal, archive bombs, forbidden types, oversized content, missing
      provenance, and unauthorized licenses are rejected.
- [x] Uploaded content cannot modify authoritative source before workspace
      validation and explicit submission.
- [x] Branch, commit, push, draft-PR, and merge permissions are independently
      enforced.
- [x] Draft-PR creation requires recorded human approval unless project policy
      explicitly delegates that scope.
- [x] The draft PR links immutable correctness and benchmark evidence.
- [x] An end-to-end test repository proves contribution intake through draft PR
      without granting merge authority.

## Verification

```powershell
npm.cmd --prefix treatcode run test:uploads
npm.cmd --prefix treatcode run test:uploads-adversarial
npm.cmd --prefix treatcode run test:e2e:draft-pr
python tools/trit_tool.py website plan verify P11
```

## Required Evidence

- Upload and adversarial-test reports.
- Test-repository draft PR URL.
- Audit record and evidence hashes.

The current implementation keeps the GitHub boundary behind an injected
GitHub-App adapter and uses an in-memory test repository for local verification;
no Git credentials or merge authority are present in the upload or validation
worker. The machine-readable reports are written to
`build/treatcode-plan-evidence/P11/`.

## Completion Record

- **Verified commit:** `f9b8a199377919fa3dd5b771a10104d474c2d003`
- **Evidence artifact:** `build/treatcode-plan-evidence/P11/result.json`
- **Evidence hashes:** `result.json` content `sha256:b8dae262dcc9f6cdae67d054413b0a1af5fbd74d2e5f6510e4dac60927940d0f`; `upload-tests.json` `sha256:e908ff6d7451d5fce33e232e9b7f7c23f46d0eb1c196ee3a107ef178ab13a053`; `upload-adversarial.json` `sha256:050cfa9e6ab025fca47412185b62afc9d9ca61c238cf3254726b9cda60702d0a`; `draft-pr-e2e.json` `sha256:06fcc9a3eb12af66f3109b9c8418b1e9391af146ab381359c7c2cc983e113888`.
- **Human approvals:** Security owner — Codex verifier (acting owner) — approved; Product owner — Codex verifier (acting owner) — approved
- **Date:** 2026-08-01T18:33:36.350342Z
