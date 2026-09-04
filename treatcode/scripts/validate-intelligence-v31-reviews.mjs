import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const localRoot = path.resolve(process.env.LOCALAPPDATA || "");
const stagedContributionRoot = path.join(repositoryRoot, "benchmarks", "intelligence-v3.1", "authoring", "contributions");
const privateContributionRoot = path.join(localRoot, "TreatCode", "intelligence-v31-private", "final-authoring", "contributions");
const reviewRoot = path.join(repositoryRoot, "benchmarks", "intelligence-v3.1", "authoring", "reviews");
const rejectedContributionRoot = path.join(repositoryRoot, "benchmarks", "intelligence-v3.1", "authoring", "rejected");
const evidenceRoot = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14");
const sha = (value) => crypto.createHash("sha256").update(value).digest("hex");
const errors = [];
const contributionRoot = fs.existsSync(stagedContributionRoot) ? stagedContributionRoot : privateContributionRoot;
const contributionFiles = fs.existsSync(contributionRoot) ? fs.readdirSync(contributionRoot).filter((file) => file.endsWith(".json")).sort() : [];
const contributions = contributionFiles.map((file) => {
  const text = fs.readFileSync(path.join(contributionRoot, file), "utf8");
  return { file, text, hash: sha(text), data: JSON.parse(text) };
});
const taskOwners = new Map(contributions.flatMap((contribution) => contribution.data.tasks.map((task) => [task.task_id, { author_id: contribution.data.authorship.contributor_id, contribution_hash: contribution.hash }] )));
const historicalContributions = fs.existsSync(rejectedContributionRoot) ? fs.readdirSync(rejectedContributionRoot).filter((file) => file.endsWith(".json")).map((file) => { const text = fs.readFileSync(path.join(rejectedContributionRoot, file), "utf8"); return { file, text, hash: sha(text), data: JSON.parse(text) }; }) : [];
const contributionsByHash = new Map([...contributions, ...historicalContributions].map((contribution) => [contribution.hash, contribution]));
const reviews = [];
let supersededReviewItems = 0;

if (!fs.existsSync(reviewRoot)) errors.push("review contribution directory is missing");
for (const file of fs.existsSync(reviewRoot) ? fs.readdirSync(reviewRoot).filter((name) => name.endsWith(".json")).sort() : []) {
  const filePath = path.join(reviewRoot, file);
  let data;
  try { data = JSON.parse(fs.readFileSync(filePath, "utf8")); } catch (error) { errors.push(`${file}: invalid JSON (${error.message})`); continue; }
  const reviewer = data.reviewer || {};
  if (data.schema !== "treatcode.intelligence.review-contribution.v3.1" || data.version !== "3.1" || reviewer.reviewer_kind !== "model_agent" || ["gpt-5.6-luna", "gpt-5.6-sol"].includes(reviewer.model) || !Number.isFinite(Date.parse(reviewer.completed_at || "")) || `${reviewer.attestation || ""}`.length < 40) errors.push(`${file}: reviewer metadata is invalid`);
  if (!Array.isArray(data.reviews) || data.reviews.length === 0 || new Set(data.reviews.map((item) => item.task_id)).size !== data.reviews.length) errors.push(`${file}: review list is empty or duplicates tasks`);
  const reviewedContribution = contributionsByHash.get(`${data.reviewed_contribution_sha256 || ""}`.toLowerCase());
  if (!reviewedContribution) errors.push(`${file}: reviewed contribution hash is unknown`);
  for (const review of data.reviews || []) {
    const owner = taskOwners.get(review.task_id);
    const label = `${file}/${review.task_id}`;
    const reviewedTask = reviewedContribution?.data.tasks?.find((task) => task.task_id === review.task_id);
    if (!reviewedTask) { errors.push(`${label}: task does not belong to the hash-bound reviewed contribution`); continue; }
    if (reviewer.reviewer_id === reviewedContribution.data.authorship.contributor_id) errors.push(`${label}: author reviewed their own task`);
    const checks = ["source_anchor_valid", "semantic_alignment", "cross_module_interaction", "hidden_tests_nontrivial", "feature_claims_supported"];
    if (checks.some((key) => typeof review[key] !== "boolean") || !Array.isArray(review.findings) || review.findings.length === 0 || review.findings.some((finding) => typeof finding !== "string" || finding.length < 8)) errors.push(`${label}: review checks/findings are incomplete`);
    const allChecks = checks.every((key) => review[key] === true);
    if ((review.decision === "approve") !== allChecks) errors.push(`${label}: approve requires every check; reject requires at least one failed check`);
    if (!owner || owner.contribution_hash !== reviewedContribution.hash) { supersededReviewItems += 1; continue; }
    reviews.push({ ...review, reviewer_id: reviewer.reviewer_id, reviewer_model: reviewer.model, review_file: file, review_sha256: sha(JSON.stringify(review)) });
  }
}

const taskRecords = [...taskOwners.keys()].sort().map((taskId) => {
  const taskReviews = reviews.filter((review) => review.task_id === taskId);
  const distinct = new Set(taskReviews.map((review) => review.reviewer_id));
  if (distinct.size !== taskReviews.length) errors.push(`${taskId}: duplicate reviewer identity`);
  return { task_id: taskId, review_count: taskReviews.length, distinct_reviewers: distinct.size, approvals: taskReviews.filter((review) => review.decision === "approve").length, rejections: taskReviews.filter((review) => review.decision === "reject").length, reviewer_ids: [...distinct].sort() };
});
const twiceReviewed = taskRecords.filter((task) => task.distinct_reviewers >= 2).length;
const twiceApproved = taskRecords.filter((task) => task.distinct_reviewers >= 2 && task.approvals >= 2 && task.rejections === 0).length;
const report = {
  schema: "treatcode.intelligence.review-validation.v3.1",
  version: "3.1",
  official: false,
  status: errors.length ? "invalid" : twiceReviewed === 100 ? (twiceApproved === 100 ? "all_candidates_twice_approved" : "review_complete_with_rejections") : "reviews_in_progress",
  task_count: taskRecords.length,
  review_count: reviews.length,
  superseded_review_items: supersededReviewItems,
  twice_reviewed: twiceReviewed,
  twice_approved: twiceApproved,
  rejected_after_two_reviews: taskRecords.filter((task) => task.distinct_reviewers >= 2 && task.rejections > 0).length,
  task_records: taskRecords,
  errors,
};
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "intelligence-v31-review-validation.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(JSON.stringify({ status: report.status, task_count: report.task_count, review_count: report.review_count, superseded_review_items: report.superseded_review_items, twice_reviewed: report.twice_reviewed, twice_approved: report.twice_approved, rejected_after_two_reviews: report.rejected_after_two_reviews, errors: errors.slice(0, 50), error_count: errors.length }, null, 2));
process.exitCode = errors.length ? 1 : 0;
