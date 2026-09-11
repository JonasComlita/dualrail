# TreatCode Intelligence Benchmark v3.1

V3.1 adds a multi-track Trit intelligence portfolio alongside the diagnostic
scalar executor. The scalar tasks `TC-V3-001` and `TC-V3-002` are permanent
diagnostics and are never eligible for a future Luna/Sol holdout.

The 30-task pilot corpus was disposable. All packages were qualified, then Luna
max and Sol high each received one fresh-context attempt. Every one of the 28
fully observed pairs passed for both subjects; two other pairs were excluded
from correctness inference because a host refresh interrupted one side. The
uniform family was therefore classified as ceiling-prone and discarded. A
hash-bound disposal manifest and all one-shot run evidence remain under
`build/treatcode-plan-evidence/P14/`; no pilot is eligible for reuse.

The final contract requires 100 separate unseen tasks. Luna and Sol are excluded
from final authoring and calibration. After non-subject calibration, accepted
participant bundles, graders, protocol, budgets, and task order must be frozen
before either subject model receives a task.

## Portfolio tracks and score dimensions

`tracks.v3.1.json` defines the benchmark portfolio. Fresh coding draws on
LiveCodeBench release-after-cutoff tasks; repository repair draws on SWE-bench
and DeepSWE; terminal-agent workflows draw on Terminal-Bench; expert reasoning,
frontier mathematics, and abstract generalization draw on GPQA, FrontierMath,
and ARC-AGI-2. Multimodal and evidence-grounded research tracks are represented
as extension points inspired by MMMU-Pro and BrowseComp. The inspirations guide
task shape only—external scores are never copied into TreatCode evidence.

Correctness remains the headline binary task result. Robustness, latency,
resource use, tool execution, and discussion quality are reported as separate
percent dimensions. A missing signal is `null` with an explanation, and
discussion remains secondary; v3.1 intentionally does not publish an IQ-like
composite. Repository task records may carry a `track` field so the same
hash-bound runner can aggregate results by track as the portfolio grows.

Final authorship is divided by `author-shard-plan.v3.1.json` into twenty
independently recoverable shards of five tasks. Each shard binds both category
slots and track slots, so the final portfolio mix cannot drift during authoring.
An author writes exactly one
`authoring/contributions/V31-SHARD-NNN.json` file with the assigned task IDs,
category slots, and an explicit portfolio `track` for every task. Create the JSON structure before authoring so partial work is
always a syntactically valid, resumable file, then validate and checkpoint it:

```powershell
npm run prepare:intelligence:v31:author-shard -- --shard=V31-SHARD-001 --contributor=stable-author-id --model=gpt-5.4-mini --reasoning=xhigh
npm run validate:intelligence:v31:authors -- --shard=V31-SHARD-001
npm run checkpoint:intelligence:v31:author-shard -- --shard=V31-SHARD-001
```

Checkpoint publication writes the contribution and its receipt into a temporary
private directory, flushes both files, and atomically renames the directory into
place. The receipt binds the shard plan, complete contribution bytes, canonical
task array, and each of the five task payloads. A retry with identical bytes is
idempotent; a changed retry is rejected. A completed shard remains usable if the
public staging file or author context later disappears. The aggregate validator
promotes nothing until all twenty checkpoints independently pass, and private
ingestion resumes safely after partial copying. Reviews bind both the shard ID
and contribution hash, and each review contribution covers exactly the same five
tasks so review work is recoverable at the same boundary. Earlier monolithic
author batches are not eligible for this final corpus.

Current final reviews are never written into the public benchmark tree. After
private author ingestion, each reviewer creates a five-task scaffold in the
private review root and saves each completed item before continuing:

```powershell
npm run prepare:intelligence:v31:review-shard -- --shard=V31-SHARD-001 --reviewer=stable-reviewer-id --model=gpt-5.5 --reasoning=xhigh
npm run validate:intelligence:v31:reviews
```

Two distinct non-author, non-subject reviewer identities must independently
approve every task. Only aggregate counts and hashes leave the private root;
review findings that could reveal a held-out defect remain private.

Repository manifests contain command identifiers only. A trusted service-side
registry maps those identifiers to shell-free executable/argument arrays. The
private evaluator overlays an allowlisted submission onto a clean baseline,
requires at least one Trit change, runs public and private suites with bounded
resources, writes immutable evidence outside the subject workspace, and returns
only a sealed aggregate.

Final-candidate promotion is fail-closed. The author validator checks current,
unique source anchors and the fixed category/design quotas; two independent
hash-bound reviews must approve every retained task. Package qualification runs
the broken starter, clean reference, and each adversarial mutant against real
`TEST_MANIFEST.json` targets. Calibration then requires 18 verified observations
per task: two non-author/non-subject model families across weak, medium, and
frontier cohorts, with three fresh-context runs per configuration. The pair is
selected deterministically before outcomes exist. An author family in the
three-family pool is removed; otherwise the task number rotates an adjacent
pair. This prevents accidental 27-run matrices while balancing the unused third
family across the suite.

Official freezing additionally requires a current Ed25519 infrastructure
attestation from a key in `trusted-infrastructure-keys.v3.1.json`. That signed
payload binds OS/container network isolation, process-tree resource accounting,
and the append-only evidence provider. The development trust registry is empty,
so local proxy variables or mutable files cannot be promoted as official proof.
The frozen subject publisher accepts exactly one Luna-max and one Sol-high grade
for every task in the committed order and always reports the paired bootstrap
interval and exact sign test, including when the replication target is missed.
Every calibration and subject grade also requires a separate provider-signed
model-execution attestation. That task-specific payload binds the model and
reasoning level, run and task IDs, release time, released participant bundle,
frozen suite when applicable, calibration cohort/configuration/run slot,
fresh-context and cross-task-memory controls, attempt count, and tool trace hash.
Each calibration slot receives a distinct attempt ID and append-only evidence
path. A command-line model label alone can never create official evidence.
Trusted signing keys declare separate `infrastructure` and `model_execution`
capabilities, so a model-identity key cannot attest sandbox isolation and an
infrastructure-only key cannot relabel a model run.

For local engineering checks where those publication inputs do not exist, use
the disposable development phase:

```powershell
npm run prepare:intelligence:v31:development -- --run-id=dev-001 --task=TC-V31-FINAL-001 --model=gpt-5.6-luna --reasoning=max
npm run grade:intelligence:v31:development -- --attempt-root=build/intelligence-v31-development-runs/dev-001/TC-V31-FINAL-001/gpt-5.6-luna-max
npm run prepare:intelligence:v31:development -- --run-id=dev-001 --task=TC-V31-FINAL-001 --model=gpt-5.6-sol --reasoning=high
npm run grade:intelligence:v31:development -- --attempt-root=build/intelligence-v31-development-runs/dev-001/TC-V31-FINAL-001/gpt-5.6-sol-high
# after both model slots for one or more tasks are graded:
npm run aggregate:intelligence:v31:development -- --run-id=dev-001
```

The same development aggregator can consume a freshly graded disposable pilot
run by supplying a build-relative source root, for example:

```powershell
npm run aggregate:intelligence:v31:development -- --run-id=live-20260906-b --run-root=build/intelligence-v31-pilot-runs/live-20260906-b
```

Development grades are deliberately marked `official: false`, are not added to
the holdout registry, and must not be reported as a v3.1 holdout comparison.
The aggregator accepts a paired one-task smoke run or any larger paired run,
computes both model percentages, the task-pass matrix, paired bootstrap interval,
and exact sign test, and writes an immutable development artifact consumed by the
Intelligence tab. The task participant and private grader packages still have to
be present; this mode only removes release/publication prerequisites, not the
evaluator's allowlist, workspace, resource, or network boundaries.

To inspect the best completed Luna/Sol score already present on a development
checkout, without creating a new run:

```powershell
npm run score:intelligence:v31
```

The command prefers an official result, then a disposable development result,
then the completed v3.1 pilot, and finally the older two-task diagnostic. Its
output always identifies the phase and whether the score is official.

Machine-readable contracts live in `tracks.v3.1.json`,
`score-observation.schema.v3.1.json`, `repository-task.schema.v3.1.json`,
`repository-grader.schema.v3.1.json`, `repository-patch.schema.v3.1.json`, and
`frozen-suite.schema.v3.1.json`. Final-candidate provenance and non-subject
calibration use `final-task-record.schema.v3.1.json` and
`calibration-observation.schema.v3.1.json`. Replacement patches are intentionally not raw
shell or VCS patches: they bind exact editable-file replacements to the released
bundle hash, keeping traversal, file creation, and patch-driver behavior outside
the participant surface.
