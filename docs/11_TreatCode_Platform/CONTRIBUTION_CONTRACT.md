# P11 Contribution Contract

The contribution boundary is implemented by
`treatcode/src/contributionService.ts`. It is a quarantine-first service: an
upload is not a workspace change, and a workspace change is not a repository
mutation.

## Lifecycle

```text
upload session -> chunk ranges -> SHA-256 finalization -> quarantine acceptance
       -> isolated workspace -> validation -> immutable evidence
       -> human approval -> GitHub App branch/commit/push -> draft PR
```

The service never writes an authoritative checkout. `authoritativeSourceTouched`
is a literal `false` in every workspace record; the only repository boundary is
the injected `GitHubAppAdapter`.

## Upload policy

- Uploads are resumable by non-overlapping byte ranges. Replaying the same
  range is idempotent only when its bytes match.
- Finalization assembles ranges in offset order and verifies the declared
  `sha256:<64 hex digits>` hash.
- Provenance requires an author and `original`, `adapted`, or `third_party`
  source classification. Adapted and third-party content also requires an
  HTTP(S) source URL.
- Licenses are SPDX identifiers and are checked against the project allowlist.
- Relative paths reject absolute paths, drive prefixes, URL-encoded traversal,
  `..`, duplicate archive paths, and archive symlinks.
- Source extensions are allowlisted; executable extensions and executable magic
  (`MZ`, ELF, Mach-O), shell shebangs, binary NUL content, encrypted ZIPs,
  ZIP64/data-descriptor entries, nested archives, and excessive expansion are
  rejected.
- Accepted bytes remain marked as quarantined until explicit materialization.

## Permissions and evidence

The service recognizes independent scopes for upload, workspace, validation,
evidence, branch creation, commit creation, push, draft-PR creation, approval,
audit, and merge. A task/project mismatch is denied and recorded in the
hash-chained audit log. Merge is always denied by the contribution boundary.

Draft-PR creation requires a passing workspace validation, immutable
`correctness` and `benchmark` evidence, and a recorded approval from a human
identity unless project policy explicitly delegates that action. The
`GitHubAppClient` uses an injected installation-token provider; tokens never
enter upload or execution workers. `InMemoryGitHubApp` is the deterministic
test-repository adapter used by the P11 end-to-end fixture.

## Verification

```powershell
npm.cmd --prefix treatcode run test:uploads
npm.cmd --prefix treatcode run test:uploads-adversarial
npm.cmd --prefix treatcode run test:e2e:draft-pr
```

Reports are written to `build/treatcode-plan-evidence/P11/` and are referenced
by the P11 plan manifest.
