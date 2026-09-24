# Ternary Intelligence pilot

This is the new 36-family benchmark at `/intelligence`, labeled **Pilot · Windows-native execution**. Correctness is binary and independent of spending. Dollar ceilings and pricing are optional; time, token, and tool limits define comparable protocol profiles.

## Local operation

Run from `treatcode/` with Bun and the repository's Windows Trit compiler available:

```powershell
npm run build:ternary-worker
npm run test:ternary
npm run qualify:ternary
npm run build
npm start
```

The default private store is `build/ternary-intelligence/` at the repository root. `TI_DATA_ROOT` can select another private directory. Do not place it in website source or static asset directories. SQLite and content-addressed objects are private; only explicit publications are public. The CLI qualifier and server share a process lease, so stop the server before CLI qualification or use the authenticated lab action.

### Operator identity

Stop the server. Set `TI_OPERATOR_PASSWORD` in the local process environment, then run `npm run setup:ternary-operator -- chosen_handle`. Restart the server and sign in at `/account`. Setup never prints the password or replaces an existing identity. Use the same `TREATCODE_AUTH_STATE_PATH` as the server when overriding account storage. Demo and participant accounts do not acquire operator privileges.

### Model configuration

The initial preselected cohort uses the local Codex harness: `gpt-6-astra`, `gpt-5.6-sol`, `gpt-5.6-terra`, and `gpt-5.6-luna`, each with `reasoning.effort: high`. The lab supplies these defaults. Sign in locally, then run `npm run verify:ternary-harness` with the server stopped. This checks the four exact configurations with two non-scored requests each. It uses the existing account; no API key needs to be pasted into the website or chat.

Each harness episode gets a fresh ephemeral session with no project, filesystem, shell, browser, or delegation tools. Structured actions go through the benchmark worker. Learning rounds retain that episode's context. The protocol records the CLI version, resolved model, effective settings, actual usage, and transport limitations. Harness input usage is available after a turn, not as an advance count; over-budget answers fail. Output enforcement combines reported usage with the harness rollout budget. Subscription dollar cost is unavailable. Harness and direct API experiments use separate protocol profiles.

For later API experiments, set provider keys in local server environment variables. In Operator Lab, edit the configuration JSON. Every API entry has:

```json
{
  "id": "chosen-configuration-name",
  "provider": "openai",
  "model": "REPLACE_WITH_EXACT_PROVIDER_MODEL_ID",
  "keyEnv": "OPENAI_API_KEY",
  "settings": {},
  "supportedSettings": {},
  "maxOutputTokens": 24000,
  "pricing": null
}
```

API providers are `openai`, `anthropic`, and `google`; `codex` selects the local harness. The surrounding object contains `models`, `calibrationModelIds`, and `calibrationTiers`. The four-model preset assigns Luna to `lower`, Terra to `middle`, and Sol/Astra to `higher`; these are provisional preselection labels, not benchmark grades. Settings use dotted names such as `reasoning.effort`, with explicitly supported values in `supportedSettings`. Run compatibility checks before evaluation. These send two non-scored requests using the exact configuration, with and without offered tools, and record model identities and usage. API requests can incur charges; harness requests consume account usage. Unsupported settings fail this check. No host retry follows an ambiguous request; a reported harness inference error or retry invalidates the episode.

If supplying pricing, use a dated snapshot with `snapshot`, `date`, `inputUsdPerMillion`, and `outputUsdPerMillion`. Costs are estimates under that snapshot, not provider invoices. A dollar ceiling requires pricing; leaving it blank does not change correctness. Time/token/tool exhaustion remains a model failure, whereas transport and worker failures leave coverage incomplete.

### Release sequence

1. Qualify references, starters, and targeted incorrect solutions for all 36 families.
2. Verify the four preselected harness configurations at high reasoning effort.
3. Run `bun scripts/run-ternary-calibration.ts default` (432 episodes), then `bun scripts/run-ternary-calibration.ts controlled` (864 episodes), using the same resource limits. Both commands share the server's private store and process lease. Progress prints every 30 seconds; completed evidence survives restart and exposed interrupted episodes are not replayed.
4. Review ambiguity, repeatability, condition differences, and floor/ceiling behavior. More than 25% universally passed/failed families requires corpus revision and recalibration.
5. Freeze the accepted revision and run scored evaluations: 36 episodes per model by default, or 72 per model for the controlled experiment.
6. Inspect evidence and publish only genuine, complete results. Fixtures and calibration runs are ineligible.

Public examples and calibration-exposed pilot tasks cannot later be described as an unseen evaluation set. Changes to task/checker content, prompts, orchestration, adapters, assignments, seeds, worker/compiler, or resource limits invalidate matching against the frozen protocol.

## Evidence and current limits

As of 2026-09-24:

- Production build and TypeScript checks passed. The public privacy audit checked 190 built text assets and exports without finding private corpus or evaluator content.
- All four selected harness models passed live compatibility verification with explicit Trit syntax and two distinct nonzero executable answers. A separate two-turn Luna check requested a structured tool, consumed its result, and submitted code that passed the native grader. These are non-scored readiness checks, not intelligence grades.
- All 36 families passed native executable qualification. Latest report hash: `e90538dc313de02bdea4fefb40069e14e47eb735f7a56ba57487966a89c0386c` (stored in the private database and objects).
- Foundation/control tests cover SQLite recovery, idempotence, cost reservations, immutable publication, provider continuations, learning stages/context reset, tool restrictions, role/grant checks, binary failures, and family-cluster bootstrap behavior.
- Latest focused verification: `npm run test:ternary` passed 34 tests across seven files (446 assertions); TypeScript checks passed.
- Native tests passed for time, output, memory, process count, descendant cleanup, parent-process exit, cancellation, and recorded public coding examples.
- Browser checks verified public examples, invalid input feedback, keyboard navigation, empty Results, restricted Operator Lab, and 390/1280-pixel layouts. An isolated operator fixture exercised starting a run, refresh/reconnection, progress, evidence inspection, cancellation, and blocked fixture publication.
- Shared participant, Learn, and Practice regression checks passed, including native exercise validation, drafts, public solutions, and voting. Reporting tests cover family-weighted learning curves and accessible numeric tables.
- Release-policy fixtures cover complete calibration, ambiguity rejection, accepted-revision identity, immutable publication, and rejection of incomplete coverage or changed limits. Operator setup tests cover persistence, grants, password handling, and refusal to replace existing identities; an underscore-to-identity-slug defect found by this check was fixed.
- Desktop and 390-pixel browser checks verified the rendered learning charts, numeric alternatives, and paired comparison table without page overflow. Rendering data stayed in an isolated local preview and was never inserted into the operator publication store.

The first live calibration attempt (`cbe9c2ad-c998-4171-b80e-786bcc73491b`) was cancelled and explicitly invalidated after traces revealed compatibility-answer instructions in the harness system prompt. Its evidence is retained. The prompt was corrected, verification strengthened and rerun, and a fresh default calibration (`5ba6a7fb-4a59-4552-bac0-b0623ece6b90`) started. Only the current protocol can satisfy the calibration gate.

Remaining delivery work includes live default and controlled calibration, ambiguity review, any corpus revisions required by the floor/ceiling gate, freeze, genuine evaluations and publication, and the final requirement-by-requirement audit. Readiness checks and deterministic fixtures do not appear as model results.

## Architecture

- `src/ternary/`: public contracts, methodology, restricted browser interpreter, examples, and React UI.
- `private/ternary/`: corpus/checkers, provider adapters, durable store, orchestration, qualification, reporting, access-controlled router, and Windows worker host.
- `tools/ternary_intelligence_job.cpp`: suspended process creation, Job Object assignment, limits, output capture, parent-exit monitoring, and descendant cleanup.
- Native participant tools expose one allowlisted source file and named public tests; hidden graders use separate private working directories.
- Each episode starts with a new provider session. Learning probes at rounds 0, 1, and 3 are disjoint from examples and feedback.
- Publications use 10,000 deterministic paired bootstrap samples with task family as the resampling unit and require matching coverage/protocols.

Provider token-counting references: [OpenAI](https://developers.openai.com/api/docs/guides/token-counting), [Anthropic](https://platform.claude.com/docs/en/api/messages/count_tokens), [Google](https://ai.google.dev/api/tokens). Native execution uses [Windows Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects); it is not represented as container isolation.
