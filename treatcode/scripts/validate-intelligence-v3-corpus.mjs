import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";

const repoRoot = path.resolve(import.meta.dirname, "..", "..");
const protocolPath = path.join(repoRoot, "benchmarks", "intelligence-v3", "protocol.v3.json");
const corpusPath = path.join(repoRoot, "benchmarks", "intelligence-v3", "corpus.candidates.v3.json");
const draftsPath = path.join(repoRoot, "benchmarks", "intelligence-v3", "executable-drafts.v3.json");
const calibrationPlanPath = path.join(repoRoot, "benchmarks", "intelligence-v3", "calibration-plan.v3.json");
const discussionRubricPath = path.join(repoRoot, "benchmarks", "intelligence-v3", "discussion-rubric.v3.json");
const ownerConfirmationPath = path.join(repoRoot, "benchmarks", "intelligence-v3", "owner-confirmation.v3.json");
const evidencePath = path.join(repoRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v3-readiness.json");
const requireOfficial = process.argv.includes("--official");

const [protocolText, corpusText, draftsText, calibrationPlanText, discussionRubricText, ownerConfirmationText] = await Promise.all([
  readFile(protocolPath, "utf8"),
  readFile(corpusPath, "utf8"),
  readFile(draftsPath, "utf8"),
  readFile(calibrationPlanPath, "utf8"),
  readFile(discussionRubricPath, "utf8"),
  readFile(ownerConfirmationPath, "utf8"),
]);
const protocol = JSON.parse(protocolText);
const corpus = JSON.parse(corpusText);
const drafts = JSON.parse(draftsText);
const calibrationPlan = JSON.parse(calibrationPlanText);
const discussionRubric = JSON.parse(discussionRubricText);
const ownerConfirmation = JSON.parse(ownerConfirmationText);

assert.equal(protocol.schema, "treatcode.intelligence.protocol.v3");
assert.equal(corpus.schema, "treatcode.intelligence.corpus-candidates.v3");
assert.equal(drafts.schema, "treatcode.intelligence.executable-drafts.v3");
assert.equal(drafts.official_eligible, false);
assert.equal(calibrationPlan.schema, "treatcode.intelligence.calibration-plan.v3");
assert.equal(calibrationPlan.official, false);
assert.equal(discussionRubric.schema, "treatcode.intelligence.discussion-rubric.v3");
assert.equal(ownerConfirmation.schema, "treatcode.intelligence.owner-confirmation.v3");
assert.equal(ownerConfirmation.version, 3);
assert.equal(ownerConfirmation.status, "development_authorized");
assert.ok(Array.isArray(ownerConfirmation.scope) && ownerConfirmation.scope.length > 0);
assert.ok(Array.isArray(ownerConfirmation.nonclaims) && ownerConfirmation.nonclaims.length >= 6);
assert.equal(discussionRubric.included_in_executable_score, false);
assert.equal(discussionRubric.length.maximum_words, protocol.dimensions.discussion.maximum_words);
assert.ok(discussionRubric.rating.minimum_raters >= 2);
assert.equal(corpus.task_count, corpus.tasks.length);
assert.equal(new Set(corpus.tasks.map((task) => task.id)).size, corpus.tasks.length);
assert.equal(new Set(corpus.tasks.map((task) => task.title)).size, corpus.tasks.length);
assert.deepEqual(corpus.category_counts, protocol.task_mix);
for (const [feature, minimum] of Object.entries(protocol.task_design_coverage)) assert.ok(corpus.design_feature_counts[feature] >= minimum, `${feature} coverage is below ${minimum}`);
const corpusIds = new Set(corpus.tasks.map((task) => task.id));
assert.equal(new Set(drafts.tasks.map((task) => task.task_id)).size, drafts.tasks.length);
assert.ok(drafts.tasks.every((task) => corpusIds.has(task.task_id) && task.grader_outside_subject_workspace && task.one_shot_hidden_submission && !task.starter.public_passed && !task.starter.hidden_passed && task.private_known_good.public_passed && task.private_known_good.hidden_passed));
assert.equal(calibrationPlan.candidate_manifest.sha256, createHash("sha256").update(corpusText).digest("hex"));
assert.equal(calibrationPlan.candidate_manifest.task_count, corpus.task_count);
assert.deepEqual(calibrationPlan.design.cohorts, protocol.calibration.required_cohorts);
assert.equal(calibrationPlan.design.models_per_cohort, protocol.calibration.minimum_models_per_cohort);
assert.equal(calibrationPlan.design.runs_per_configuration, protocol.calibration.minimum_runs_per_configuration);
assert.equal(calibrationPlan.design.full_suite_run_units, protocol.calibration.required_cohorts.length * protocol.calibration.minimum_models_per_cohort * protocol.calibration.minimum_runs_per_configuration);
assert.equal(calibrationPlan.design.required_task_observations, calibrationPlan.design.full_suite_run_units * corpus.task_count);

const readiness = {
  distinct_tasks: new Set(corpus.tasks.map((task) => task.id)).size,
  executable_drafts: drafts.tasks.length,
  authored: corpus.tasks.filter((task) => task.provenance.author_id).length,
  reviewed: corpus.tasks.filter((task) => new Set(task.provenance.reviewer_ids).size >= protocol.official_requirements.minimum_reviewers_per_task).length,
  independence_verified: corpus.tasks.filter((task) => task.provenance.subject_model_independence_verified).length,
  packaged: corpus.tasks.filter((task) => task.packaging.participant_bundle_hash && task.packaging.grader_bundle_hash && task.packaging.isolation_audit_hash).length,
  calibrated: corpus.tasks.filter((task) => task.calibration.status === "accepted").length,
  frozen: corpus.tasks.filter((task) => task.holdout.frozen && task.holdout.manifest_hash).length,
  discussion_rubric_calibrated: discussionRubric.status === "calibrated" || discussionRubric.status === "frozen",
  owner_development_confirmation: ownerConfirmation.status === "development_authorized",
};
const authors = new Set(corpus.tasks.map((task) => task.provenance.author_id).filter(Boolean));
const required = protocol.official_requirements.minimum_distinct_tasks;
const blockers = [];
if (readiness.distinct_tasks < required) blockers.push(`requires ${required} distinct tasks`);
if (readiness.executable_drafts < required) blockers.push(`${required - readiness.executable_drafts} tasks lack executable participant/grader development packages`);
if (readiness.authored < required) blockers.push(`${required - readiness.authored} tasks lack an independent author`);
if (authors.size < protocol.official_requirements.minimum_independent_authors) blockers.push(`requires ${protocol.official_requirements.minimum_independent_authors} independent authors; found ${authors.size}`);
if (readiness.reviewed < required) blockers.push(`${required - readiness.reviewed} tasks lack two independent reviews`);
if (readiness.independence_verified < required) blockers.push(`${required - readiness.independence_verified} tasks lack subject-model independence evidence`);
if (readiness.packaged < required) blockers.push(`${required - readiness.packaged} tasks lack disjoint participant/grader package audits`);
if (readiness.calibrated < required || protocol.calibration.status !== "complete") blockers.push("weak/medium/frontier calibration is incomplete");
if (readiness.frozen < required || protocol.status !== "frozen") blockers.push("the accepted holdout and protocol are not frozen");
if (!readiness.discussion_rubric_calibrated) blockers.push("the secondary discussion rubric lacks human calibration and approval");
blockers.push("no provider-backed weak/medium/frontier or compared-model full-suite run evidence exists");
if (!corpus.official) blockers.push("candidate corpus is explicitly non-official");

const requirements = [
  {
    id: "independent_tasks",
    requirement: "At least 100 independently authored executable tasks",
    status: readiness.authored >= required && readiness.executable_drafts >= required && authors.size >= protocol.official_requirements.minimum_independent_authors ? "satisfied" : "incomplete",
    evidence: { candidate_ids: readiness.distinct_tasks, executable_drafts: readiness.executable_drafts, signed_authored_tasks: readiness.authored, independent_authors: authors.size, required_tasks: required, required_authors: protocol.official_requirements.minimum_independent_authors },
  },
  {
    id: "one_attempt",
    requirement: "One scored attempt per task",
    status: "control_implemented_no_real_run",
    evidence: { protocol_attempts_per_task: protocol.official_requirements.attempts_per_task, attempt_ledger_test: "build/treatcode-plan-evidence/P14/intelligence-v3-foundation.json", provider_runs: 0 },
  },
  {
    id: "hidden_isolation",
    requirement: "Hidden repositories and graders outside the model-accessible workspace",
    status: readiness.packaged >= required ? "satisfied" : "incomplete",
    evidence: { official_isolation_audits: readiness.packaged, executable_isolated_drafts: readiness.executable_drafts, required },
  },
  {
    id: "diagnostic_repository_tasks",
    requirement: "Underspecified repository diagnosis with multi-module and adversarial task design",
    status: readiness.executable_drafts >= required ? "satisfied" : "incomplete",
    evidence: { candidate_briefs: readiness.distinct_tasks, executable_drafts: readiness.executable_drafts, design_feature_counts: corpus.design_feature_counts },
  },
  {
    id: "fixed_budgets",
    requirement: "Fixed tool, token, and wall-clock budgets starting before release",
    status: "control_implemented_no_real_run",
    evidence: { budgets: protocol.budgets, harness_contract_test: "build/treatcode-plan-evidence/P14/intelligence-v3-foundation.json", provider_runs: 0 },
  },
  {
    id: "full_suite_every_model",
    requirement: "Complete identical suite execution for every compared model",
    status: "missing_external_runs",
    evidence: { provider_backed_full_suite_runs: 0, required_tasks_per_run: required },
  },
  {
    id: "separate_dimensions",
    requirement: "Separate correctness, robustness, efficiency, and agent-execution scores",
    status: "implemented",
    evidence: { dimensions: Object.keys(protocol.dimensions).filter((name) => name !== "discussion"), foundation_test: "build/treatcode-plan-evidence/P14/intelligence-v3-foundation.json" },
  },
  {
    id: "uncertainty_and_significance",
    requirement: "Task bootstrap confidence intervals and paired significance testing",
    status: "implemented",
    evidence: { statistics: protocol.statistics, foundation_test: "build/treatcode-plan-evidence/P14/intelligence-v3-foundation.json" },
  },
  {
    id: "pilot_calibration",
    requirement: "Weak, medium, and frontier pilot calibration",
    status: readiness.calibrated >= required && protocol.calibration.status === "complete" ? "satisfied" : "missing_external_runs",
    evidence: { calibrated_tasks: readiness.calibrated, required, required_observations: calibrationPlan.design.required_task_observations, actual_observations: 0 },
  },
  {
    id: "item_revision",
    requirement: "Revision or removal of ceiling, floor, and low-discrimination tasks",
    status: readiness.calibrated >= required ? "satisfied" : "engine_implemented_no_empirical_items",
    evidence: { thresholds: protocol.statistics, calibrated_tasks: readiness.calibrated, calibration_contract_test: "build/treatcode-plan-evidence/P14/intelligence-v3-calibration-contract.synthetic.json" },
  },
  {
    id: "discussion_secondary",
    requirement: "Human-calibrated, length-controlled discussion grading as a secondary metric only",
    status: readiness.discussion_rubric_calibrated ? "satisfied" : "rubric_implemented_human_calibration_missing",
    evidence: { rubric_status: discussionRubric.status, included_in_executable_score: discussionRubric.included_in_executable_score, length: discussionRubric.length },
  },
  {
    id: "holdout_freeze",
    requirement: "Accepted suite and protocol frozen before subject runs",
    status: readiness.frozen >= required && protocol.status === "frozen" ? "satisfied" : "incomplete",
    evidence: { frozen_tasks: readiness.frozen, required, protocol_status: protocol.status },
  },
];

const report = {
  schema: "treatcode.intelligence.v3-readiness.v1",
  status: blockers.length === 0 ? "official_ready" : "development",
  official_ready: blockers.length === 0,
  protocol: { status: protocol.status, calibration_status: protocol.calibration.status, minimum_distinct_tasks: required },
  corpus: { status: corpus.status, official: corpus.official, task_count: corpus.task_count },
  owner_confirmation: {
    status: ownerConfirmation.status,
    confirmed_at: ownerConfirmation.confirmed_at,
    scope: ownerConfirmation.scope,
    nonclaims: ownerConfirmation.nonclaims,
  },
  readiness,
  requirements,
  blockers,
};

await mkdir(path.dirname(evidencePath), { recursive: true });
await writeFile(evidencePath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
console.log(JSON.stringify({ ...report, evidence: evidencePath }, null, 2));
if (requireOfficial && blockers.length > 0) process.exitCode = 2;
