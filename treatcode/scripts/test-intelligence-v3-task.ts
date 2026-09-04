import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { IntelligenceV3TaskExecutor } from "../src/intelligenceV3Executor";

const repositoryRoot = path.resolve(import.meta.dir, "..", "..");
const privateBase = process.env.TREATCODE_V3_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v3-private");
const taskId = process.argv.find((value) => value.startsWith("--task="))?.slice("--task=".length) || "TC-V3-001";
if (!/^TC-V3-\d{3}$/.test(taskId)) throw new Error("--task must be a TC-V3-NNN identifier");
const graderRoot = path.join(privateBase, taskId);
const artifactRoot = path.join(repositoryRoot, "build", "treatcode-intelligence-v3-runner");
const useCalibrationFixture = process.argv.includes("--calibration-fixture");
const calibrationBase = process.env.TREATCODE_V3_CALIBRATION_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v3-calibration-fixtures");
const subjectWorkspaceRoot = useCalibrationFixture ? calibrationBase : repositoryRoot;
const participantRoot = useCalibrationFixture
  ? path.join(calibrationBase, taskId)
  : path.join(repositoryRoot, "benchmarks", "intelligence-v3", "tasks", taskId, "participant");

const executor = await IntelligenceV3TaskExecutor.create({
  repository_root: repositoryRoot,
  subject_workspace_root: subjectWorkspaceRoot,
  participant_root: participantRoot,
  grader_root: graderRoot,
  artifact_root: artifactRoot,
});

const publicReport = await executor.runPublic();
const hiddenReport = await executor.submitHidden();
if (useCalibrationFixture && !publicReport.passed) console.error(JSON.stringify(publicReport, null, 2));
assert.equal(publicReport.task_id, taskId);
assert.equal(hiddenReport.task_id, taskId);
assert.equal(hiddenReport.sealed, true);
assert.ok(hiddenReport.total_cases >= 10);
assert.ok(hiddenReport.adversarial_cases_total > 0);
assert.equal(hiddenReport.passed, useCalibrationFixture, useCalibrationFixture
  ? "the private known-good calibration fixture must pass every hidden case"
  : "the intentionally defective starter must not pass the private grader");
await assert.rejects(() => executor.submitHidden(), /exactly one hidden submission/);

const report = {
  schema: "treatcode.intelligence.v3-task-development-check.v1",
  task_id: publicReport.task_id,
  fixture: useCalibrationFixture ? "private-known-good" : "participant-starter",
  starter_public_passed: publicReport.passed,
  starter_public_passes: publicReport.cases.filter((item) => item.passed).length,
  starter_public_total: publicReport.cases.length,
  starter_hidden_passed: hiddenReport.passed,
  starter_hidden_passes: hiddenReport.passed_cases,
  starter_hidden_total: hiddenReport.total_cases,
  required_suites_passed: hiddenReport.required_suites_passed,
  required_suites_total: hiddenReport.required_suites_total,
  adversarial_passes: hiddenReport.adversarial_cases_passed,
  adversarial_total: hiddenReport.adversarial_cases_total,
  evidence_hash: hiddenReport.evidence_hash,
  grader_root_outside_subject_workspace: true,
  one_shot_enforced: true
};
const taskSlug = taskId.toLowerCase().replace(/[^a-z0-9]+/g, "-");
const evidencePath = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", `intelligence-v3-${taskSlug}-${useCalibrationFixture ? "known-good" : "starter"}.json`);
await mkdir(path.dirname(evidencePath), { recursive: true });
await writeFile(evidencePath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
console.log(JSON.stringify({ ...report, evidence: evidencePath }, null, 2));
