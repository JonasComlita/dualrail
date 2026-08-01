# TreatCode Launch Policies

These policies define the minimum launch boundary. They do not claim external
certification or legal compliance. The release record must point to the policy
version used for each candidate.

## Security

- Treat repository source, generated snapshots, uploaded content, workspace
  state, runner state, credentials, and public records as different trust
  domains.
- Use least privilege, short-lived credentials, explicit authorization checks,
  and immutable audit records for security-relevant actions.
- Execute untrusted code only through the P09 isolated runner contract. A
  failed authorization, limit, cancellation, or evidence write fails closed.
- Never place GitHub credentials or service secrets in a runner workspace,
  public snapshot, client bundle, or support ticket.
- Report suspected isolation escapes, unauthorized writes, credential exposure,
  or provenance corruption through the incident procedure immediately.

## Privacy

- Collect only the identity, workspace, run, and contribution data required for
  the requested operation.
- Keep private identity and workspace data out of read-only public snapshots.
- Redact credentials, access tokens, private source, and user-provided secrets
  from logs and support artifacts.
- Define retention and deletion for runs, workspaces, uploads, audit records,
  and incident material before enabling the corresponding production feature.
- A feature without an approved retention owner remains non-production.

## Licensing and provenance

- Every published source-backed claim must carry a repository path, commit, and
  appropriate evidence reference.
- Uploaded contributions must pass license and provenance validation before
  they can enter a reviewable workspace or draft pull request.
- Do not publish a license assertion for a source whose license is not present
  or whose compatibility has not been reviewed. The release record must name
  the repository license decision before launch.

## Accessibility

- Public and operator journeys must have named landmarks, keyboard-operable
  controls, visible focus, useful labels, live feedback where state changes,
  and no required horizontal scrolling at the supported 390 CSS-pixel width.
- Accessibility evidence is a release gate, not a post-launch suggestion.
- A passing automated check does not replace human review of the required
  critical journeys.

## Contributors and agents

- Contributions enter quarantine and an isolated workspace before validation.
- Agents may read, edit, test, benchmark, and prepare evidence only within the
  scopes granted by the identity and workspace contracts.
- No upload, agent, or automated test may write directly to an authoritative
  branch or merge a pull request.
- Draft pull requests require the explicit human approval defined by P11 and
  must link immutable correctness and benchmark evidence.
