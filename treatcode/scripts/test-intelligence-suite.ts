import { strict as assert } from "node:assert";
import { mkdtemp, rm, mkdir, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { IntelligenceSuiteService } from "../src/intelligenceService";
import { correctedIntelligenceFiles } from "./intelligence-corrected-fixtures";

const repoRoot = path.resolve(import.meta.dir, "..", "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P14");
const checks: string[] = [];
const tempRoots: string[] = [];

const report = {
  schema: "treatcode.p14_intelligence_suite_e2e.diagnostic.v3.1",
  ok: false,
  evaluation: { kind: "harness_fixture", model_generated: false, harness: "known-correct-suite-fixtures" },
  trial_protocol: "five tasks × four fresh trials × one sealed hidden submission per trial",
  tasks: [] as Array<Record<string, unknown>>,
  checks,
  errors: [] as string[],
};

try {
  const storageRoot = await mkdtemp(path.join(os.tmpdir(), "treatcode-intelligence-suite-e2e-"));
  tempRoots.push(storageRoot);
  const suite = new IntelligenceSuiteService({ storageRoot });
  const catalog = suite.suiteCatalog();
  assert.equal(catalog.tasks.length, 5);
  assert.deepEqual(catalog.tasks.map((entry) => entry.task.id), ["TC-SWE-001", "TC-SWE-002", "TC-SWE-003", "TC-SWE-004", "TC-SWE-005"]);
  assert(catalog.tasks.every((entry) => entry.version === 3.1 && entry.task.difficulty === "hard"));
  assert.equal(catalog.tasks.reduce((total, entry) => total + entry.task.repository_shape.hidden_cases, 0), 140);
  assert.equal(catalog.tasks.reduce((total, entry) => total + entry.task.repository_shape.editable_files, 0), 12);
  checks.push("five hard diagnostic contracts expose 12 editable files and 140 hidden-case counts without server-only values");

  for (const task of catalog.tasks) {
    const taskId = task.task.id;
    const run = await suite.startRun({ task_id: taskId, participant_id: `suite-proof-${taskId}` });
    const taskReport: Record<string, unknown> = { task_id: taskId, difficulty: task.task.difficulty, capabilities: task.task.capabilities, repository_shape: task.task.repository_shape, run_id: run.run_id, trial_ids: [], score: null };
    const trialIds: string[] = [];
    for (const trial of run.trials) {
      trialIds.push(trial.id);
      for (const [relative, source] of Object.entries(correctedIntelligenceFiles[taskId])) {
        await suite.writeFile(run.run_id, trial.id, relative, source);
      }
      const publicReport = await suite.runPublicTests(run.run_id, trial.id);
      assert(publicReport.passed, `${taskId} public gate failed: ${JSON.stringify(publicReport)}`);
      const receipt = await suite.submitHidden(run.run_id, trial.id);
      assert.equal(receipt.accepted, true);
      assert.equal(receipt.sealed, true);
    }
    const attestation = await suite.attest({ run_id: run.run_id, principal: "tc:identity:service-suite", evaluation_kind: "harness_fixture", provider: "treatcode", model: "bounded-suite-proof", reasoning_effort: "not-applicable", harness: "known-correct-suite-fixtures", model_configuration: "known-correct fixture" });
    assert(attestation.evidence_hash.startsWith("sha256:"));
    const aggregate = await suite.getAggregate(run.run_id);
    assert("score" in aggregate && aggregate.score === 100, `${taskId} did not score 100`);
    assert("official_eligible" in aggregate && aggregate.official_eligible === true, `${taskId} is not official eligible`);
    taskReport.trial_ids = trialIds;
    taskReport.score = aggregate.score;
    taskReport.completed_trials = aggregate.completed_trials;
    taskReport.passed_trials = aggregate.passed_trials;
    taskReport.official_eligible = aggregate.official_eligible;
    taskReport.attested = aggregate.published;
    report.tasks.push(taskReport);
  }
  assert.equal(suite.getOfficialLeaderboard().length, 5);
  checks.push("560 hidden case executions complete through the bounded compiler and publish explicitly harness-scoped aggregates; no model score is inferred");

  const restarted = new IntelligenceSuiteService({ storageRoot });
  assert.equal(restarted.getOfficialLeaderboard().length, 5);
  assert.equal(new Set(restarted.getOfficialLeaderboard().map((entry) => entry.task_id)).size, 5);
  checks.push("task-scoped official leaderboard records survive a suite service restart");
  report.ok = true;
} catch (error) {
  report.errors.push(String(error instanceof Error ? error.stack || error.message : error));
} finally {
  await mkdir(evidenceRoot, { recursive: true });
  await writeFile(path.join(evidenceRoot, "intelligence-suite-e2e.json"), `${JSON.stringify(report, null, 2)}\n`, "utf8");
  await Promise.all(tempRoots.map((root) => rm(root, { recursive: true, force: true })));
}

console.log(`P14 intelligence suite E2E: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of report.errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
