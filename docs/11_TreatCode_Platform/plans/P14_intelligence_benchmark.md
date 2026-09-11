# P14 — Intelligence Benchmark and Participant Journey

## Metadata

- **Plan ID:** P14
- **Version:** 2
- **Status:** `complete` (technical implementation)
- **Depends on:** P06, P07, P09, P10
- **Scope owner:** TreatCode product and architecture owners

## Objective

Add a first-class repository-repair intelligence benchmark for Trit coding
agents, durable participant accounts and community artifacts, and
evidence-backed end-to-end proof. The five-task/four-trial scalar protocol is
retained as a diagnostic product and grader regression suite. It is not a
model-ranking benchmark because repeated execution of one authored solution is
not an independent observation. The v3.1 protocol defines at least 100
distinct tasks across its frozen coding, terminal, reasoning, math, and
generalization tracks, with one fresh, bounded model attempt per task.
TreatCode scores remain separate from the external DeepSWE reference (currently
`gpt-5.6-luna[max]` at `67% ±4%` over 113 tasks).

## Deliverables

1. Durable handle/password accounts with least-privilege participant grants.
2. Versioned saved solutions, explicit public practice-solution posts,
   authenticated submissions, and plain-text solution discussions. Practice
   uses a two-tab problem panel (`General information` and `Discussions`): a
   discussion post is one verified solution, its code, explanation/pseudocode,
   measured runtime/memory/cycle/test evidence, and durable community votes.
   Benchmark workspace drafts remain private unless a future surface explicitly
   opts into publication.
3. Diagnostic scalar intelligence suite with five hard, executable multi-file
   mini-repository tasks and 140 hidden cases (560 hidden executions across
   the required four trials):
   algorithmic Trit repair, parser/serialization, memory/pointer safety,
   concurrency/state, and syscall/ABI integration. Each task has public tests,
   isolated trials, server-only hidden verification, sealed four-trial scoring,
   persistence, and attestation.
4. `/intelligence` route, leaderboard, trial workspace, and practice content
   controls.
5. API, security, persistence, browser E2E, and bounded-run proof evidence under
   `build/treatcode-plan-evidence/P14/`.
6. Versioned v3 protocol requiring 100 distinct tasks, independent author and
   reviewer provenance, participant/grader bundle isolation, fixed pre-release
   clocks and tool/token budgets, a frozen holdout, and weak/medium/frontier
   calibration.
7. V3.1 portfolio scoring with fresh coding, repository repair, terminal
   agency, expert reasoning, frontier mathematics, and abstract/multimodal/
   evidence-grounded generalization tracks. Correctness, robustness, latency,
   resource use, tool execution, and discussion quality remain separate
   dimensions; discussion review is secondary and contributes zero weight to
   the executable score.
8. Task-bootstrap confidence intervals, paired task comparison, exact sign
   tests, and enforced ceiling/floor/discrimination gates.
9. V3.1 replaces monolithic final author batches with twenty atomic five-task
   shards. Each immutable checkpoint binds the category and track assignment
   plan, contribution bytes, canonical task array, and individual task hashes;
   private ingest is resumable and current review findings remain outside the
   public tree.
10. Provider-signed model-execution attestations bind every calibration and
    subject grade to its model, reasoning effort, task, release time, released
    bundle, suite, calibration slot, fresh-context controls, and tool trace.
    Repeated calibration runs have collision-free attempt IDs and evidence
    paths. Self-labeled executions are never official evidence.

## Non-Goals

- Replacing the existing P10 performance arena or its metrics.
- Email verification, password recovery, billing, or social identity.
- Exercising operator-only workspaces, operations, contribution, approval, or
  destructive recovery flows in the participant proof.
- Claiming model cost or token usage when the runner does not provide it.

## Acceptance Criteria

- [x] A participant can register, log in after restart, solve T001, save the
      solution, submit it under the authenticated handle, and publish a linked
      explanation plus pseudocode.
- [x] A practice problem exposes `General information` and `Discussions` tabs;
      the latter lists the latest explicitly posted, verified solutions from
      the community as unified posts containing code, author handles,
      plain-English explanation/pseudocode, runtime/memory/cycle/test metrics,
      and durable thumbs-up ranking. A post is publishable only after its
      author passes verification with the same source; private benchmark
      drafts never enter this feed and there is no separate publish-discussion
      action.
- [x] `TC-SWE-001` exposes only allowlisted files and public tests; hidden
      verifier data is absent from client payloads and trial workspaces.
- [x] Four clean one-shot trials produce a sealed `passed/4` score; incomplete,
      timed-out, or tampered trials cannot enter the official leaderboard, and
      a privileged attestation is required before official publication. A
      caller-supplied model label without provider/session/prompt/artifact
      provenance is a harness result, not a model score.
- [x] The full suite catalog exposes five hard task contracts, including
      difficulty, capability, and repository-shape metadata without hidden
      values; a bounded compiler run completes four fresh sealed trials for
      every task across all 140 hidden cases.
- [x] The browser participant journey reaches public search/learn, P10 arena,
      practice, saved solution, discussion, intelligence benchmark, and both
      leaderboards.
- [x] Restart persistence, auth isolation, path traversal rejection, content
      limits, and existing challenge/auth regressions pass.
- [x] P14 evidence records the tested commit, model configuration, trial IDs,
      route assertions, command exit codes, and artifact hashes.

The checked items above are the technical completion gate. The remaining items
are intentionally tracked as optional publication validation; they do not block
P14's technical status.

### Optional publication validation (non-blocking)

- [ ] V3.1 contains at least 100 executable, independently authored and
      two-reviewer-approved tasks across the frozen track mix. Candidate
      identifiers or generated briefs do not satisfy this criterion.
- [ ] Every v3 task is attempted once in a fresh context with the wall clock
      started before task release, fixed tool/token/test/patch budgets, and
      cross-task memory disabled.
- [ ] Hidden graders and reference solutions are packaged under a privileged
      root outside the participant tree; package and access-trace hashes prove
      that the participant could not read them.
- [ ] Weak, medium, and frontier pilot cohorts calibrate every task; excessive
      ceiling/floor items and items below the discrimination threshold are
      revised or removed before the holdout is frozen.
- [ ] Full-suite model comparisons use identical task sets and report task-level
      results, dimension scores, bootstrap confidence intervals, paired deltas,
      and significance. Partial coverage is never official.
- [ ] Discussion grading is length-controlled, human-calibrated, reported
      separately, and never changes the executable model score.
- [ ] All twenty v3.1 five-task author shards have immutable checkpoint
      receipts, all forty private review shards cover their exact five-task
      contribution, and interrupted private ingest resumes without changing a
      contribution hash.
- [ ] Calibration uses the deterministic two-family assignment for exactly
      1,800 provider-signed observations; the frozen comparison uses exactly
      200 provider-signed Luna-max/Sol-high executions.

## Verification

```powershell
npm.cmd --prefix treatcode run test:community
npm.cmd --prefix treatcode run test:intelligence
npm.cmd --prefix treatcode run test:intelligence:suite
npm.cmd --prefix treatcode run test:intelligence:v3
npm.cmd --prefix treatcode run validate:intelligence:v3
npm.cmd --prefix treatcode run validate:intelligence:v31
npm.cmd --prefix treatcode run test:intelligence:v31:author-shards
npm.cmd --prefix treatcode run test:intelligence:v31:repository
npm.cmd --prefix treatcode run test:intelligence:v31:calibration-assignments
npm.cmd --prefix treatcode run test:intelligence:v31:calibration
npm.cmd --prefix treatcode run test:intelligence:v31:infrastructure
npm.cmd --prefix treatcode run test:intelligence:v31:comparison
npm.cmd --prefix treatcode run test:intelligence:v31:catalog
npm.cmd --prefix treatcode run test:intelligence:v31:tracks
npm.cmd --prefix treatcode run test:e2e:intelligence
npm.cmd --prefix treatcode run test:participant-journey
tools/trit-doctor.ps1
tools/trit-test.ps1 smoke
python tools/trit_tool.py test production --no-build
python tools/trit_tool.py knowledge status
python tools/trit_tool.py website plans validate
python tools/trit_tool.py website plan verify P14
```

The manifest gives the full twenty-trial intelligence suite and production gate
900-second per-command ceilings; all other commands keep the verifier's
300-second default. This prevents a successful long gate from being recorded as
a timeout without weakening shorter checks.

## Required Evidence

- `build/treatcode-plan-evidence/P14/result.json`
- `build/treatcode-plan-evidence/P14/community-tests.json`
- `build/treatcode-plan-evidence/P14/intelligence-tests.json`
- `build/treatcode-plan-evidence/P14/intelligence-suite-e2e.json`
- `build/treatcode-plan-evidence/P14/intelligence-e2e.json`
- `build/treatcode-plan-evidence/P14/participant-journey.json`
- `benchmarks/intelligence-v3/protocol.v3.json`
- `benchmarks/intelligence-v3/corpus.candidates.v3.json`
- `benchmarks/intelligence-v3/calibration-observations.schema.v3.json`
- `benchmarks/intelligence-v3/calibration-plan.v3.json`
- `benchmarks/intelligence-v3/discussion-rubric.v3.json`
- `benchmarks/intelligence-v3/provenance-bundle.schema.v3.json`
- `benchmarks/intelligence-v3/trusted-contributors.schema.v3.json`
- `benchmarks/intelligence-v3/executable-drafts.v3.json`
- `build/treatcode-plan-evidence/P14/intelligence-v3-foundation.json`
- `build/treatcode-plan-evidence/P14/intelligence-v3-readiness.json`
- `benchmarks/intelligence-v3.1/author-shard-plan.v3.1.json`
- `benchmarks/intelligence-v3.1/tracks.v3.1.json`
- `benchmarks/intelligence-v3.1/score-observation.schema.v3.1.json`
- `benchmarks/intelligence-v3.1/author-shard-receipt.schema.v3.1.json`
- `benchmarks/intelligence-v3.1/model-execution-attestation.schema.v3.1.json`
- `build/treatcode-plan-evidence/P14/intelligence-v31-corpus-validation.json`
- `build/treatcode-plan-evidence/P14/intelligence-v31-authoring-validation.json`

The v3 candidate registry contains 100 unique authoring briefs and two
separately registered executable development drafts, but remains deliberately
non-official. `TC-V3-001` has a two-module participant repository and a grader
outside the subject workspace; its starter fails 0/5 public and 0/12 hidden
cases, and a private known-good fixture passes 5/5 and 12/12. `TC-V3-002`
adds interval normalization and boundary reasoning; its starter passes only
1/5 public and 4/14 hidden, while the private known-good passes 5/5 and 14/14.
Independent authors, two reviewers per task, the other 98 executable packages,
pilot cohort results, and a frozen holdout are still required. The workspace
owner authorized continued nonofficial development in
`owner-confirmation.v3.json`; its explicit nonclaims prevent that authorization
from being counted as independent review or calibration. The validator treats
every missing official artifact as a publication blocker.

V3.1 infrastructure and orchestration are implemented, and P14 is technically
complete. The final holdout remains development evidence rather than an official
model score. The 30 disposable pilots were qualified, run, classified as
ceiling-prone, and hash-bound at disposal. Final readiness is intentionally zero
until genuine non-subject authors complete all twenty final five-task shards.
Each shard can be scaffolded before task work begins, validated alone, and
atomically checkpointed; a synthetic regression assembles twenty checkpoints
into exactly 100 tasks and resumes an ingest interrupted after its first copy.
No task outcome may be tuned after a suite is frozen, and a missed 60–75 score
band or 1–4 task Sol lead is reported as a failed replication rather than
repaired in place.

## Optional publication validation

Independent authorship, two-reviewer approval, provider-signed calibration and
subject executions, an immutable freeze, and product/architecture/security
approvals are publication-quality evidence requirements only. They do not block
the technical P14 completion status. Until supplied, v3.1 remains clearly
labeled development evidence and cannot be presented as an official model score.
