import assert from "node:assert/strict";
import { createHash, generateKeyPairSync, sign } from "node:crypto";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import {
  compareIntelligenceV3Runs,
  scoreIntelligenceV3Run,
  scoreIntelligenceV3Task,
  validateIntelligenceV3Protocol,
  type IntelligenceV3Protocol,
  type IntelligenceV3TaskResult,
} from "../src/intelligenceV3";
import { auditIntelligenceV3TaskPackage } from "../src/intelligenceV3Packaging";
import { intelligenceV3CatalogService } from "../src/intelligenceV3Catalog";
import { IntelligenceV3AttemptLedger } from "../src/intelligenceV3Attempts";
import {
  calibrateIntelligenceV3Suite,
  type IntelligenceV3CalibrationInput,
  type IntelligenceV3CalibrationObservation,
} from "../src/intelligenceV3Calibration";
import {
  calibrateIntelligenceV3DiscussionRubric,
  scoreIntelligenceV3Discussion,
  type IntelligenceV3DiscussionCalibrationInput,
  type IntelligenceV3DiscussionRating,
  type IntelligenceV3DiscussionRubric,
} from "../src/intelligenceV3Discussion";
import {
  canonicalProvenancePayload,
  verifyIntelligenceV3ProvenanceBundle,
  type IntelligenceV3ProvenanceBundle,
} from "../src/intelligenceV3Provenance";
import {
  IntelligenceV3FullSuiteHarness,
  type IntelligenceV3ParticipantRelease,
  type IntelligenceV3SubjectAdapter,
  type IntelligenceV3TaskDescriptor,
} from "../src/intelligenceV3FullSuite";

const repoRoot = path.resolve(import.meta.dir, "..", "..");
const protocolPath = path.join(repoRoot, "benchmarks", "intelligence-v3", "protocol.v3.json");
const corpusPath = path.join(repoRoot, "benchmarks", "intelligence-v3", "corpus.candidates.v3.json");
const discussionRubricPath = path.join(repoRoot, "benchmarks", "intelligence-v3", "discussion-rubric.v3.json");
const protocol = JSON.parse(await readFile(protocolPath, "utf8")) as IntelligenceV3Protocol;
const discussionRubric = JSON.parse(await readFile(discussionRubricPath, "utf8")) as IntelligenceV3DiscussionRubric;
const checks: string[] = [];

assert.deepEqual(validateIntelligenceV3Protocol(protocol), []);
assert.equal(Object.values(protocol.task_mix).reduce((sum, count) => sum + count, 0), 100);
checks.push("v3 protocol requires 100 independent single-attempt tasks and executable weights sum to one");

const corpus = JSON.parse(await readFile(corpusPath, "utf8")) as {
  official: boolean;
  task_count: number;
  category_counts: Record<string, number>;
  design_feature_counts: Record<string, number>;
  tasks: Array<{ id: string; category: string; title: string; state: string; provenance: { author_id: string | null; reviewer_ids: string[] } }>;
};
assert.equal(corpus.official, false);
assert.equal(corpus.task_count, 100);
assert.deepEqual(corpus.category_counts, protocol.task_mix);
for (const [feature, minimum] of Object.entries(protocol.task_design_coverage)) assert.ok(corpus.design_feature_counts[feature] >= minimum, `candidate coverage for ${feature} is below protocol minimum`);
assert.equal(new Set(corpus.tasks.map((task) => task.id)).size, 100);
assert.equal(new Set(corpus.tasks.map((task) => task.title)).size, 100);
assert.ok(corpus.tasks.every((task) => task.state === "needs_independent_author" && task.provenance.author_id === null && task.provenance.reviewer_ids.length === 0));
checks.push("100 unique cross-category task briefs exist, while missing independent authors and reviewers remain explicit publication blockers");

const publicCatalog = intelligenceV3CatalogService.catalog();
assert.equal(publicCatalog.official, false);
assert.equal(publicCatalog.corpus.task_count, 100);
assert.deepEqual(publicCatalog.corpus.readiness, { executable_drafts: 2, authored: 0, reviewed: 0, packaged: 0, calibrated: 0, frozen: 0, discussion_rubric_calibrated: false, required: 100 });
assert.ok(publicCatalog.corpus.tasks.every((task) => !("provenance" in task) && !("packaging" in task) && !("calibration" in task)));
assert.equal(intelligenceV3CatalogService.discussionRubric.included_in_executable_score, false);
assert.equal(intelligenceV3CatalogService.discussionRubric.status, "draft_pending_human_calibration");
checks.push("public v3 catalog exposes task briefs and readiness counts without private provenance, package, calibration, or grader material");

const hash = (character: string) => character.repeat(64);
const makeResult = (index: number, passed = true): IntelligenceV3TaskResult => ({
  task_id: `TC-V3-${String(index + 1).padStart(3, "0")}`,
  category: corpus.tasks[index].category,
  attempt_id: `attempt-${index + 1}`,
  attempt_count: 1,
  attempt: {
    fresh_context: true,
    clock_started_before_task_release: true,
    cross_task_memory_disabled: true,
    budget_evidence_hash: hash("e"),
    tool_trace_hash: hash("f"),
  },
  provenance: {
    author_id: `author-${index % 10}`,
    reviewer_ids: [`reviewer-${index % 20}`, `reviewer-${(index + 1) % 20}`],
    subject_model_independent: true,
    provenance_bundle_hash: hash("7"),
  },
  isolation: {
    participant_root: `/participant/task-${index + 1}`,
    grader_root: `/privileged-graders/task-${index + 1}`,
    participant_bundle_hash: hash("a"),
    grader_bundle_hash: hash("b"),
    hidden_grader_isolated: true,
    isolation_audit_hash: hash("c"),
  },
  holdout: { frozen: true, manifest_hash: hash("d") },
  calibration: {
    accepted: true,
    cohort_pass_rates: { weak: 0.2, medium: 0.55, frontier: 0.75 },
    item_discrimination: 0.55,
    human_reviewed: true,
  },
  correctness: { passed, required_suites_passed: passed ? 4 : 3, required_suites_total: 4 },
  robustness: { passed: 8, total: 8 },
  efficiency: {
    wall_clock_ms: protocol.budgets.wall_clock_ms / 2,
    tool_calls: protocol.budgets.tool_calls / 2,
    input_tokens: protocol.budgets.input_tokens / 2,
    output_tokens: protocol.budgets.output_tokens / 2,
    repository_reads: protocol.budgets.repository_reads / 2,
    test_runs: protocol.budgets.test_runs / 2,
    patch_attempts: protocol.budgets.patch_attempts / 2,
  },
  agent_execution: { valid_actions: 50, total_actions: 50, policy_violations: 0, completed: true },
  discussion: { word_count: 250, human_score: 10, maximum_score: 20, rubric_version: "treatcode-discussion-v3", rater_count: 2, blind_review: true, length_controlled: true, calibration_evidence_hash: hash("9") },
});

const perfectTask = scoreIntelligenceV3Task(protocol, makeResult(0));
assert.equal(perfectTask.executable_score, 100);
assert.equal(perfectTask.discussion_score, 50);
checks.push("discussion is reported separately and cannot change the executable score");

const candidateResults = Array.from({ length: 100 }, (_, index) => makeResult(index));
const expectedTaskIds = candidateResults.map((result) => result.task_id);
const developmentScore = scoreIntelligenceV3Run(protocol, candidateResults, expectedTaskIds);
assert.equal(developmentScore.official, false);
assert.ok(developmentScore.official_blockers.includes("protocol is not frozen"));
assert.ok(developmentScore.official_blockers.includes("suite calibration is incomplete"));
checks.push("development and uncalibrated suites cannot publish official model scores");

const frozenProtocol: IntelligenceV3Protocol = {
  ...protocol,
  status: "frozen",
  calibration: { ...protocol.calibration, status: "complete" },
};
const officialScore = scoreIntelligenceV3Run(frozenProtocol, candidateResults, expectedTaskIds);
assert.equal(officialScore.official, true);
assert.equal(officialScore.distinct_task_count, 100);
assert.equal(officialScore.dimensions.correctness, 100);
assert.equal(officialScore.headline_score, 100);
assert.equal(officialScore.confidence_interval.low, 100);
assert.equal(officialScore.confidence_interval.high, 100);
checks.push("a frozen, calibrated, provenance-bound 100-task corpus can produce an official score and task-bootstrap interval");

const duplicateResults = [...candidateResults.slice(0, 99), { ...candidateResults[0], attempt_id: "duplicate" }];
const duplicateScore = scoreIntelligenceV3Run(frozenProtocol, duplicateResults, expectedTaskIds);
assert.equal(duplicateScore.official, false);
assert.ok(duplicateScore.official_blockers.includes("duplicate task attempts are not independent observations"));

const repeatedAttempt = candidateResults.map((result, index) => index === 0 ? { ...result, attempt_count: 4 } : result);
assert.ok(scoreIntelligenceV3Run(frozenProtocol, repeatedAttempt, expectedTaskIds).official_blockers.some((item) => item.includes("attempt count must be one")));
const exceededBudgetResults = candidateResults.map((result, index) => index === 0 ? { ...result, efficiency: { ...result.efficiency, tool_calls: protocol.budgets.tool_calls + 1 } } : result);
assert.ok(scoreIntelligenceV3Run(frozenProtocol, exceededBudgetResults, expectedTaskIds).official_blockers.some((item) => item.includes("fixed task budget was exceeded")));

const leakedGrader = candidateResults.map((result, index) => index === 0 ? {
  ...result,
  isolation: { ...result.isolation, grader_root: `${result.isolation.participant_root}/hidden` },
} : result);
assert.ok(scoreIntelligenceV3Run(frozenProtocol, leakedGrader, expectedTaskIds).official_blockers.some((item) => item.includes("hidden grader is not isolated")));
checks.push("duplicate trials, repeated attempts, incomplete suites, and grader roots inside participant workspaces are publication blockers");

const weakerResults = Array.from({ length: 100 }, (_, index) => makeResult(index, index >= 60));
const weakerScore = scoreIntelligenceV3Run(frozenProtocol, weakerResults, expectedTaskIds, 1234);
const comparison = compareIntelligenceV3Runs(officialScore, weakerScore, 10000, 0.95, 5678);
assert.equal(comparison.task_count, 100);
assert.equal(comparison.metric, "correctness_task_pass_rate");
assert.equal(comparison.sign_test.wins, 60);
assert.equal(comparison.sign_test.losses, 0);
assert.equal(comparison.statistically_resolved, true);
assert.ok(comparison.confidence_interval.low > 0);
assert.throws(() => compareIntelligenceV3Runs(officialScore, { ...weakerScore, task_scores: weakerScore.task_scores.slice(1) }), /identical task sets/);
checks.push("paired comparison requires identical tasks and reports paired bootstrap confidence plus an exact sign test");

const packageRoot = await mkdtemp(path.join(os.tmpdir(), "treatcode-v3-package-"));
try {
  const participantRoot = path.join(packageRoot, "participant", "TC-V3-001");
  const graderRoot = path.join(packageRoot, "privileged-graders", "TC-V3-001");
  await Promise.all([mkdir(path.join(participantRoot, "src"), { recursive: true }), mkdir(path.join(graderRoot, "tests"), { recursive: true })]);
  await Promise.all([
    writeFile(path.join(participantRoot, "src", "repair.trit"), "fn repair(x: t40) -> t40 { return x; }\n"),
    writeFile(path.join(graderRoot, "tests", "behavioral.json"), "{\"cases\":[1,2,3]}\n"),
  ]);
  const packageAudit = await auditIntelligenceV3TaskPackage({ task_id: "TC-V3-001", participant_root: participantRoot, grader_root: graderRoot, subject_workspace_root: path.join(packageRoot, "participant") });
  assert.equal(packageAudit.passed, true);
  assert.equal(packageAudit.participant_files.length, 1);
  assert.equal(packageAudit.grader_files.length, 1);
  assert.notEqual(packageAudit.participant_bundle_hash, packageAudit.grader_bundle_hash);
  const workspaceLeakAudit = await auditIntelligenceV3TaskPackage({ task_id: "TC-V3-001", participant_root: participantRoot, grader_root: graderRoot, subject_workspace_root: packageRoot });
  assert.equal(workspaceLeakAudit.passed, false);
  assert.ok(workspaceLeakAudit.issues.includes("grader root must be outside the complete subject workspace"));

  const leakedRoot = path.join(participantRoot, "hidden");
  await mkdir(leakedRoot, { recursive: true });
  await writeFile(path.join(leakedRoot, "answer.json"), "{}\n");
  const leakedAudit = await auditIntelligenceV3TaskPackage({ task_id: "TC-V3-001", participant_root: participantRoot, grader_root: graderRoot });
  assert.equal(leakedAudit.passed, false);
  assert.ok(leakedAudit.issues.some((issue) => issue.includes("reserved grader/reference path")));
checks.push("package audit hashes disjoint bundles and rejects graders anywhere in the subject workspace or hidden/answer material in the participant tree");
} finally {
  await rm(packageRoot, { recursive: true, force: true });
}

const contributorIdentities = ["independent-author", "independent-reviewer-a", "independent-reviewer-b"];
const contributorKeys = Object.fromEntries(contributorIdentities.map((identity) => [identity, generateKeyPairSync("ed25519")]));
const trustedContributorKeys = Object.fromEntries(contributorIdentities.map((identity) => [identity, contributorKeys[identity].publicKey.export({ type: "spki", format: "pem" }).toString()]));
const participantHash = "a".repeat(64);
const graderHash = "b".repeat(64);
const authorPayload = {
  role: "author" as const,
  task_id: "TC-V3-001",
  identity_id: "independent-author",
  participant_bundle_hash: participantHash,
  grader_bundle_hash: graderHash,
  independently_created: true as const,
  no_subject_rollout_access: true as const,
  subject_model_families_excluded: ["gpt-5.6-luna", "gpt-5.6-sol"],
  conflict_disclosure: "No financial, employment, or evaluation conflict.",
  completed_at: "2026-08-26T12:00:00Z",
};
const reviewPayload = (identity: string) => ({
  role: "reviewer" as const,
  task_id: "TC-V3-001",
  identity_id: identity,
  participant_bundle_hash: participantHash,
  grader_bundle_hash: graderHash,
  independent_of_author: true as const,
  independent_of_subject_models: true as const,
  conflict_disclosure: "No financial, employment, or evaluation conflict.",
  checklist: {
    symptom_is_appropriately_underspecified: true as const,
    reference_behavior_is_correct: true as const,
    hidden_grader_is_behavioral_and_adversarial: true as const,
    participant_bundle_has_no_grader_or_answer_leakage: true as const,
    budgets_are_feasible: true as const,
    task_requires_repository_level_reasoning: true as const,
  },
  decision: "approve" as const,
  reviewed_at: "2026-08-27T12:00:00Z",
});
const signedAttestation = <T extends typeof authorPayload | ReturnType<typeof reviewPayload>>(identity: string, payload: T) => ({
  identity_id: identity,
  algorithm: "ed25519" as const,
  payload,
  signature_base64: sign(null, Buffer.from(canonicalProvenancePayload(payload)), contributorKeys[identity].privateKey).toString("base64"),
});
const provenanceBundle: IntelligenceV3ProvenanceBundle = {
  schema: "treatcode.intelligence.provenance-bundle.v3",
  version: 3,
  task_id: "TC-V3-001",
  participant_bundle_hash: participantHash,
  grader_bundle_hash: graderHash,
  author: signedAttestation("independent-author", authorPayload),
  reviews: [
    signedAttestation("independent-reviewer-a", reviewPayload("independent-reviewer-a")),
    signedAttestation("independent-reviewer-b", reviewPayload("independent-reviewer-b")),
  ],
};
const provenanceVerification = verifyIntelligenceV3ProvenanceBundle(provenanceBundle, trustedContributorKeys, ["gpt-5.6-luna", "gpt-5.6-sol"]);
assert.equal(provenanceVerification.passed, true);
assert.match(provenanceVerification.provenance_bundle_hash, /^[a-f0-9]{64}$/);
const tamperedProvenance: IntelligenceV3ProvenanceBundle = {
  ...provenanceBundle,
  reviews: [{ ...provenanceBundle.reviews[0], payload: { ...provenanceBundle.reviews[0].payload, decision: "reject" } }, provenanceBundle.reviews[1]],
};
const tamperedVerification = verifyIntelligenceV3ProvenanceBundle(tamperedProvenance, trustedContributorKeys, ["gpt-5.6-luna", "gpt-5.6-sol"]);
assert.equal(tamperedVerification.passed, false);
assert.ok(tamperedVerification.issues.some((issue) => issue.includes("signature verification failed")));
assert.ok(tamperedVerification.issues.some((issue) => issue.includes("did not approve")));
const incompleteIndependence = verifyIntelligenceV3ProvenanceBundle(provenanceBundle, trustedContributorKeys, ["gpt-5.6-luna", "gpt-5.6-sol", "future-subject-family"]);
assert.equal(incompleteIndependence.passed, false);
assert.ok(incompleteIndependence.issues.includes("author did not attest independence from subject model family future-subject-family"));
checks.push("official authorship and two-reviewer approval are cryptographically bound to exact participant/grader hashes, trusted distinct identities, conflict disclosures, leakage checks, and every subject model family");

let fakeNow = 1_000_000;
const ledger = new IntelligenceV3AttemptLedger(protocol, () => fakeNow);
const ledgerRun = ledger.startRun({ provider: "test", model: "bounded-fixture", reasoning_effort: "none", expected_task_ids: expectedTaskIds });
assert.equal(ledgerRun.attempts.length, 0);
assert.equal(ledgerRun.fresh_context_per_task, true);
assert.equal(ledgerRun.cross_task_memory_disabled, true);
const firstAttempt = ledger.releaseTask(ledgerRun.run_id, expectedTaskIds[0]);
assert.equal(firstAttempt.released_at_ms, fakeNow);
assert.throws(() => ledger.releaseTask(ledgerRun.run_id, expectedTaskIds[0]), /Exactly one attempt/);
fakeNow += 250;
ledger.recordUsage(ledgerRun.run_id, expectedTaskIds[0], { tool_calls: 3, input_tokens: 1000, output_tokens: 100, repository_reads: 8, test_runs: 1, patch_attempts: 1 });
fakeNow += 750;
const sealedFirst = ledger.submitTask(ledgerRun.run_id, expectedTaskIds[0], { correctness_passed: true, robustness_passed: 2, robustness_total: 2, policy_violations: 0 });
assert.equal(sealedFirst.wall_clock_ms, 1000);
assert.throws(() => ledger.submitTask(ledgerRun.run_id, expectedTaskIds[0], { correctness_passed: true, robustness_passed: 1, robustness_total: 1, policy_violations: 0 }), /already sealed/);
assert.throws(() => ledger.finalizeRun(ledgerRun.run_id), /99 tasks are incomplete/);
for (const taskId of expectedTaskIds.slice(1)) {
  ledger.releaseTask(ledgerRun.run_id, taskId);
  fakeNow += 1;
  ledger.submitTask(ledgerRun.run_id, taskId, { correctness_passed: true, robustness_passed: 1, robustness_total: 1, policy_violations: 0 });
}
const finalizedLedger = ledger.finalizeRun(ledgerRun.run_id);
assert.equal(finalizedLedger.complete, true);
assert.equal(finalizedLedger.attempts.length, 100);
assert.match(finalizedLedger.event_trace_hash, /^[a-f0-9]{64}$/);
checks.push("hash-chained attempt ledger starts clocks before release, enforces one sealed attempt per task, records fixed-budget usage, disables cross-task memory, and rejects partial-suite finalization");

let suiteNow = 2_000_000;
const releasedAfterClock = new Set<string>();
const fullSuiteTasks: IntelligenceV3TaskDescriptor[] = expectedTaskIds.map((taskId, index) => ({
  task_id: taskId,
  releaseParticipantTask: async (): Promise<IntelligenceV3ParticipantRelease> => {
    releasedAfterClock.add(taskId);
    suiteNow += 1;
    return { task_id: taskId, participant_root: `/subject/${taskId}`, participant_bundle_hash: (index + 1).toString(16).padStart(64, "0"), initial_signal: `Diagnose the underspecified failure in ${taskId}`, public_commands: ["trit-test smoke"] };
  },
}));
const createdContexts: string[] = [];
const destroyedContexts: string[] = [];
const executedTasks: string[] = [];
const fakeSubject: IntelligenceV3SubjectAdapter = {
  createFreshContext: async ({ task_id }) => {
    assert.ok(releasedAfterClock.has(task_id));
    const contextId = `fresh-${task_id}`;
    createdContexts.push(contextId);
    return { context_id: contextId, fresh_context: true, cross_task_memory_disabled: true, context_evidence_hash: "c".repeat(64) };
  },
  executeTask: async (context, release, controls) => {
    assert.equal(context.context_id, `fresh-${release.task_id}`);
    assert.equal(controls.budgets, protocol.budgets);
    assert.ok(controls.deadline_ms > suiteNow);
    assert.equal(controls.signal.aborted, false);
    executedTasks.push(release.task_id);
    suiteNow += 2;
    return {
      submission_artifact_hash: createHash("sha256").update(`submission:${release.task_id}`).digest("hex"),
      tool_trace_hash: createHash("sha256").update(`trace:${release.task_id}`).digest("hex"),
      budget_evidence_hash: createHash("sha256").update(`budget:${release.task_id}`).digest("hex"),
      usage: { tool_calls: 2, input_tokens: 100, output_tokens: 20, repository_reads: 4, test_runs: 1, patch_attempts: 1 },
      valid_actions: 8,
      total_actions: 8,
      policy_violations: 0,
    };
  },
  destroyContext: async (context) => { destroyedContexts.push(context.context_id); },
};
const fullSuiteHarness = new IntelligenceV3FullSuiteHarness(protocol, fakeSubject, {
  grade: async (taskId) => ({ correctness_passed: taskId !== expectedTaskIds[99], robustness_passed: taskId === expectedTaskIds[99] ? 2 : 3, robustness_total: 3, grader_evidence_hash: createHash("sha256").update(`grade:${taskId}`).digest("hex") }),
}, () => suiteNow);
const fullSuiteRun = await fullSuiteHarness.run({ provider: "contract-test", model: "fresh-context-fixture", reasoning_effort: "none", tasks: fullSuiteTasks });
assert.equal(fullSuiteRun.complete, true);
assert.equal(fullSuiteRun.task_count, 100);
assert.equal(fullSuiteRun.failed_task_count, 1);
assert.equal(createdContexts.length, 100);
assert.equal(new Set(createdContexts).size, 100);
assert.deepEqual(destroyedContexts, createdContexts);
assert.equal(executedTasks.length, 100);
assert.match(fullSuiteRun.full_suite_evidence_hash, /^[a-f0-9]{64}$/);
assert.ok(fullSuiteRun.tasks.every((task) => task.context_evidence_hash && task.participant_bundle_hash && task.submission_artifact_hash && task.grader_evidence_hash && task.error === null));

const leakingTasks = fullSuiteTasks.map((task, index) => index === 0 ? {
  ...task,
  releaseParticipantTask: async () => ({ ...(await task.releaseParticipantTask()), grader_root: "/privileged/grader" }) as IntelligenceV3ParticipantRelease,
} : task);
const leakHarness = new IntelligenceV3FullSuiteHarness(protocol, fakeSubject, { grade: async () => { throw new Error("leaked task must not reach grader"); } }, () => suiteNow);
const leakRun = await leakHarness.run({ provider: "contract-test", model: "leak-fixture", reasoning_effort: "none", tasks: leakingTasks });
assert.match(leakRun.tasks[0].error || "", /leaks privileged evaluator material/);
assert.equal(leakRun.tasks[0].outcome.correctness_passed, false);
checks.push("full-suite harness starts each clock before release, creates and destroys 100 unique memory-isolated contexts, exposes participant-only payloads, performs one privileged grade per submission, seals failures without retries, and hashes complete evidence");

const calibrationObservations: IntelligenceV3CalibrationObservation[] = [];
let calibrationArtifact = 1;
for (const [cohortIndex, cohort] of protocol.calibration.required_cohorts.entries()) {
  for (let modelIndex = 0; modelIndex < protocol.calibration.minimum_models_per_cohort; modelIndex += 1) {
    for (let runIndex = 1; runIndex <= protocol.calibration.minimum_runs_per_configuration; runIndex += 1) {
      const ability = cohortIndex * 6 + modelIndex * 3 + runIndex - 1;
      const modelId = `${cohort}-model-${modelIndex + 1}`;
      const configurationId = `${modelId}-fixed-config`;
      const runId = `${configurationId}-run-${runIndex}`;
      for (const [taskIndex, taskId] of expectedTaskIds.entries()) {
        const threshold = 3 + (taskIndex % 12);
        calibrationObservations.push({
          task_id: taskId,
          cohort,
          model_id: modelId,
          configuration_id: configurationId,
          run_index: runIndex,
          run_id: runId,
          attempt_count: 1,
          fresh_context: true,
          passed: ability >= threshold,
          task_release_hash: (taskIndex + 1).toString(16).padStart(64, "0"),
          artifact_hash: (calibrationArtifact++).toString(16).padStart(64, "0"),
        });
      }
    }
  }
}
const calibrationInput: IntelligenceV3CalibrationInput = {
  schema: "treatcode.intelligence.calibration-observations.v3",
  version: 3,
  candidate_manifest_hash: "9".repeat(64),
  task_ids: expectedTaskIds,
  observations: calibrationObservations,
};
const calibrationReport = calibrateIntelligenceV3Suite(protocol, calibrationInput);
assert.equal(calibrationReport.status, "ready_for_human_review");
assert.equal(calibrationReport.official, false);
assert.equal(calibrationReport.observation_count, 1800);
assert.equal(calibrationReport.run_unit_count, 18);
assert.equal(calibrationReport.accepted_tasks, 100);
assert.equal(calibrationReport.revise_tasks, 0);
assert.equal(calibrationReport.insufficient_tasks, 0);
assert.ok(calibrationReport.tasks.every((task) => task.item_discrimination >= protocol.statistics.minimum_item_discrimination));

const ceilingObservations = calibrationObservations.map((observation) => observation.task_id === expectedTaskIds[0] ? { ...observation, passed: true } : observation);
const ceilingReport = calibrateIntelligenceV3Suite(protocol, { ...calibrationInput, observations: ceilingObservations });
assert.equal(ceilingReport.status, "needs_revision");
assert.equal(ceilingReport.tasks[0].ceiling_effect, true);
assert.equal(ceilingReport.tasks[0].disposition, "revise");

const incompleteReport = calibrateIntelligenceV3Suite(protocol, { ...calibrationInput, observations: calibrationObservations.slice(1) });
assert.equal(incompleteReport.status, "insufficient_data");
assert.ok(incompleteReport.data_issues.some((issue) => issue.includes("did not attempt the complete identical task set")));
assert.equal(incompleteReport.tasks[0].disposition, "insufficient_data");

const replayedEvidence = calibrationObservations.map((observation, index) => index === 1 ? { ...observation, artifact_hash: calibrationObservations[0].artifact_hash } : observation);
const replayedReport = calibrateIntelligenceV3Suite(protocol, { ...calibrationInput, observations: replayedEvidence });
assert.equal(replayedReport.status, "insufficient_data");
assert.ok(replayedReport.data_issues.includes("artifact evidence is reused by more than one calibration attempt"));
checks.push("calibration requires complete fresh full-suite matrices across weak/medium/frontier cohorts, computes leave-one-task-out discrimination, and sends ceiling, floor, cohort-order, or replayed-evidence items back for revision");

const discussionDimensions = discussionRubric.rating.dimensions.map((dimension) => dimension.id);
const discussionExamples = Array.from({ length: 30 }, (_, index) => {
  const anchorScore = index < 10 ? 1 : index < 20 ? 2 : 4;
  const rating = (raterId: string): IntelligenceV3DiscussionRating => ({
    rater_id: raterId,
    human_attested: true,
    model_identity_blinded: true,
    executable_score_blinded: true,
    scores: Object.fromEntries(discussionDimensions.map((dimension) => [dimension, anchorScore])),
  });
  return { artifact_hash: (5000 + index).toString(16).padStart(64, "0"), word_count: 200, ratings: [rating("human-a"), rating("human-b")] as [IntelligenceV3DiscussionRating, IntelligenceV3DiscussionRating] };
});
const discussionCalibrationInput: IntelligenceV3DiscussionCalibrationInput = {
  schema: "treatcode.intelligence.discussion-calibration.v3",
  version: 3,
  rubric_id: discussionRubric.rubric_id,
  examples: discussionExamples,
};
const discussionCalibration = calibrateIntelligenceV3DiscussionRubric(discussionRubric, discussionCalibrationInput);
assert.equal(discussionCalibration.status, "ready_for_human_approval");
assert.equal(discussionCalibration.official, false);
assert.equal(discussionCalibration.mean_agreement, 1);
assert.deepEqual(discussionCalibration.score_bands, { low: 10, middle: 10, high: 10 });

const discordantDiscussionInput: IntelligenceV3DiscussionCalibrationInput = {
  ...discussionCalibrationInput,
  examples: discussionExamples.map((example) => ({
    ...example,
    ratings: [example.ratings[0], { ...example.ratings[1], scores: Object.fromEntries(discussionDimensions.map((dimension) => [dimension, 4 - example.ratings[0].scores[dimension]])) }] as [IntelligenceV3DiscussionRating, IntelligenceV3DiscussionRating],
  })),
};
const discordantDiscussion = calibrateIntelligenceV3DiscussionRubric(discussionRubric, discordantDiscussionInput);
assert.equal(discordantDiscussion.status, "needs_calibration");
assert.ok(discordantDiscussion.issues.some((issue) => issue.includes("agreement")));

const calibratedDiscussionRubric: IntelligenceV3DiscussionRubric = { ...discussionRubric, status: "calibrated" };
const scoredDiscussion = scoreIntelligenceV3Discussion(calibratedDiscussionRubric, Array(150).fill("word").join(" "), discussionExamples[20].ratings, "8".repeat(64));
assert.equal(scoredDiscussion.valid, true);
assert.equal(scoredDiscussion.included_in_executable_score, false);
assert.equal(scoredDiscussion.percent, 100);
assert.equal(scoreIntelligenceV3Discussion(discussionRubric, "too short", discussionExamples[20].ratings, "8".repeat(64)).valid, false);

const missingDiscussionResults = candidateResults.map((result, index) => index === 0 ? { ...result, discussion: undefined } : result);
assert.ok(scoreIntelligenceV3Run(frozenProtocol, missingDiscussionResults, expectedTaskIds).official_blockers.some((item) => item.includes("secondary discussion rating is missing")));
checks.push("discussion remains zero-weight and requires 120-400 words, blinded dual human ratings, calibrated rubric evidence, score-band anchors, agreement thresholds, and adjudication of large disagreements");

const evidencePath = path.join(repoRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v3-foundation.json");
const syntheticCalibrationInputPath = path.join(repoRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v3-calibration-input.synthetic.json");
await mkdir(path.dirname(evidencePath), { recursive: true });
await writeFile(syntheticCalibrationInputPath, `${JSON.stringify(calibrationInput, null, 2)}\n`, "utf8");
await writeFile(evidencePath, `${JSON.stringify({
  schema: "treatcode.intelligence.v3-foundation-test.v1",
  status: "passed",
  checks,
  development_blockers: developmentScore.official_blockers,
  official_fixture_score: officialScore.dimensions,
  paired_comparison: comparison,
  synthetic_calibration_contract: {
    status: calibrationReport.status,
    task_count: calibrationReport.task_count,
    observation_count: calibrationReport.observation_count,
    run_unit_count: calibrationReport.run_unit_count,
    accepted_tasks: calibrationReport.accepted_tasks,
    ceiling_fraction: calibrationReport.ceiling_fraction,
    floor_fraction: calibrationReport.floor_fraction,
  },
  synthetic_calibration_input: syntheticCalibrationInputPath,
  synthetic_discussion_calibration_contract: discussionCalibration,
}, null, 2)}\n`, "utf8");

console.log(JSON.stringify({ status: "passed", checks, evidence: evidencePath }, null, 2));
