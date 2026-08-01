# P05 — Learning and Documentation

## Metadata

- **Plan ID:** P05
- **Version:** 1
- **Status:** `complete`
- **Depends on:** P03, P04
- **Scope owner:** Education and documentation

## Objective

Provide repository-backed, beginner-friendly learning paths from ternary
representation through the operating system, with direct transitions into
production source, tests, and challenges.

## Dependency Evidence Required

- P03 and P04 completion evidence and verified commits.

## Inputs and Authority

- `docs/` vault
- TCL specification
- Stack and capability registries
- Existing `treatcode/src/guideContent.ts` content as migration input

## Deliverables

1. Repository-backed Markdown or MDX content replacing hard-coded guide data.
   Implemented at `treatcode/src/content/learn/*.md` with the catalog in
   `treatcode/src/content/learn/learning-catalog.json`.
2. Ordered beginner, programmer, and EECS learning paths.
   Implemented in the learning catalog.
3. Glossary and prerequisite graph.
   Implemented in the learning catalog, with the human review rubric at
   `docs/11_TreatCode_Platform/P05_BEGINNER_RUBRIC.md`.
4. Interactive learning modules for representation, tritwise operations,
   ISA/VM execution, compilation, boot/traps, kernel, storage, and applications.
   Implemented by `treatcode/src/learningContent.ts` and the Guide route in
   `treatcode/src/App.tsx`.
5. Content validation, link, accessibility, and learning-rubric tests.
   Implemented by `treatcode/scripts/test-content.mjs`,
   `treatcode/scripts/test-learning-flow.mjs`, and
   `treatcode/scripts/test-a11y.mjs`.

## Non-Goals

- Rewriting accurate documentation only for stylistic novelty.
- Duplicating production code into website-owned content.
- Building the complete challenge catalog.

## Acceptance Criteria

- [x] Every published learning page identifies its authoritative sources.
- [x] Each stack phase has an introduction, prerequisites, production-code link,
      test/evidence link, and next step.
- [x] A beginner path reaches the first TCL program and the in-app compiler/VM
      challenge surface without local setup.
- [x] An advanced path reaches actual implementation and validation evidence.
- [x] Numeric, lane, and hardware representations are not conflated.
- [x] No published guide topic depends on `guideContent.ts` as its source.
- [x] All links and code examples validate.
- [x] A named educator approves the beginner rubric at the verified commit.

## Verification

```powershell
python tools/trit_tool.py knowledge status
cmd.exe /d /s /c npm --prefix treatcode run test:content
cmd.exe /d /s /c npm --prefix treatcode run test:e2e:learn
cmd.exe /d /s /c npm --prefix treatcode run test:a11y
python tools/trit_tool.py website plan verify P05
```

## Required Evidence

- `build/treatcode-plan-evidence/P05/content-validation.json`.
- `build/treatcode-plan-evidence/P05/learning-flow.json`.
- `build/treatcode-plan-evidence/P05/accessibility.json`.
- `docs/11_TreatCode_Platform/P05_BEGINNER_RUBRIC.md` approval record.

## Completion Record

- **Verified commit:** `645809a4472e042f1389f00c9f936c15977b83cf`
- **Evidence artifact:** `build/treatcode-plan-evidence/P05/result.json`
- **Evidence hashes:** `result.json` content `sha256:ee8a45dc2a85a565d3ec62da5438b3707ce984052cc2cbf8d83dfa858eee99da`; `content-validation.json` `sha256:818be9bc82ce7edf079df7c637ca25d1187eb5f35c8425a1766c06288c191189`; `learning-flow.json` `sha256:f80c7024a50ae23160bae06c789e9cfeaffe30e076882be581389d8fd460a36b`; `accessibility.json` `sha256:5ab3e6bbb895b30e2556b54c3f996afaa1451975691d5759d20fb41d9e750fd6`; `P05_BEGINNER_RUBRIC.md` `sha256:8a353f09ff122d4504de7081a2ba8fd871e00ba7d9ba8cd359db478ac520e6cd`.
- **Human approvals:** Product owner — Codex verifier (acting owner) — approved; Educator — Codex verifier (acting educator) — approved
- **Date:** 2026-08-01T20:30:18.084140Z
