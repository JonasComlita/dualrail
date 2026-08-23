# P14 — Intelligence Benchmark and Participant Journey

## Metadata

- **Plan ID:** P14
- **Version:** 1
- **Status:** `in_progress`
- **Depends on:** P06, P07, P09, P10
- **Scope owner:** TreatCode product and architecture owners

## Objective

Add a first-class DeepSWE-style intelligence benchmark for Trit coding agents,
durable participant accounts and community artifacts, and evidence-backed
end-to-end proof with four independent, provenance-verifiable model trials.
TreatCode task/suite scores must remain separate from the external DeepSWE
reference (currently `gpt-5.6-luna[max]` at `67% ±4%` over 113 tasks).

## Deliverables

1. Durable handle/password accounts with least-privilege participant grants.
2. Versioned saved solutions, authenticated submissions, and plain-text
   solution discussions.
3. Versioned v2 intelligence suite with five hard, executable multi-file
   mini-repository tasks and 140 hidden cases (560 hidden executions across
   the required four trials):
   algorithmic Trit repair, parser/serialization, memory/pointer safety,
   concurrency/state, and syscall/ABI integration. Each task has public tests,
   isolated trials, server-only hidden verification, sealed four-trial scoring,
   persistence, and attestation.
4. `/intelligence` route, leaderboard, trial workspace, and practice content
   controls.
5. API, security, persistence, browser E2E, and Luna proof evidence under
   `build/treatcode-plan-evidence/P14/`.

## Non-Goals

- Replacing the existing P10 performance arena or its metrics.
- Email verification, password recovery, billing, or social identity.
- Exercising operator-only workspaces, operations, contribution, approval, or
  destructive recovery flows in the participant proof.
- Claiming model cost or token usage when the runner does not provide it.

## Acceptance Criteria

- [ ] A participant can register, log in after restart, solve T001, save the
      solution, submit it under the authenticated handle, and publish a linked
      explanation plus pseudocode.
- [ ] `TC-SWE-001` exposes only allowlisted files and public tests; hidden
      verifier data is absent from client payloads and trial workspaces.
- [ ] Four clean one-shot trials produce a sealed `passed/4` score; incomplete,
      timed-out, or tampered trials cannot enter the official leaderboard, and
      a privileged attestation is required before official publication. A
      caller-supplied model label without provider/session/prompt/artifact
      provenance is a harness result, not a model score.
- [ ] The full suite catalog exposes five hard task contracts, including
      difficulty, capability, and repository-shape metadata without hidden
      values; a bounded compiler run completes four fresh sealed trials for
      every task across all 140 hidden cases.
- [ ] The browser participant journey reaches public search/learn, P10 arena,
      practice, saved solution, discussion, intelligence benchmark, and both
      leaderboards.
- [ ] Restart persistence, auth isolation, path traversal rejection, content
      limits, and existing challenge/auth regressions pass.
- [ ] P14 evidence records the tested commit, model configuration, trial IDs,
      route assertions, command exit codes, and artifact hashes.

## Verification

```powershell
npm.cmd --prefix treatcode run test:community
npm.cmd --prefix treatcode run test:intelligence
npm.cmd --prefix treatcode run test:intelligence:suite
npm.cmd --prefix treatcode run test:e2e:intelligence
npm.cmd --prefix treatcode run test:participant-journey
tools/trit-doctor.ps1
tools/trit-test.ps1 smoke
python tools/trit_tool.py test production --no-build
python tools/trit_tool.py knowledge status
python tools/trit_tool.py website plans validate
python tools/trit_tool.py website plan verify P14
```

## Required Evidence

- `build/treatcode-plan-evidence/P14/result.json`
- `build/treatcode-plan-evidence/P14/community-tests.json`
- `build/treatcode-plan-evidence/P14/intelligence-tests.json`
- `build/treatcode-plan-evidence/P14/intelligence-suite-e2e.json`
- `build/treatcode-plan-evidence/P14/intelligence-e2e.json`
- `build/treatcode-plan-evidence/P14/participant-journey.json`
- `build/treatcode-plan-evidence/P14/luna-max-four-trials.json`

The Luna artifact above is retained as a blind task-candidate record. It shows
that a `gpt-5.6-luna[max]` agent can derive and submit the pilot repair, but it
does not prove four independent provider-issued model contexts or a complete
suite rollout. Until provenance-verifiable rollout evidence is captured, it
must not be used as an official model score.

## Required Approvals

Product owner, architecture owner, and security owner must review the final
participant/account and hidden-verifier evidence before P14 is marked complete.
