# TreatCode Platform Plans

This package breaks the TreatCode platform program into bounded plans that a
single agent can execute without carrying the entire product roadmap in context.

TreatCode's intended role is the official human-and-agent workspace for learning,
querying, developing, testing, benchmarking, reviewing, and releasing the Trit
software and hardware stack.

## Authority

These plans guide work; they do not replace authoritative source, manifests, or
tests. Current authority remains:

- Repository source at an exact commit.
- `TEST_MANIFEST.json` and focused test targets.
- `ROADMAP_STATUS.json` and `KNOWN_GAPS.md`.
- `SYSCALL_MANIFEST.json`, `IMAGE_FORMAT_MANIFEST.json`, and `APP_MANIFEST.json`.
- Accepted, versioned architecture contracts and decisions.

## Execution Rule

Give an agent only:

1. One plan file.
2. The completion protocol.
3. Evidence for its completed dependencies.
4. The source paths explicitly listed by that plan.

If required work exceeds a plan's scope, create a new plan or amend the plan
before implementation. Do not silently expand scope.

## Files

- [PLAN_INDEX.md](PLAN_INDEX.md): dependency order and current state.
- [COMPLETION_PROTOCOL.md](COMPLETION_PROTOCOL.md): objective completion rules.
- [PLAN_TEMPLATE.md](PLAN_TEMPLATE.md): required structure for new plans.
- [`plans/`](plans/): independently executable plans.

## Product Requirements Preserved Across Every Plan

- Preserve the existing visual identity.
- Preserve the LeetCode-style challenge experience.
- Preserve accurate TCL documentation and make it repository-backed.
- Keep public reading pages fast and static-first.
- Make every important human operation available through a versioned API.
- Keep Git, manifests, tests, and immutable evidence authoritative.
- Never promote uploaded or agent-generated work without validation and review.

