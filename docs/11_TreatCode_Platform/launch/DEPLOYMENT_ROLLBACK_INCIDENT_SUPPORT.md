# TreatCode Deployment, Rollback, Incident, and Support Procedure

This procedure applies to a TreatCode launch candidate after P04-P12 have
complete evidence. It is intentionally operational: every action is tied to an
immutable commit, a generated snapshot, and a recorded result.

## Release preparation

1. Freeze the candidate commit and record it in `RELEASE_RECORD.md`.
2. Start from a clean checkout. Record the repository commit, environment
   fingerprint, runtime versions, and hashes for the public snapshot and all
   P13 evidence artifacts.
3. Run the exact P13 verification commands from
   `docs/11_TreatCode_Platform/plans/P13_launch_closure.md`.
4. Confirm the plan verifier reports complete and that each required approver
   has a reviewer, decision, date, and candidate commit.
5. Build the web app with `npm.cmd --prefix treatcode run build` and publish the
   resulting static assets and server package from the same candidate.

## Deployment

- Deploy the immutable artifact to a new versioned release slot.
- Keep the previous healthy slot available until post-deploy smoke and public
  route checks pass.
- Do not run migrations or write generated output into the authoritative source
  checkout during deployment.
- Verify `/`, `/stack`, `/learn`, the versioned public API, and the supported
  practice boundary from a clean client.
- Record deploy start/end, artifact hashes, operator, slot, and result in the
  release record or incident system.

## Rollback

Trigger rollback for a failed smoke check, broken public route, provenance
drift, authorization regression, execution-isolation failure, data loss, or a
security incident.

1. Stop traffic to the candidate slot and preserve its logs and evidence.
2. Repoint traffic to the last recorded healthy slot; never rebuild it from a
   moving branch.
3. Verify the previous slot with the same public smoke and health checks.
4. Mark the candidate release as rolled back, record the reason and hashes, and
   open an incident if the failure affects users or trust boundaries.
5. Preserve the failed artifact for investigation. A rollback does not erase
   evidence or silently change the release decision.

## Incident response

| Severity | Trigger | Initial action | Required follow-up |
|---|---|---|---|
| SEV-1 | Credential exposure, isolation escape, unauthorized write, or broad outage. | Page security and operations owners; contain access; rollback or disable the affected boundary. | Timeline, evidence preservation, user notification decision, root cause, and corrective plan. |
| SEV-2 | Broken public journey, provenance mismatch, data integrity concern, or repeated workspace failure. | Page operations owner; stop promotion; preserve the candidate and logs. | Reproduction, impact assessment, rollback decision, and tracked fix. |
| SEV-3 | Degraded non-critical route, documentation defect, or isolated support issue. | Assign support owner and capture the exact route, commit, and client details. | Fix or documented workaround with verification evidence. |

## Support handoff

Support must collect the user journey, UTC timestamp, route or API, candidate
commit, workspace/run ID when applicable, client dimensions, and a redacted
error excerpt. Secrets, tokens, uploaded source, and private identifiers do
not belong in public tickets. Support escalates authorization, execution,
contribution, and data-retention issues to the security or operations owner.
