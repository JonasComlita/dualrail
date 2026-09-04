# TreatCode Intelligence Benchmark v3.1

V3.1 adds repository-level Trit engineering tasks without removing the legacy
v3 scalar executor. The scalar tasks `TC-V3-001` and `TC-V3-002` are permanent
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

Repository manifests contain command identifiers only. A trusted service-side
registry maps those identifiers to shell-free executable/argument arrays. The
private evaluator overlays an allowlisted submission onto a clean baseline,
requires at least one Trit change, runs public and private suites with bounded
resources, writes immutable evidence outside the subject workspace, and returns
only a sealed aggregate.

Machine-readable contracts live in `repository-task.schema.v3.1.json`,
`repository-grader.schema.v3.1.json`, `repository-patch.schema.v3.1.json`, and
`frozen-suite.schema.v3.1.json`. Final-candidate provenance and non-subject
calibration use `final-task-record.schema.v3.1.json` and
`calibration-observation.schema.v3.1.json`. Replacement patches are intentionally not raw
shell or VCS patches: they bind exact editable-file replacements to the released
bundle hash, keeping traversal, file creation, and patch-driver behavior outside
the participant surface.
