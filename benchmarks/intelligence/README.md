# TreatCode Intelligence Systems Repair Suite v2

This directory contains five versioned Trit mini-repository repairs. Version 2
replaces the original one-expression exercises with compound contracts that
require validation precedence, multi-file navigation, edge-case reasoning, and
actual execution through the bounded Trit compiler/VM.

| Task | Repair surface | Files | Public | Hidden partitions |
| --- | --- | ---: | ---: | ---: |
| `TC-SWE-001` | resilient sensor consensus | 4 | 6 | 28 cases / 4 suites |
| `TC-SWE-002` | checked balanced-ternary codec | 2 | 7 | 28 cases / 4 suites |
| `TC-SWE-003` | aligned half-open pointer span | 2 | 6 | 26 cases / 3 suites |
| `TC-SWE-004` | ordered saturating state machine | 2 | 6 | 28 cases / 3 suites |
| `TC-SWE-005` | syscall result-policy adapter | 2 | 8 | 30 cases / 5 suites |

The suite therefore exercises 140 hidden cases per trial pass and 560 hidden
case executions across its required four fresh trials. Hidden fixtures are
loaded only by the TreatCode service; they are never copied into participant
workspaces or returned by a public catalog endpoint. Public catalog metadata
may disclose counts and partition names, but never case IDs, inputs, expected
values, or the server-side fixture path.

Each benchmark run creates four independent clean trial fixtures. A public
test may be run repeatedly while the trial is open. A hidden submission is
one-shot per trial; its individual result remains sealed until all four trials
have submitted. The task aggregate is `passed / 4 * 100`, and its scope is
**task trial reliability** only. A deterministic fixture replay can therefore
reach 100% without measuring a model. It is published as a harness result
unless an external attestor binds four independent model rollouts to
provider/session, prompt, and artifact evidence. TreatCode does not substitute
the external DeepSWE reference (`gpt-5.6-luna[max]`, 67% ±4% over 113 tasks)
for its own measurement; a full-suite model score is withheld until every
versioned task has provenance-bound coverage.

Every starter repository is intentionally defective. A repair may edit only
the task manifest's allowlist and must preserve its published scalar entrypoint.
The checked-in known-correct fixtures exist only in the test harness: they prove
that every public and hidden expectation is executable, but are explicitly
attested as `harness_fixture` evidence and never promoted to a model score.
