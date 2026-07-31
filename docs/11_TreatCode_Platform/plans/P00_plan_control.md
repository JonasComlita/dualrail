# P00 — Plan Control and Verification

## Metadata

- **Plan ID:** P00
- **Version:** 1
- **Status:** `not_started`
- **Depends on:** None
- **Scope owner:** Platform tooling

## Objective

Provide one machine-verifiable mechanism that validates every TreatCode plan and
prevents a plan from being marked complete without required evidence.

## Inputs and Authority

- [Completion protocol](../COMPLETION_PROTOCOL.md)
- [Plan index](../PLAN_INDEX.md)
- `tools/trit_tool.py`

## Deliverables

1. `TREATCODE_PLAN_MANIFEST.json` with plan IDs, versions, dependencies, status,
   verification commands, gates, and evidence references.
2. A versioned JSON schema for the manifest.
3. `python tools/trit_tool.py website plans validate`.
4. `python tools/trit_tool.py website plan verify <PLAN_ID>`.
5. Automated tests containing one valid fixture and fixtures for every required
   failure mode.
6. CI integration that preserves verifier results as artifacts.

## Non-Goals

- Implementing any product feature described by P01–P13.
- Automatically deciding subjective UX or architectural questions.

## Acceptance Criteria

- [ ] The manifest contains exactly one entry for every plan in the index.
- [ ] Missing dependencies, cycles, duplicate IDs, invalid statuses, absent
      commands, and absent evidence references are rejected.
- [ ] A plan with a failing command cannot verify as complete.
- [ ] A plan missing a required human approval cannot verify as complete.
- [ ] Verification records commit, environment, commands, exit codes, and hashes.
- [ ] Re-running verification at the same commit produces equivalent structured
      results apart from timestamps and runtime durations.

## Verification

```powershell
python -m unittest tools.tests.test_treatcode_plan_verifier
python tools/trit_tool.py website plans validate
python tools/trit_tool.py website plan verify P00
```

## Required Evidence

- Passing unit-test log.
- `build/treatcode-plan-evidence/P00/result.json`.
- CI artifact identifier and content hash.

## Completion Record

- **Verified commit:**
- **Evidence artifact:**
- **Human approvals:** Tooling maintainer
- **Date:**

