import { strict as assert } from "node:assert";
import { mkdtemp, rm, mkdir, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { IntelligenceSuiteService } from "../src/intelligenceService";

const repoRoot = path.resolve(import.meta.dir, "..", "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P14");
const checks: string[] = [];
const tempRoots: string[] = [];

const correctedFiles: Record<string, Record<string, string>> = {
  "TC-SWE-001": {
    "src/compare.trit": `fn compare(a: t40, b: t40) -> t40 {
    match a - b {
        neg => { return -1; }
        zero => { return 0; }
        pos => { return 1; }
    }
}`,
    "src/median.trit": `fn median(a: t40, b: t40, c: t40) -> t40 {
    if a <= b {
        if b <= c { return b; }
        if a <= c { return c; }
        return a;
    }
    if a <= c { return a; }
    if b <= c { return c; }
    return b;
}`,
  },
  "TC-SWE-002": {
    "src/parser.trit": "fn parse_serialize(token: t40) -> t40 { return token; }",
  },
  "TC-SWE-003": {
    "src/pointer_guard.trit": "fn pointer_guard(address: t40) -> t40 { if address < 0 { return -1; } if address > 7 { return -1; } return address; }",
  },
  "TC-SWE-004": {
    "src/state_transition.trit": "fn state_transition(state: t40, event: t40) -> t40 { match event { neg => { return state - 1; } zero => { return state; } pos => { return state + 1; } } }",
  },
  "TC-SWE-005": {
    "src/syscall_adapter.trit": "fn abi_return(status: t40, value: t40) -> t40 { if status < 0 { return status; } return value; }",
  },
};

const report = {
  schema: "treatcode.p14_intelligence_suite_e2e.v1",
  ok: false,
  model: "bounded Trit compiler with known-correct suite fixtures",
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
  checks.push("all five versioned task contracts are discoverable without server-only hidden cases");

  for (const task of catalog.tasks) {
    const taskId = task.task.id;
    const run = await suite.startRun({ task_id: taskId, participant_id: `suite-proof-${taskId}` });
    const taskReport: Record<string, unknown> = { task_id: taskId, run_id: run.run_id, trial_ids: [], score: null };
    const trialIds: string[] = [];
    for (const trial of run.trials) {
      trialIds.push(trial.id);
      for (const [relative, source] of Object.entries(correctedFiles[taskId])) {
        await suite.writeFile(run.run_id, trial.id, relative, source);
      }
      const publicReport = await suite.runPublicTests(run.run_id, trial.id);
      assert(publicReport.passed, `${taskId} public gate failed: ${JSON.stringify(publicReport)}`);
      const receipt = await suite.submitHidden(run.run_id, trial.id);
      assert.equal(receipt.accepted, true);
      assert.equal(receipt.sealed, true);
    }
    const attestation = await suite.attest({ run_id: run.run_id, principal: "tc:identity:service-suite", model: "bounded-suite-proof", model_configuration: "known-correct fixture" });
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
  checks.push("all five tasks execute through the bounded compiler, complete four sealed trials, and publish 100-point official aggregates");

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
