# Plan Completion Protocol

This protocol makes plan completion binary, reviewable, and reproducible.

## Required Plan Sections

Every plan must define:

- One bounded objective.
- Dependencies and required dependency evidence.
- Inputs and authoritative sources.
- Deliverables with exact paths or API contracts.
- Explicit non-goals.
- Binary acceptance criteria.
- Exact verification commands.
- Required evidence artifacts.
- Any required human approval.

## Completion Rule

A plan is complete only when all of the following are true:

1. Every dependency is complete at a recorded commit.
2. Every required deliverable exists.
3. Every acceptance criterion is marked pass.
4. Every verification command exits successfully.
5. Required negative and failure-path tests pass.
6. Evidence records the tested commit, commands, environment, and artifact
   hashes.
7. Every required human gate has a named reviewer, decision, date, and commit.
8. There are no unresolved required items, placeholders, or `TBD` values.
9. The plan index points to its completion evidence.

Passing tests does not waive a missing deliverable or human gate.

## Gate Types

- **Structural:** schema, file, manifest, dependency, or API conformance.
- **Correctness:** positive, negative, differential, and integration tests.
- **Security:** isolation, authorization, abuse, and trust-boundary tests.
- **Performance:** fixed budgets and reproducible benchmark protocols.
- **Human:** explicitly identified UX, content, policy, or architecture review.

Subjective goals such as "beginner friendly" or "easy to use" must be converted
into a written rubric and receive explicit human approval. They cannot be closed
by assertion.

## Verification Interface

P00 must provide:

```powershell
python tools/trit_tool.py website plans validate
python tools/trit_tool.py website plan verify <PLAN_ID>
```

The verifier must fail when a required gate or evidence field is absent. It must
write a machine-readable result to:

```text
build/treatcode-plan-evidence/<PLAN_ID>/result.json
```

## Evidence Record

Each completed plan must record:

```text
Plan ID:
Plan version:
Verified commit:
Dependency commits:
Verifier:
Verification commands:
Command exit codes:
Environment fingerprint:
Evidence artifact paths:
Evidence artifact hashes:
Human approvals:
Verification date:
```

CI artifacts may hold large results. The plan index must retain a stable URI or
artifact identifier plus its content hash.

## Open-Ended Work

An open-ended objective is not a valid completion criterion. Convert it into:

- A fixed inventory or coverage target.
- A versioned schema or protocol.
- A measurable budget.
- A finite set of required user journeys.
- A named human signoff rubric.

Newly discovered work becomes a new capability, issue, or plan. It does not
retroactively make a correctly scoped completed plan incomplete unless it proves
one of that plan's stated acceptance criteria false.

