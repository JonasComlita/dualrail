# P15 — P04 Completeness Amendment: Public Knowledge API and Stack Explorer

## Metadata

- **Plan ID:** P15
- **Version:** 1
- **Status:** `in_progress`
- **Depends on:** P02, P03, P04
- **Scope owner:** Public platform
- **Replaces for completeness purposes:** the v1 user-facing scope of P04
- **Agent handoff prompt:** [P04 agent completion prompt](P04_AGENT_COMPLETION_PROMPT.md)

## Why this amendment exists

P04 v1 proved that a versioned snapshot, API surface, and Stack Explorer route
could be built. It did not prove that the explorer was a complete browser for
the repository. Its checks were satisfied by route existence, schema
conformance, a handful of known queries, and general responsive/performance
budgets. They did not require every stack phase to have a useful detail view,
every related record to be reachable, every result to be untruncated, or a
reader to complete representative journeys across the entire inventory.

The v1 completion record remains useful historical evidence for the plumbing
slice. It is not evidence for this amendment and must not be reused to claim
that the complete Stack Explorer is finished.

## Objective

Deliver a public, read-only, repository-backed knowledge browser that lets a
new reader move from the physical/ternary boundary through the ISA, VM,
compiler, runtime, boot process, kernel, storage, devices, user space, images,
and system closure, while reaching the exact source, symbol, contract,
capability, test, benchmark, decision, release, and known-gap evidence behind
each claim.

“Complete” means complete coverage of the current repository knowledge model,
not implementation of every future hardware or operating-system gap. Planned
or missing capabilities must be represented as explicitly labelled gaps with
their authoritative evidence; they may not disappear from the public view.

## Scope boundary

Included:

- The full current `stack_nodes` inventory and all public entity types exposed
  by the P04 API contract.
- Static snapshots and live API responses with identical records,
  relationships, provenance, and freshness metadata.
- Public index, detail, relationship, source, search, gap, release, and
  evidence views that work as direct links.
- Read-only browsing, exact/symbol/relationship/semantic search, keyboard
  navigation, responsive layouts, and no-JavaScript reading paths.

Excluded:

- Authentication, writes, remote workspaces, agent execution, or shipping the
  editor/runner into public reading routes.
- Pretending that a planned physical driver, accelerator, or other known gap
  is already implemented. The explorer must expose that distinction instead.

## Required repository inventory

Create a machine-readable coverage report derived from the generated snapshot,
not from a hand-maintained sample list. The report must enumerate every record
in these collections and the routes/links that expose it:

- `projects`, `stack_nodes`, `components`, `capabilities`, `contracts`,
  `decisions`, `sources`, `symbols`, `tests`, `benchmarks`, `runs`,
  `releases`, and `gaps`.
- Every relationship edge, including dependencies, contracts, capabilities,
  source/evidence links, decisions, releases, tests, benchmarks, and gap
  references.
- Every current stack phase. The current registry contains 21 phases, but the
  validator must derive the count and IDs from the source-of-truth registry so
  a future phase cannot silently be omitted.

For each stack phase, the detail view must answer all of these questions:

1. What problem does this phase solve, and what enters and leaves it?
2. What depends on it and what does it depend on?
3. Which production files, symbols, contracts, tests, benchmarks, decisions,
   releases, and known gaps establish the claim?
4. Which parts are implemented, planned, experimental, or unavailable?
5. What should a reader open next, including a learning lesson when one
   exists?

No required collection may be silently limited to the first N records. If a
  view paginates, it must provide deterministic pagination and a visible way
  to reach every page; the coverage report must still prove that every record
  is reachable.

## Deliverables

1. **Authoritative data pipeline.** Generate the OpenAPI contract, snapshot,
   coverage manifest, relationship index, and freshness/provenance report from
   the validated registries and P03 index. Fail the build on an orphan,
   unresolved required reference, stale snapshot, private-path leak, or
   disagreement between static and API data.
2. **Complete Stack Explorer.** Implement an index and direct-link detail route
   for every current stack phase, plus collection/detail or contextual views
   for every public entity type. Replace sample-only fallbacks and truncated
   collections with complete data or a fail-closed error state.
3. **Evidence navigation.** From any phase or search result, a reader must be
   able to reach exact repository locations and the relevant test, benchmark,
   decision, release, and gap records without copying or inventing links.
4. **Search coverage.** Implement exact, symbol, relationship, and semantic
   search over the full indexed inventory. Generate a fixture set containing
   at least one successful and one no-result query for every entity family,
   plus dependency, source, test, gap, and release journeys. Results must show
   why they matched and their provenance.
5. **Static and responsive reading.** Direct links and server/static output
   must contain page-specific content with JavaScript disabled. Verify the
   home, stack index, every phase detail route, search results, and all public
   evidence views at 390, 768, and 1280 CSS pixels. Preserve the visual system
   established by `/practice` and the shared header/footer.
6. **Safety and performance.** Keep public routes read-only, escaped, and
   limited to intentionally public repository data. Meet the existing bundle
   budgets only after full-content coverage passes; a small bundle containing
   a partial dataset is not a pass.
7. **Regression suite and runbook.** Add the coverage, no-truncation,
   provenance, static-route, browser-journey, and security tests needed by the
   verification section. Document the generated artifacts and the exact
   sequential command order because snapshot generation writes shared files.

## Completeness gates (the anti-partial-implementation contract)

An agent may not mark P15 complete when any of the following is true:

- The implementation is described as a demo, sample, placeholder, proof of
  concept, or representative subset.
- The coverage matrix has a missing, partial, unresolved, or unreviewed row.
- A required entity or stack phase is only named in a list but has no useful
  detail/evidence path.
- A collection is truncated with `slice`, a hard-coded first-N limit, or an
  equivalent hidden cap without complete pagination coverage.
- A three-node or otherwise small fallback is used as if it were the complete
  public dataset. A fallback may exist for an explicit outage state, but the
  test must fail if the live snapshot cannot load.
- Tests inspect only headings, bundle size, or route status without exercising
  records, relationships, exact links, and direct deep links.
- Any claim lacks an exact source snapshot/commit and a reachable evidence
  record, or any “planned/missing” state is presented as implemented.

## Acceptance criteria

- [ ] The generated coverage report proves that every current stack phase and
      every public collection record has at least one reachable public view.
- [ ] Every phase detail satisfies the five required questions above and
      exposes its complete dependency/evidence/gap/release context.
- [ ] All relationship edges resolve to records or to an explicitly recorded
      and tested external source; no required edge is silently dropped.
- [ ] Static output for the stack index, every phase detail, search results,
      and evidence views remains useful with JavaScript disabled.
- [ ] Full-inventory exact, symbol, relationship, and semantic search passes
      the generated success/no-result fixture set with exact provenance.
- [ ] The no-truncation test proves there is no hidden first-N limit and that
      pagination, if used, reaches the complete inventory.
- [ ] Snapshot/API parity, freshness, source-link integrity, public-data
      safety, and escaped rendering all pass.
- [ ] Browser journeys cover silicon-to-user navigation, a phase-to-source-
      to-test journey, a gap/release journey, and search-to-evidence navigation
      at all required viewport sizes.
- [ ] Keyboard and accessibility checks pass for every route family, and
      public reading routes contain no editor/runner bundle.
- [ ] Bundle and performance budgets pass with the complete dataset loaded.
- [ ] Product, architecture, and accessibility reviewers approve the complete
      coverage report and screenshots; no acting approval is accepted without
      the named review artifacts.

## Verification

Run these commands sequentially from the repository root. Do not run snapshot
generation in parallel with the build or browser tests.

```powershell
python tools/trit_tool.py knowledge status
npm.cmd --prefix treatcode run build
npm.cmd --prefix treatcode run test:api
npm.cmd --prefix treatcode run test:public-coverage
npm.cmd --prefix treatcode run test:e2e:public-complete
npm.cmd --prefix treatcode run test:a11y
npm.cmd --prefix treatcode run test:public-security
npm.cmd --prefix treatcode run check:bundle-budget
python tools/trit_tool.py website plans validate
python tools/trit_tool.py website plan verify P15
```

The new tests are part of the implementation. If a command does not yet
exist, adding the plan without adding the test is incomplete.

## Required Evidence

- `build/treatcode-plan-evidence/P15/result.json` — machine-readable plan
  result with the complete status and command outputs.
- `build/treatcode-plan-evidence/P15/stack-coverage.json` — every stack phase,
  required field, linked route, and reviewer state.
- `build/treatcode-plan-evidence/P15/resource-coverage.json` — every public
  collection record and its reachable detail/context view.
- `build/treatcode-plan-evidence/P15/no-truncation.json` — proof that all
  records are reachable without hidden first-N limits.
- `build/treatcode-plan-evidence/P15/search-coverage.json` — generated search
  fixtures and exact provenance results.
- `build/treatcode-plan-evidence/P15/provenance-audit.json` — snapshot/API
  parity, freshness, source-link, and unresolved-reference report.
- `build/treatcode-plan-evidence/P15/static-route-e2e.json` — direct-link and
  JavaScript-disabled route assertions.
- `build/treatcode-plan-evidence/P15/public-browser-e2e.json` — complete
  browser journeys, viewport results, and screenshot manifest.
- `build/treatcode-plan-evidence/P15/accessibility.json` — keyboard,
  landmarks, names, focus, contrast, and responsive accessibility report.
- `build/treatcode-plan-evidence/P15/security.json` — public-data boundary,
  escaping, read-only, and link-safety report.
- `build/treatcode-plan-evidence/P15/bundle-budget.json` — performance and
  bundle report produced with the full dataset.
- Product, architecture, and accessibility approval records attached to the
  coverage report and screenshots.

## Completion Record

- **Verified commit:** pending
- **Evidence artifact:** pending
- **Evidence hashes:** pending
- **Human approvals:** pending
- **Date:** pending
