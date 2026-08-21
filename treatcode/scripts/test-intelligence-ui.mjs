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
  assert(app.includes("allowlisted files") && app.includes("parseSerializedSolution") && app.includes("// FILE:"), "task file contract or multi-file solution restoration is missing");
  assert(app.includes("IDENTITY_KEY") && app.includes("RUN_KEY_PREFIX"), "identity and run metadata are not persisted for reload recovery");
  assert(app.includes('aria-live="polite"') && app.includes('aria-label={`${task.id} allowlisted files`}'), "loading, selection, or file controls lack accessible live/name hooks");
  assert(css.includes(".intelligence-suite-grid") && css.includes(".intelligence-suite-card"), "suite catalog styles are missing");
  assert(/<html[^>]+lang="[a-z-]+"/.test(shell) && /name="viewport"/.test(shell), "intelligence shell lacks language or responsive viewport metadata");
  assert(shell.includes("aria-label=\"Intelligence benchmark suite\""), "static intelligence shell lacks a named suite landmark");
  assert(vite.includes('intelligence: "intelligence/index.html"'), "Vite does not build the intelligence entry point");
  checks.push("five task contracts remain discoverable in the app and static shell");
  checks.push("selection, allowlisted files, trial/run, and task-scoped leaderboards have stable hooks");
  checks.push("versioned and legacy suite/task metadata routes have resilient fallbacks");
  checks.push("identity, run receipt, and serialized multi-file solution state rehydrate on reload");
  checks.push("suite cards, live status, tabs, editor labels, and static shell metadata are accessible");
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
