# P05 — Learning and Documentation

## Metadata

- **Plan ID:** P05
- **Version:** 1
- **Status:** `not_started`
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
2. Ordered beginner, programmer, and EECS learning paths.
3. Glossary and prerequisite graph.
4. Interactive learning modules for representation, tritwise operations,
   ISA/VM execution, compilation, boot/traps, kernel, storage, and applications.
5. Content validation, link, accessibility, and learning-rubric tests.

## Non-Goals

- Rewriting accurate documentation only for stylistic novelty.
- Duplicating production code into website-owned content.
- Building the complete challenge catalog.

## Acceptance Criteria

- [ ] Every published learning page identifies its authoritative sources.
- [ ] Each stack phase has an introduction, prerequisites, production-code link,
      test/evidence link, and next step.
- [ ] A beginner path reaches and runs a first TCL program without local setup.
- [ ] An advanced path reaches actual implementation and validation evidence.
- [ ] Numeric, lane, and hardware representations are not conflated.
- [ ] No published guide topic depends on `guideContent.ts` as its source.
- [ ] All links and code examples validate.
- [ ] A named educator approves the beginner rubric at the verified commit.

## Verification

```powershell
python tools/trit_tool.py knowledge status
npm --prefix treatcode run test:content
npm --prefix treatcode run test:e2e:learn
npm --prefix treatcode run test:a11y
python tools/trit_tool.py website plan verify P05
```

## Required Evidence

- Content-validation report.
- Learning-path E2E report.
- Beginner-rubric approval with reviewer and commit.

## Completion Record

- **Verified commit:**
- **Evidence artifact:**
- **Human approvals:** Product owner; educator
- **Date:**

