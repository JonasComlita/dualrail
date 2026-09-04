import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { createIntelligenceV31FrozenSuite, verifyIntelligenceV31FrozenSuite } from "../src/intelligenceV31Repository";

const repositoryRoot = path.resolve(import.meta.dir, "..", "..");
const corpusRoot = path.join(repositoryRoot, "benchmarks", "intelligence-v3.1");
const appRoot = path.join(repositoryRoot, "treatcode");
const privateRoot = process.env.TREATCODE_V31_FINAL_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v31-private");
const authorRoot = path.join(privateRoot, "final-authoring", "contributions");
const graderRoot = path.join(privateRoot, "final-graders");
const reviewRoot = path.join(corpusRoot, "authoring", "reviews");
const qualificationRoot = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v31-final-qualification");
const reviewReportPath = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v31-review-validation.json");
const calibrationReportPath = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v31-final-calibration.json");
const contractPath = path.join(corpusRoot, "final-corpus-contract.v3.1.json");
const protocolPath = path.join(corpusRoot, "protocol.v3.1.json");
const suitePath = path.join(corpusRoot, "frozen-suite.v3.1.json");
const taskRecordRoot = path.join(corpusRoot, "frozen-task-records");
const sha256 = (value: string | Uint8Array) => createHash("sha256").update(value).digest("hex");

function collectFiles(root: string): Array<{ path: string; bytes: number; sha256: string }> {
  const result: Array<{ path: string; bytes: number; sha256: string }> = [];
  const visit = (directory: string) => {
    for (const entry of readdirSync(directory, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
      const absolute = path.join(directory, entry.name);
      if (entry.isDirectory()) visit(absolute);
      else if (entry.isFile()) {
        const bytes = readFileSync(absolute);
        result.push({ path: path.relative(root, absolute).replaceAll(path.sep, "/"), bytes: statSync(absolute).size, sha256: sha256(bytes) });
      }
    }
  };
  visit(root);
  return result;
}

function bundleHash(root: string): string {
  return sha256(collectFiles(root).map((file) => `${file.path}\0${file.bytes}\0${file.sha256}\n`).join(""));
}

for (const required of [authorRoot, graderRoot, reviewRoot, qualificationRoot, reviewReportPath, calibrationReportPath, contractPath, protocolPath]) if (!existsSync(required)) throw new Error(`freeze prerequisite is unavailable: ${required}`);
if (existsSync(suitePath) || existsSync(taskRecordRoot)) throw new Error("v3.1 suite freeze is one-shot and already has output");
const contract = JSON.parse(readFileSync(contractPath, "utf8"));
const missingInfrastructure = Object.entries(contract.infrastructure_readiness).filter(([, ready]) => ready !== true).map(([name]) => name);
if (missingInfrastructure.length) throw new Error(`official freeze blocked by unverified infrastructure: ${missingInfrastructure.join(", ")}`);
const reviewReport = JSON.parse(readFileSync(reviewReportPath, "utf8"));
if (reviewReport.status !== "all_candidates_twice_approved" || reviewReport.twice_approved !== 100) throw new Error("all 100 current candidates require two independent approvals before freeze");
const calibrationReport = JSON.parse(readFileSync(calibrationReportPath, "utf8"));
if (calibrationReport.status !== "ready_to_freeze" || calibrationReport.accepted_tasks !== 100 || calibrationReport.infrastructure_blocked) throw new Error("complete verified non-subject calibration is required before freeze");

const authorContributions = readdirSync(authorRoot).filter((file) => file.endsWith(".json")).sort().map((file) => {
  const text = readFileSync(path.join(authorRoot, file), "utf8");
  return { data: JSON.parse(text), hash: sha256(text) };
});
const reviewContributions = readdirSync(reviewRoot).filter((file) => file.endsWith(".json")).sort().map((file) => {
  const text = readFileSync(path.join(reviewRoot, file), "utf8");
  return { data: JSON.parse(text), hash: sha256(text) };
});
const qualificationFiles = readdirSync(qualificationRoot).filter((file) => file.endsWith(".json")).map((file) => ({ file, text: readFileSync(path.join(qualificationRoot, file), "utf8") })).map((item) => ({ ...item, data: JSON.parse(item.text), hash: sha256(item.text) }));
const taskInputs = authorContributions.flatMap((contribution) => contribution.data.tasks.map((task: any) => ({ task, contribution }))).sort((a, b) => a.task.task_id.localeCompare(b.task.task_id));
if (taskInputs.length !== 100 || new Set(taskInputs.map((item) => item.task.task_id)).size !== 100) throw new Error("freeze requires exactly 100 distinct authored task records");

const frozenTasks: Array<{ task_id: string; participant_bundle_hash: string; grader_bundle_hash: string }> = [];
const taskRecords: Array<{ task_id: string; text: string }> = [];
for (const { task, contribution } of taskInputs) {
  const taskGraderRoot = path.join(graderRoot, task.task_id);
  const grader = JSON.parse(readFileSync(path.join(taskGraderRoot, "grader.repository.v3.1.json"), "utf8"));
  const participantHash = grader.baseline_bundle_sha256;
  const graderHash = bundleHash(taskGraderRoot);
  const qualification = qualificationFiles.find((item) => item.data.task_id === task.task_id && item.data.participant_bundle_sha256 === participantHash);
  if (!qualification?.data.passed) throw new Error(`${task.task_id}: current participant bundle lacks passing qualification`);
  const calibration = calibrationReport.tasks.find((item: any) => item.task_id === task.task_id);
  if (calibration?.disposition !== "accept") throw new Error(`${task.task_id}: calibration disposition is not accept`);
  const approvedReviews = reviewContributions.flatMap((reviewContribution) => {
    if (reviewContribution.data.reviewed_contribution_sha256 !== contribution.hash) return [];
    return reviewContribution.data.reviews.filter((review: any) => review.task_id === task.task_id && review.decision === "approve").map((review: any) => ({ reviewer: reviewContribution.data.reviewer, review, contribution_hash: reviewContribution.hash }));
  });
  const distinctReviews = [...new Map(approvedReviews.map((item) => [item.reviewer.reviewer_id, item])).values()].slice(0, 2);
  if (distinctReviews.length !== 2) throw new Error(`${task.task_id}: two hash-bound independent approvals are unavailable`);
  const record = {
    schema: "treatcode.intelligence.final-task-record.v3.1",
    version: "3.1",
    task_id: task.task_id,
    category: task.category,
    design_features: task.design_features,
    participant_bundle_hash: participantHash,
    grader_bundle_hash: graderHash,
    authorship: { contributor_id: contribution.data.authorship.contributor_id, contributor_kind: contribution.data.authorship.contributor_kind, completed_at: contribution.data.authorship.completed_at, attestation_hash: sha256(contribution.data.authorship.attestation) },
    reviews: distinctReviews.map((item) => ({ reviewer_id: item.reviewer.reviewer_id, reviewer_kind: item.reviewer.reviewer_kind, decision: item.review.decision, completed_at: item.reviewer.completed_at, review_hash: sha256(JSON.stringify(item.review)) })),
    qualification: { reference_passed: qualification.data.reference_passed, starter_failed: qualification.data.starter_failed, mutants_caught: qualification.data.mutants_caught, mutants_total: qualification.data.mutants_total, evidence_hash: qualification.hash },
    calibration: { complete: true, observation_count: calibration.observation_count, item_discrimination: calibration.item_discrimination, cohort_pass_rates_ordered: calibration.cohort_order_valid, evidence_hash: calibrationReport.observation_set_sha256 },
    status: "frozen",
    official: false,
  };
  frozenTasks.push({ task_id: task.task_id, participant_bundle_hash: participantHash, grader_bundle_hash: graderHash });
  taskRecords.push({ task_id: task.task_id, text: `${JSON.stringify(record, null, 2)}\n` });
}

const frozen = createIntelligenceV31FrozenSuite({ frozen_at: new Date().toISOString(), protocol_sha256: sha256(readFileSync(protocolPath)), tasks: frozenTasks });
const issues = verifyIntelligenceV31FrozenSuite(frozen);
if (issues.length) throw new Error(`constructed frozen suite is invalid: ${issues.join("; ")}`);
mkdirSync(taskRecordRoot, { recursive: false });
for (const record of taskRecords) writeFileSync(path.join(taskRecordRoot, `${record.task_id}.json`), record.text, { flag: "wx", mode: 0o444 });
writeFileSync(suitePath, `${JSON.stringify(frozen, null, 2)}\n`, { flag: "wx", mode: 0o444 });
contract.status = "frozen_awaiting_one_shot_subject_comparison";
contract.readiness.frozen = 100;
writeFileSync(contractPath, `${JSON.stringify(contract, null, 2)}\n`);
console.log(JSON.stringify({ status: "frozen", official: false, task_count: 100, suite_sha256: frozen.suite_sha256, task_records: path.relative(repositoryRoot, taskRecordRoot).replaceAll(path.sep, "/") }, null, 2));
