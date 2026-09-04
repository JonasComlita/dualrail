# Follow-up prompt for the original P04 agent

You are resuming work on the TreatCode public API and Stack Explorer in
`C:\Users\jonas\Documents\trit`.

Your previous P04 result was a valid first draft, but it was not a complete
implementation of the intended user-facing goal. Treat the old P04 completion
record as historical v1 evidence only. The current execution authority is
`docs/11_TreatCode_Platform/plans/P15_P04_completeness_amendment.md`.

## What was incomplete

The previous work demonstrated that the API, generated snapshot, public routes,
provenance fields, and visual/performance checks could work. It did not prove
that a reader could inspect the complete repository knowledge model. In
particular:

- The acceptance criteria did not require an exhaustive coverage matrix for
  every stack phase, public entity, and relationship.
- The tests proved schema/route presence and a few known queries, but did not
  traverse every record, relationship, direct deep link, or evidence path.
- A broad snapshot can coexist with an incomplete UI. A list capped at the
  first N records, an abbreviated detail page, or a small fallback dataset is
  still incomplete even when the API contains more data.
- The no-JavaScript checks did not establish that every stack/evidence route
  contained useful page-specific content.
- The bundle and responsive checks measured the shell; they did not establish
  that the full knowledge inventory was present and usable.

Do not defend the old result by pointing to passing v1 tests. Those tests were
too weak to detect the missing completeness work.

## Mission

Complete P15, not another sample or proof-of-function slice. A reader must be
able to move from the ternary/hardware boundary through ISA, VM, compiler,
runtime, boot, kernel, storage, devices, user space, images, and system
closure, reaching exact source, symbol, contract, capability, test, benchmark,
decision, release, and known-gap evidence for every claim.

“Complete” means complete coverage of the current repository knowledge model.
It does not mean pretending that future hardware or OS work is implemented;
planned and unavailable capabilities must remain visible as explicitly
labelled gaps.

## Required work

1. Read `P15_P04_completeness_amendment.md`, `PLAN_INDEX.md`, the P02/P03
   registries/index, the OpenAPI contract, the generated snapshot, and the
   current Stack Explorer implementation before changing code.
2. Generate a machine-readable coverage matrix from the source-of-truth
   snapshot/registries. It must include every current stack node (currently 21),
   every public collection record (`projects`, `stack_nodes`, `components`,
   `capabilities`, `contracts`, `decisions`, `sources`, `symbols`, `tests`,
   `benchmarks`, `runs`, `releases`, and `gaps`), and every relationship edge.
3. Give every stack phase a useful direct-link detail view answering: what it
   does, what enters/leaves it, dependencies, implementation status, exact
   source/evidence/test/benchmark/decision/release/gap links, and what to open
   next. Do not satisfy this by displaying only a phase name or count.
4. Make every public record reachable. Remove hidden first-N limits and
   `slice`-style truncation, or implement deterministic pagination with a
   visible path to every page. A fallback may communicate an outage, but it
   must never be counted as a complete dataset and the test suite must fail
   when the live snapshot cannot load.
5. Implement full-inventory exact, symbol, relationship, and semantic search.
   Generate fixtures for each entity family plus dependency, source, test,
   gap, and release journeys; show match reason and provenance.
6. Make direct links and static output useful with JavaScript disabled for the
   stack index, every phase detail, search results, and evidence views. Keep
   public routes read-only, escaped, safe, and free of editor/runner code.
7. Preserve the visual language of `/practice` and the shared header/footer,
   while verifying 390, 768, and 1280 CSS-pixel layouts, keyboard behavior,
   focus, accessible names, landmarks, contrast, and overflow.
8. Add the missing tests rather than weakening the requirement. The required
   commands are:

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

   If a required script does not exist, implement it and include its report;
   do not remove it from the plan.

## Do not claim completion when

- only a few representative pages or entities are rendered;
- the 21-phase inventory exists in data but not in usable detail/evidence
  routes;
- any public collection is silently truncated;
- tests inspect only headings, route status, bundle size, or one known query;
- a source link substitutes for an explanation or an unresolved reference is
  hidden;
- a small fallback passes while the real snapshot is unavailable;
- the work is called a demo, sample, placeholder, or proof of concept;
- acting approval is used instead of named review artifacts.

## Required completion proof

Before reporting success, write all P15 evidence artifacts, including the
stack/resource coverage matrices, no-truncation report, search report,
provenance audit, static-route report, full browser journeys/screenshots,
accessibility report, security report, and full-dataset bundle report under
`build/treatcode-plan-evidence/P15/`. Obtain named product, architecture, and
accessibility approvals. Leave P15 `in_progress` if any row is missing,
partial, unresolved, or unreviewed.

Your final response must list the implementation files changed, the exact
coverage counts, the commands and exit codes, the evidence paths, and any
remaining gap. Do not report “complete” from the old P04 result.

