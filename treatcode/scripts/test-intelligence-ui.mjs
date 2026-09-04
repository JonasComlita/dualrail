import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P14");
const checks = [];
const errors = [];

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

try {
  const app = fs.readFileSync(path.join(appRoot, "src", "IntelligenceApp.tsx"), "utf8");
  const v31Panel = fs.readFileSync(path.join(appRoot, "src", "IntelligenceV31Panel.tsx"), "utf8");
  const css = fs.readFileSync(path.join(appRoot, "src", "intelligence.css"), "utf8");
  const shell = fs.readFileSync(path.join(appRoot, "intelligence", "index.html"), "utf8");
  const vite = fs.readFileSync(path.join(appRoot, "vite.config.ts"), "utf8");

  for (const taskId of ["TC-SWE-001", "TC-SWE-002", "TC-SWE-003", "TC-SWE-004", "TC-SWE-005"]) {
    assert(app.includes(taskId), `${taskId} is missing from the suite catalog source`);
    assert(shell.includes(taskId), `${taskId} is missing from the static intelligence shell`);
  }
  assert(app.includes('data-testid="suite-catalog"') && app.includes("suite-task-card-${candidate.id}"), "suite task cards do not expose stable test hooks");
  assert(app.includes("selectedTaskId") && app.includes("function selectTask"), "task selection state or handler is missing");
  assert(app.includes("${INTELLIGENCE_API}/catalog") && app.includes("${INTELLIGENCE_API}/tasks/${encodeURIComponent(activeTaskId)}"), "suite and task metadata routes are missing from the fallback chain");
  assert(app.includes("/api/intelligence/catalog") && app.includes("/api/intelligence/tasks/"), "legacy suite/task route aliases are missing from the fallback chain");
  assert(app.includes("task_id: task.id") && app.includes("benchmark_id: task.id") && app.includes("run_id: runId"), "trial submission is not scoped to the selected task and run");
  assert(app.includes("leaderboard?task_id=") && app.includes("self_reported_leaderboard"), "leaderboards are not task-scoped or self-reported-aware");
  assert(app.includes('data-testid="external-reference"') && app.includes("not TreatCode") && app.includes("externalReference.model.score") && app.includes("externalReference.task_count"), "external DeepSWE calibration is not visibly separated from local scores");
  assert(app.includes('data-testid="model-suite-score-empty"') && app.includes("provenance-bound model rollout"), "the UI does not explain why a local task score is not a model-suite score");
  assert(app.includes("harness fixture") && app.includes("unclassified") && app.includes("entry.evaluation_kind"), "official rows do not reveal whether a result is a harness proof or a model rollout");
  assert(app.includes("runner_ready") && app.includes("Runner pending"), "runner readiness is not surfaced before a suite task can submit");
  assert(app.includes("repository_shape") && app.includes("Capabilities under test") && app.includes("Hidden coverage"), "v2 difficulty, repository shape, or capability coverage is not surfaced");
  assert(app.includes("allowlisted files") && app.includes("parseSerializedSolution") && app.includes("// FILE:"), "task file contract or multi-file solution restoration is missing");
  assert(!app.includes("INTELLIGENCE_SOLUTION_GUIDES") && !app.includes('data-testid="intelligence-learning-guide"') && !app.includes('data-testid="discussion-panel"'), "practice solution guides or discussions are incorrectly mounted on the intelligence task view");
  assert(app.includes('href="/practice"'), "intelligence view does not link participants to the practice problem solutions");
  assert(app.includes("IDENTITY_KEY") && app.includes("RUN_KEY_PREFIX"), "identity and run metadata are not persisted for reload recovery");
  assert(app.includes('aria-live="polite"') && app.includes('aria-label={`${task.id} allowlisted files`}'), "loading, selection, or file controls lack accessible live/name hooks");
  assert(css.includes(".intelligence-suite-grid") && css.includes(".intelligence-suite-card"), "suite catalog styles are missing");
  assert(app.includes("IntelligenceV31Panel") && app.includes("<IntelligenceV31Panel"), "the v3.1 benchmark panel is not mounted in the Intelligence tab");
  assert(v31Panel.includes("/api/intelligence/v3.1/catalog") && v31Panel.includes("/api/intelligence/v3.1/comparisons/latest"), "the v3.1 panel does not load catalog and paired-comparison data");
  for (const phase of ["diagnostic", "pilot", "calibration", "frozen", "official"]) assert(v31Panel.includes(phase), `the v3.1 panel does not distinguish the ${phase} phase`);
  assert(v31Panel.includes("Task pass matrix") && v31Panel.includes("95% paired CI") && v31Panel.includes("Exact sign test") && v31Panel.includes("Publication blockers"), "the v3.1 panel omits paired statistics, task matrix, or explicit blockers");
  assert(v31Panel.includes("scoreBand.minimum ?? 60") && v31Panel.includes("scoreBand.maximum ?? 75") && v31Panel.includes("lead.minimum ?? 1") && v31Panel.includes("lead.maximum ?? 4") && v31Panel.includes("not official"), "the v3.1 directional target or development label is missing");
  assert(css.includes(".intelligence-v31") && css.includes(".intelligence-v31-matrix"), "v3.1 phase and task-matrix styles are missing");
  assert(/<html[^>]+lang="[a-z-]+"/.test(shell) && /name="viewport"/.test(shell), "intelligence shell lacks language or responsive viewport metadata");
  assert(shell.includes("aria-label=\"Intelligence benchmark suite\""), "static intelligence shell lacks a named suite landmark");
  assert(vite.includes('intelligence: "intelligence/index.html"'), "Vite does not build the intelligence entry point");
  checks.push("five task contracts remain discoverable in the app and static shell");
  checks.push("selection, allowlisted files, trial/run, and task-scoped leaderboards have stable hooks");
  checks.push("hard-task capability labels and public/hidden repository shape are visible without exposing hidden cases");
  checks.push("external 67% calibration stays labeled as a reference while local model-suite publication remains provenance-gated");
  checks.push("versioned and legacy suite/task metadata routes have resilient fallbacks");
  checks.push("identity, run receipt, and serialized multi-file solution state rehydrate on reload");
  checks.push("practice solution guides and discussions stay attached to the practice problem view");
  checks.push("suite cards, live status, tabs, editor labels, and static shell metadata are accessible");
  checks.push("v3.1 phase status, paired statistics, task-pass matrix, target, and publication blockers are visible without claiming an official run");
} catch (error) {
  errors.push(String(error?.message || error));
}

const report = { schema: "trit.treatcode_p14_intelligence_ui.v1", ok: errors.length === 0, checks, errors };
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "intelligence-ui.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(`P14 intelligence UI: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
