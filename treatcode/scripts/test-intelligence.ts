import { strict as assert } from "node:assert";
import { symlink, writeFile, mkdtemp, rm } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import {
  IntelligenceBenchmarkService,
  IntelligenceSuiteService,
  IntelligenceServiceError,
  type IntelligenceExecutionInput,
} from "../src/intelligenceService";
import { correctedIntelligenceFiles } from "./intelligence-corrected-fixtures";

const repoRoot = path.resolve(import.meta.dir, "..", "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P14");
const tempRoots: string[] = [];
const checks: string[] = [];

function expectedExecutor(mode: "correct" | "timeout" = "correct") {
  return async (input: IntelligenceExecutionInput) => {
    if (mode === "timeout" && input.visibility === "hidden") return { success: false, state: "timed_out" as const, timed_out: true };
    return { success: true, state: "succeeded" as const, value: input.test.expected };
  };
}

async function makeService(mode: "correct" | "timeout" = "correct") {
  const root = await mkdtemp(path.join(os.tmpdir(), "treatcode-intelligence-test-"));
  tempRoots.push(root);
  return new IntelligenceBenchmarkService({ storageRoot: root, artifactRoot: path.join(root, "evidence"), executor: expectedExecutor(mode) });
}

async function fixTrial(service: IntelligenceBenchmarkService, runId: string, trialId: string): Promise<void> {
  for (const [relative, source] of Object.entries(correctedIntelligenceFiles["TC-SWE-001"])) {
    await service.writeFile(runId, trialId, relative, source);
  }
}

async function expectCode(action: Promise<unknown>, code: string): Promise<void> {
  try {
    await action;
    assert.fail(`expected ${code}`);
  } catch (error) {
    assert(error instanceof IntelligenceServiceError, `expected IntelligenceServiceError, got ${String(error)}`);
    assert.equal(error.code, code);
  }
}

const report = {
  schema: "treatcode.p14_intelligence_tests.v1",
  ok: false,
  checks,
  errors: [] as string[],
};

try {
  const service = await makeService();
  const catalog = service.catalog();
  const catalogJson = JSON.stringify(catalog);
  assert.equal(catalog.task.id, "TC-SWE-001");
  assert.equal(catalog.task.trial_count, 4);
  assert.deepEqual(catalog.task.allowlisted_files.map((item) => item.path), ["src/validation.trit", "src/compare.trit", "src/median.trit", "src/consensus.trit"]);
  assert.equal(catalog.task.difficulty, "hard");
  assert.equal(catalog.task.repository_shape.editable_files, 4);
  assert.equal(catalog.task.repository_shape.hidden_cases, 28);
  assert.equal(catalog.task.repository_shape.hidden_suites, 4);
  assert.equal(catalog.hidden_tests.server_only, true);
  assert.equal(catalog.hidden_tests.sealed, true);
  assert.equal(catalog.task.score_scope, "task_trial_reliability");
  assert.equal(catalog.task.runner_ready, true);
  assert(!catalogJson.includes("three-neg-mixed"), "hidden case IDs must not enter the public catalog");
  assert(!catalogJson.includes("missing-edge-average"), "hidden case IDs must not enter the public catalog");
  checks.push("versioned TC-SWE-001 catalog exposes four allowlisted files, difficulty metadata, and public data only");

  const run = await service.startRun({ participant_id: "luna" });
  assert.equal(run.trials.length, 4);
  assert.equal(new Set(run.trials.map((trial) => trial.files.find((file) => file.path === "src/compare.trit")?.sha256)).size, 1);
  await expectCode(service.submitHidden(run.run_id, run.trials[0].id), "public_gate_required");
  checks.push("every run materializes four independent clean fixtures and hidden submission has a public gate");

  await expectCode(service.writeFile(run.run_id, run.trials[0].id, "../outside.trit", "bad"), "path_traversal");
  await expectCode(service.writeFile(run.run_id, run.trials[0].id, "src/other.trit", "bad"), "path_not_allowed");
  await expectCode(service.writeFile(run.run_id, run.trials[0].id, "src/compare.trit", "x".repeat(65_537)), "file_too_large");
  checks.push("traversal, non-allowlisted paths, and oversized writes are rejected");

  await fixTrial(service, run.run_id, run.trials[0].id);
  const publicFirst = await service.runPublicTests(run.run_id, run.trials[0].id);
  const publicSecond = await service.runPublicTests(run.run_id, run.trials[0].id);
  assert(publicFirst.passed && publicSecond.passed);
  assert.equal(publicSecond.attempt, 2);
  checks.push("public tests are repeatable and report only public expected values");

  const firstReceipt = await service.submitHidden(run.run_id, run.trials[0].id);
  assert.equal(firstReceipt.accepted, true);
  assert.equal(firstReceipt.sealed, true);
  assert.equal(firstReceipt.aggregate, null);
  await expectCode(service.submitHidden(run.run_id, run.trials[0].id), "hidden_submission_already_recorded");
  await expectCode(service.writeFile(run.run_id, run.trials[0].id, "src/median.trit", correctedIntelligenceFiles["TC-SWE-001"]["src/median.trit"]), "hidden_submission_sealed");
  assert(!JSON.stringify(firstReceipt).includes("three-neg-mixed"));
  checks.push("hidden submission is one-shot per trial, seals writes, and withholds hidden outcomes");

  for (const trial of run.trials.slice(1)) {
    await fixTrial(service, run.run_id, trial.id);
    assert((await service.runPublicTests(run.run_id, trial.id)).passed);
    const receipt = await service.submitHidden(run.run_id, trial.id);
    assert.equal(receipt.accepted, true);
  }
  const aggregate = await service.getAggregate(run.run_id);
  assert("score" in aggregate && aggregate.score === 100);
  assert("sealed" in aggregate && aggregate.sealed === true);
  assert("official_eligible" in aggregate && aggregate.official_eligible === true);
  assert("published" in aggregate && aggregate.published === false);
  assert.equal(service.getOfficialLeaderboard().length, 0);
  await expectCode(service.attest({
    run_id: run.run_id,
    principal: "tc:identity:service-p14",
    evaluation_kind: "model_rollout",
    provider: "openai",
    model: "gpt-5.6-luna",
    reasoning_effort: "max",
    harness: "unverified",
    prompt_hash: "sha256:" + "a".repeat(64),
    rollout_ids: run.trials.map((trial) => `rollout-${trial.id}`),
    artifact_hashes: run.trials.map(() => "sha256:" + "b".repeat(64)),
  }), "attestation_required");
  checks.push("a clean fixture run cannot be relabeled as gpt-5.6-luna: the live service requires an external provenance attestor for model claims");
  const publicationAttestation = await service.attest({ run_id: run.run_id, principal: "tc:identity:service-p14", evaluation_kind: "harness_fixture", provider: "treatcode", model: "bounded-service-fixture", reasoning_effort: "not-applicable", harness: "state-machine-test-fixture", model_configuration: "known-correct fixture", evidence_hashes: ["sha256:p14-intelligence-test"] });
  assert(publicationAttestation.evidence_hash.startsWith("sha256:"));
  assert.equal(service.getOfficialLeaderboard().length, 1);
  assert.equal(service.getOfficialLeaderboard()[0]?.attested, true);
  service.recordSelfReported({ participant_id: "luna", score: 100, note: "local replay" });
  const restarted = new IntelligenceBenchmarkService({ storageRoot: service.storageRoot, artifactRoot: path.join(service.storageRoot, "evidence-restart"), executor: expectedExecutor() });
  assert.equal(restarted.getOfficialLeaderboard().length, 1);
  assert.equal(restarted.getSelfReportedLeaderboard().length, 1);
  const persistedState = JSON.stringify(restarted.getOfficialLeaderboard()) + JSON.stringify(restarted.getSelfReportedLeaderboard());
  assert(!persistedState.includes("three-neg-mixed"));
  checks.push("atomic state persistence reloads official and self-reported summaries after a service restart without hidden data");
  checks.push("four sealed clean trials expose a sealed score, and only a privileged attestation publishes the official leaderboard record");

  const attested = new IntelligenceBenchmarkService({
    storageRoot: path.join(service.storageRoot, "attested"),
    executor: expectedExecutor(),
    privilegedAttestor: ({ request }) => request.principal === "operator:p14",
  });
  const attestedRun = await attested.startRun({ participant_id: "attested" });
  for (const trial of attestedRun.trials) {
    await fixTrial(attested, attestedRun.run_id, trial.id);
    await attested.runPublicTests(attestedRun.run_id, trial.id);
    await attested.submitHidden(attestedRun.run_id, trial.id);
  }
  await expectCode(attested.attest({
    run_id: attestedRun.run_id,
    principal: "operator:p14",
    evaluation_kind: "model_rollout",
    provider: "openai",
    model: "gpt-5.6-luna",
    reasoning_effort: "max",
    harness: "attestation-test-fixture",
    prompt_hash: "sha256:" + "c".repeat(64),
    rollout_ids: attestedRun.trials.map((trial) => `rollout-${trial.id}`),
    artifact_hashes: attestedRun.trials.map((_, index) => "sha256:" + String.fromCharCode(99 + index).repeat(64)),
  }), "attestation_rejected");
  const attestation = await attested.attest({ run_id: attestedRun.run_id, principal: "operator:p14", evaluation_kind: "harness_fixture", provider: "treatcode", model: "bounded-service-fixture", reasoning_effort: "not-applicable", harness: "attestation-test-fixture", model_configuration: "known-correct fixture" });
  assert(attestation.evidence_hash.startsWith("sha256:"));
  assert.equal(attested.getOfficialLeaderboard()[0]?.attested, true);
  const attestedRestart = new IntelligenceBenchmarkService({ storageRoot: attested.storageRoot, executor: expectedExecutor() });
  assert.equal(attestedRestart.getOfficialLeaderboard()[0]?.attested, true);
  checks.push("privileged attestation hook records model/configuration evidence without exposing verifier cases");

  // Exercise the model-rollout publication contract with a synthetic external
  // attestor. The fixture executor is still deterministic; this proves only
  // that provider/session/prompt/artifact metadata is bound before a model row
  // could be published. It is not a Luna capability measurement.
  const modelProof = new IntelligenceBenchmarkService({
    storageRoot: path.join(service.storageRoot, "model-proof"),
    executor: expectedExecutor(),
    privilegedAttestor: ({ request }) => request.evaluation_kind === "model_rollout" && request.provider === "openai" && request.model === "gpt-5.6-luna" && request.reasoning_effort === "max" && request.harness === "synthetic-external-attestor",
  });
  const modelPromptHash = "sha256:" + "e".repeat(64);
  const modelRun = await modelProof.startRun({
    participant_id: "synthetic-model-proof",
    evaluation_kind: "model_rollout",
    provider: "openai",
    model: "gpt-5.6-luna",
    reasoning_effort: "max",
    harness: "synthetic-external-attestor",
    prompt_hash: modelPromptHash,
    rollout_ids: ["rollout-a", "rollout-b", "rollout-c", "rollout-d"],
    artifact_hashes: ["sha256:" + "f".repeat(64), "sha256:" + "0".repeat(64), "sha256:" + "1".repeat(64), "sha256:" + "2".repeat(64)],
  });
  for (const trial of modelRun.trials) {
    await fixTrial(modelProof, modelRun.run_id, trial.id);
    await modelProof.runPublicTests(modelRun.run_id, trial.id);
    await modelProof.submitHidden(modelRun.run_id, trial.id);
  }
  const modelAttestation = await modelProof.attest({
    run_id: modelRun.run_id,
    principal: "operator:p14",
    evaluation_kind: "model_rollout",
    provider: "openai",
    model: "gpt-5.6-luna",
    reasoning_effort: "max",
    harness: "synthetic-external-attestor",
    prompt_hash: modelPromptHash,
    rollout_ids: ["rollout-a", "rollout-b", "rollout-c", "rollout-d"],
    artifact_hashes: ["sha256:" + "f".repeat(64), "sha256:" + "0".repeat(64), "sha256:" + "1".repeat(64), "sha256:" + "2".repeat(64)],
    model_configuration: "synthetic contract test; not model-generated evidence",
  });
  assert.equal(modelAttestation.evaluation_kind, "model_rollout");
  assert.equal(modelProof.getOfficialLeaderboard()[0]?.model, "gpt-5.6-luna");
  checks.push("model-rollout rows require exact provenance binding; synthetic attestation coverage is labeled as a contract test, not a capability score");

  const timedOut = await makeService("timeout");
  const timedOutRun = await timedOut.startRun({ participant_id: "timeout" });
  for (const trial of timedOutRun.trials) {
    await fixTrial(timedOut, timedOutRun.run_id, trial.id);
    await timedOut.runPublicTests(timedOutRun.run_id, trial.id);
    await timedOut.submitHidden(timedOutRun.run_id, trial.id);
  }
  const timedOutAggregate = await timedOut.getAggregate(timedOutRun.run_id);
  assert("official_eligible" in timedOutAggregate && timedOutAggregate.official_eligible === false);
  assert.equal(timedOut.getOfficialLeaderboard().length, 0);
  checks.push("timed-out hidden trials seal but cannot enter the official leaderboard");

  const tampered = await makeService();
  const tamperedRun = await tampered.startRun({ participant_id: "tampered" });
  const internal = (tampered as unknown as { runs: Map<string, { trials: Map<string, { workspaceRoot: string }> }> }).runs;
  const tamperedTrial = internal.get(tamperedRun.run_id)!.trials.get(tamperedRun.trials[0].id)!;
  const target = path.join(tamperedTrial.workspaceRoot, "src", "compare.trit");
  await rm(target);
  try {
    await symlink(path.join(tamperedTrial.workspaceRoot, "src", "median.trit"), target);
    await expectCode(tampered.runPublicTests(tamperedRun.run_id, tamperedRun.trials[0].id), "trial_tampered");
    assert.equal((await tampered.getRun(tamperedRun.run_id)).status, "tampered");
    checks.push("symlink tampering is detected before execution and marks the run ineligible");
  } catch (error) {
    // Windows developer shells may deny symlink creation without SeCreateSymbolicLinkPrivilege.
    if ((error as NodeJS.ErrnoException).code !== "EPERM" && (error as NodeJS.ErrnoException).code !== "EACCES") throw error;
    checks.push("symlink tamper probe skipped because the host denied test symlink creation");
  }

  const incomplete = await makeService();
  const incompleteRun = await incomplete.startRun({ participant_id: "incomplete" });
  for (const trial of incompleteRun.trials.slice(0, 3)) {
    await fixTrial(incomplete, incompleteRun.run_id, trial.id);
    await incomplete.runPublicTests(incompleteRun.run_id, trial.id);
    await incomplete.submitHidden(incompleteRun.run_id, trial.id);
  }
  const incompleteAggregate = await incomplete.getAggregate(incompleteRun.run_id);
  assert.equal(incompleteAggregate.sealed, false);
  assert.equal(incompleteAggregate.score, null);
  assert.equal(incomplete.getOfficialLeaderboard().length, 0);
  const selfReported = incomplete.recordSelfReported({ participant_id: "incomplete", score: 75, note: "local estimate" });
  assert.equal(selfReported.view, "self-reported");
  assert.equal(incomplete.getOfficialLeaderboard().length, 0);
  assert.equal(incomplete.getSelfReportedLeaderboard().length, 1);
  checks.push("incomplete runs stay unpublished and self-reported records remain a separate leaderboard view");

  // One focused integration check exercises the real bounded worker/compiler
  // path. The lifecycle checks above inject a deterministic adapter to remain
  // fast and deterministic.
  const realRoot = await mkdtemp(path.join(os.tmpdir(), "treatcode-intelligence-real-test-"));
  tempRoots.push(realRoot);
  const real = new IntelligenceBenchmarkService({ storageRoot: realRoot, artifactRoot: path.join(realRoot, "evidence") });
  const realRun = await real.startRun({ participant_id: "real-runner" });
  await fixTrial(real, realRun.run_id, realRun.trials[0].id);
  const realPublic = await real.runPublicTests(realRun.run_id, realRun.trials[0].id);
  assert(realPublic.passed, JSON.stringify(realPublic));
  checks.push("default intelligence execution uses the existing bounded SecureExecutionQueue/compiler path");

  const suiteRoot = await mkdtemp(path.join(os.tmpdir(), "treatcode-intelligence-suite-test-"));
  tempRoots.push(suiteRoot);
  const suite = new IntelligenceSuiteService({ storageRoot: suiteRoot, executor: expectedExecutor() });
  const suiteCatalog = suite.suiteCatalog();
  assert.equal(suiteCatalog.tasks.length, 5);
  assert.equal(suiteCatalog.score_scope, "suite_task_pass_rate");
  assert.equal(suiteCatalog.external_reference?.model.id, "gpt-5.6-luna");
  assert.equal(suiteCatalog.external_reference?.model.score, 67);
  assert.equal(suiteCatalog.external_reference?.task_count, 113);
  assert.deepEqual(suiteCatalog.tasks.map((item) => item.task.id), ["TC-SWE-001", "TC-SWE-002", "TC-SWE-003", "TC-SWE-004", "TC-SWE-005"]);
  assert.deepEqual(suiteCatalog.tasks.map((item) => item.task.category), ["algorithmic-trit-repair", "parser-serialization", "memory-pointer-safety", "concurrency-state", "syscall-abi-integration"]);
  const suiteJson = JSON.stringify(suiteCatalog);
  assert(!suiteJson.includes("three-neg-mixed") && !suiteJson.includes("kernel-minus-two") && !suiteJson.includes("hidden.server"), "suite catalog leaked server-only verifier data");
  checks.push("versioned suite catalog exposes five executable categories with task-specific public metadata and no hidden cases");

  const legacyRoot = await mkdtemp(path.join(os.tmpdir(), "treatcode-intelligence-legacy-state-"));
  tempRoots.push(legacyRoot);
  await writeFile(path.join(legacyRoot, "state.v1.json"), `${JSON.stringify({
    schema: "treatcode.intelligence.state.v1",
    official_records: [{
      schema: "treatcode.intelligence.leaderboard-record.v1",
      id: "official_legacy",
      view: "official",
      task_id: "TC-SWE-001",
      run_id: "legacy-run",
      participant_id: "legacy-participant",
      score: 100,
      passed_trials: 4,
      trial_count: 4,
      tested_commit: "legacy-v1",
      published_at: new Date(0).toISOString(),
      attested: false,
      attestation_id: null,
    }],
    self_reported_records: [],
    attestations: [],
  }, null, 2)}\n`);
  const migratedSuite = new IntelligenceSuiteService({ storageRoot: legacyRoot, executor: expectedExecutor() });
  assert.equal(migratedSuite.taskService("TC-SWE-001").getOfficialLeaderboard().length, 0);
  const migratedLegacy = migratedSuite.taskService("TC-SWE-001").getSelfReportedLeaderboard();
  assert.equal(migratedLegacy.length, 1);
  assert.equal(migratedLegacy[0].score, 100);
  assert(migratedLegacy[0].note?.includes("provenance-bound"));
  checks.push("legacy TC-SWE-001 state migrates into the suite task namespace as a labeled self-reported score without deleting the original state");

  for (const task of suiteCatalog.tasks) {
    const taskRun = await suite.startRun({ task_id: task.task.id, participant_id: `suite-${task.task.id}` });
    assert.equal(taskRun.task_id, task.task.id);
    assert.equal(taskRun.trials.length, 4);
    await expectCode(suite.writeFile(taskRun.run_id, taskRun.trials[0].id, "src/not-allowlisted.trit", "bad"), "path_not_allowed");
    for (const trial of taskRun.trials) {
      const publicReport = await suite.runPublicTests(taskRun.run_id, trial.id);
      assert(publicReport.passed, `${task.task.id} public gate did not execute`);
      await suite.submitHidden(taskRun.run_id, trial.id);
    }
    const taskAggregate = await suite.getAggregate(taskRun.run_id);
    assert("score" in taskAggregate && taskAggregate.score === 100, `${task.task.id} did not produce a sealed score`);
    assert("task_id" in taskAggregate && taskAggregate.task_id === task.task.id);
    await suite.attest({ run_id: taskRun.run_id, principal: "tc:identity:service-suite", evaluation_kind: "harness_fixture", provider: "treatcode", model: "suite-proof", reasoning_effort: "not-applicable", harness: "known-correct-suite-fixtures", model_configuration: "bounded" });
  }
  assert.equal(suite.getOfficialLeaderboard().length, 5);
  const suiteRestart = new IntelligenceSuiteService({ storageRoot: suiteRoot, executor: expectedExecutor() });
  assert.equal(suiteRestart.getOfficialLeaderboard().length, 5);
  assert(new Set(suiteRestart.getOfficialLeaderboard().map((record) => record.task_id)).size === 5);
  checks.push("all five suite tasks create four-trial harness runs, pass public/hidden gates, and reload explicitly harness-scoped records after restart");

  const realSuiteRoot = await mkdtemp(path.join(os.tmpdir(), "treatcode-intelligence-real-suite-"));
  tempRoots.push(realSuiteRoot);
  const realSuite = new IntelligenceSuiteService({ storageRoot: realSuiteRoot });
  for (const task of suiteCatalog.tasks) {
    const taskRun = await realSuite.startRun({ task_id: task.task.id, participant_id: `real-${task.task.id}` });
    const starterPublic = await realSuite.runPublicTests(taskRun.run_id, taskRun.trials[0].id);
    assert.equal(starterPublic.passed, false, `${task.task.id} starter unexpectedly satisfies the public contract`);
    for (const [relative, content] of Object.entries(correctedIntelligenceFiles[task.task.id])) await realSuite.writeFile(taskRun.run_id, taskRun.trials[0].id, relative, content);
    const realPublic = await realSuite.runPublicTests(taskRun.run_id, taskRun.trials[0].id);
    assert(realPublic.passed, `${task.task.id} failed the bounded compiler/public execution path: ${JSON.stringify(realPublic)}`);
  }
  checks.push("all five defective multi-file starters fail, while corrected repairs execute one public trial through the bounded Trit compiler path");

  report.ok = true;
} catch (error) {
  report.errors.push(String(error instanceof Error ? error.stack || error.message : error));
} finally {
  await mkdirEvidence();
  await Promise.all(tempRoots.map((root) => rm(root, { recursive: true, force: true })));
}

async function mkdirEvidence(): Promise<void> {
  const { mkdir } = await import("node:fs/promises");
  await mkdir(evidenceRoot, { recursive: true });
  await writeFile(path.join(evidenceRoot, "intelligence-tests.json"), `${JSON.stringify(report, null, 2)}\n`, "utf8");
}

console.log(`P14 intelligence tests: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of report.errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
