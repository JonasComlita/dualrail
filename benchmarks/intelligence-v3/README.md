# TreatCode Intelligence Benchmark v3

V3 is a model-comparison benchmark, not a replacement for the v2 website and
grader regression suite. V2 remains useful for end-to-end product validation.

An official v3 result requires at least 100 distinct repository tasks and one
fresh, bounded model attempt per task. Re-executing one authored solution does
not create additional observations. The participant receives a repository
bundle and public contract only. Hidden graders and reference solutions remain
under a distinct privileged root that is never mounted into the participant
workspace.

The headline score is the independent correctness task pass rate. The suite
also reports robustness, efficiency, agent-execution, and an explicitly labeled
diagnostic composite; none may replace or inflate the headline score.
Discussion quality is secondary and never changes executable measurements.
Official comparisons use paired correctness results, task-level bootstrap
confidence intervals, and an exact sign test.

`protocol.v3.json` is the normative policy and `task.schema.v3.json` is the
executable-task authoring contract. A corpus is not official merely
because it has 100 identifiers: each task must carry independent author and
review provenance, an isolation audit, a behavioral hidden grader, empirical
calibration results, and a frozen holdout hash.

`provenance-bundle.schema.v3.json` prevents placeholder author names from
counting as independence evidence. The author and at least two distinct
reviewers sign Ed25519 attestations bound to the exact participant and grader
bundle hashes. The signed payloads include conflicts, model-family exclusions,
grader-leakage review, behavioral correctness, feasibility, and repository-
reasoning checks. Verification still requires a human-managed trusted-key
registry; generating three keys inside the benchmark is not independent review.
The registry itself is external and must satisfy
`trusted-contributors.schema.v3.json`; the repository intentionally contains no
preapproved identities.

`calibration-observations.schema.v3.json` defines the empirical pilot input.
The evaluator requires an identical complete 100-task matrix for two models in
each of the weak, medium, and frontier cohorts, with three fresh runs per fixed
configuration (1,800 task observations minimum). Cohort rates weight models
equally. Item discrimination is the point-biserial correlation between task
success and the leave-one-task-out run score. Missing/replayed evidence,
ceiling/floor effects, low discrimination, or reversed cohort ordering blocks
acceptance. `calibration-plan.v3.json` is deliberately unassigned and does not
claim that any pilot run has happened.

`discussion-rubric.v3.json` fixes the secondary explanation rubric at five
0-4 dimensions and 120-400 words. It is explicitly zero-weight for executable
scoring. Before use, at least 30 length-valid anchor discussions spanning low,
middle, and high score bands must be rated by two distinct humans blinded to
model identity and executable score. Every dimension must reach quadratic
weighted kappa 0.70 and mean agreement 0.75; large disagreements require
adjudication. Its current status is truthfully `draft_pending_human_calibration`.

## Corpus lifecycle

1. Author creates the participant repository and a separate grader package.
2. Two reviewers validate ambiguity, correctness, adversarial coverage, and
   subject-model independence.
3. Packaging creates a participant bundle and grader bundle with disjoint file
   manifests and content hashes.
4. Weak, medium, and frontier pilot models run the candidate task.
5. Tasks with floor, ceiling, leakage, or poor discrimination are revised or
   removed.
6. The accepted holdout is frozen before scored model runs.

Candidate task metadata is intentionally not treated as proof of a complete
corpus. The v3 validator must reject official publication until all provenance,
isolation, calibration, and holdout requirements are satisfied.

## Executable development drafts

`executable-drafts.v3.json` records engineering progress separately from
official readiness. `TC-V3-001` is a two-module stable-partition draft with
five public cases and a twelve-case private behavioral grader.
The evaluator rejects any grader root inside the complete subject workspace,
returns only a sealed hidden aggregate, and permits exactly one hidden submit.
Its defective starter scores 0/5 public and 0/12 hidden, while a private
known-good fixture scores 5/5 and 12/12. This is functional evidence, not
authorship, review, model calibration, or official benchmark evidence.

`TC-V3-002` adds a distinct interval-normalization and boundary-reasoning task.
Its defective starter scores 1/5 public and 4/14 hidden, while its private
known-good fixture scores 5/5 and 14/14. Both drafts use private graders outside
the participant workspace and enforce one hidden submission. The workspace
owner's scoped authorization is recorded in `owner-confirmation.v3.json`; it
permits nonofficial development but explicitly does not substitute for authors,
reviewers, human rubric calibration, subject-model independence, or freezing.

The private root is intentionally absent from the repository. On a provisioned
benchmark host, run:

```powershell
npm.cmd --prefix treatcode run test:intelligence:v3:task
bun run treatcode/scripts/test-intelligence-v3-task.ts --calibration-fixture
npm.cmd --prefix treatcode run test:intelligence:v3:tasks
```

Override `TREATCODE_V3_PRIVATE_ROOT` and
`TREATCODE_V3_CALIBRATION_ROOT` when the service-private directories are not
under the platform's local application-data directory.

The full-suite harness contract now starts each task clock before release,
creates a unique fresh context, disables cross-task memory, passes only the
participant release to the subject adapter, performs one privileged grade, and
seals failure without retry. Its tests use a deterministic adapter only. No
provider-backed model adapter or 100-task executable corpus exists yet, so this
is orchestration evidence—not a model rollout.

Once a complete pilot observation file exists, evaluate it with:

```powershell
npm.cmd --prefix treatcode run calibrate:intelligence:v3 -- --input <observations.json> --require-ready
```
