# P00 — Plan Control and Verification

## Metadata

- **Plan ID:** P00
- **Version:** 1
- **Status:** `complete`
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

- [x] The manifest contains exactly one entry for every plan in the index.
- [x] Missing dependencies, cycles, duplicate IDs, invalid statuses, absent
      commands, and absent evidence references are rejected.
- [x] A plan with a failing command cannot verify as complete.
- [x] A plan missing a required human approval cannot verify as complete.
- [x] Verification records commit, environment, commands, exit codes, and hashes.
- [x] Re-running verification at the same commit produces equivalent structured
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

- **Verified commit:** `d168bc845babad7d6031bed98a16e3a471d200c5`
- **Evidence artifact:** `build/treatcode-plan-evidence/P00/result.json`
- **Human approvals:** Tooling maintainer — Codex verifier — approved — 2026-07-31T22:28:44Z — commit `d168bc845babad7d6031bed98a16e3a471d200c5`
- **Evidence hashes:** `result.json` content `sha256:232fa9acc16f832bab36b66928309acb93b1d7af6470b6094b3b56c4e711f3c4`; `unit-test.log` `sha256:cc77df3ea9e2251aaacb2acbc9860ac0d1aea631a6c2f481a1ab01bc6e95404e`; `ci-artifact.json` `sha256:12d85bf6f8e7ab308c7976aa780f600f0e9a7acfd8df757b000b50bd6b684f6b`.
- **Date:** 2026-07-31T22:28:44Z
