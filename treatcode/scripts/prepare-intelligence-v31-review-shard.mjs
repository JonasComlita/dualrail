import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { isExcludedSubjectModel, loadAuthorShardPlan, sha256 } from "./intelligence-v31-author-shards.mjs";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const argumentsByName = Object.fromEntries(process.argv.slice(2).filter((argument) => argument.startsWith("--") && argument.includes("=")).map((argument) => {
  const separator = argument.indexOf("=");
  return [argument.slice(2, separator), argument.slice(separator + 1)];
}));
const shardId = argumentsByName.shard || "";
const reviewerId = argumentsByName.reviewer || "";
const model = argumentsByName.model || "";
const reasoningEffort = argumentsByName.reasoning || "";
if (!/^[A-Za-z0-9._-]{3,}$/.test(reviewerId) || !/^[a-z0-9]+(?:[.-][a-z0-9]+)*$/.test(model) || isExcludedSubjectModel(model) || !/^[A-Za-z0-9._-]{2,}$/.test(reasoningEffort)) throw new Error("usage requires a safe reviewer id, canonical non-subject model family, and reasoning effort");
const assignment = loadAuthorShardPlan(repositoryRoot).assignments.find((item) => item.shard_id === shardId);
if (!assignment) throw new Error(`unknown author shard: ${shardId}`);
const privateBase = process.env.TREATCODE_V31_FINAL_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v31-private");
const contributionPath = path.join(privateBase, "final-authoring", "contributions", assignment.file);
if (!fs.existsSync(contributionPath)) throw new Error(`${shardId}: the immutable author contribution must be privately ingested before review`);
const contributionText = fs.readFileSync(contributionPath, "utf8");
const contribution = JSON.parse(contributionText);
if (contribution.shard_id !== shardId || contribution.authorship?.contributor_id === reviewerId) throw new Error(`${shardId}: review identity conflicts with the author contribution`);
const review = {
  schema: "treatcode.intelligence.review-contribution.v3.1",
  version: "3.1",
  reviewer: { reviewer_id: reviewerId, reviewer_kind: "model_agent", model, reasoning_effort: reasoningEffort, completed_at: "", attestation: "" },
  reviewed_shard_id: shardId,
  reviewed_contribution_sha256: sha256(contributionText),
  reviews: assignment.task_ids.map((taskId) => ({ task_id: taskId, decision: "reject", source_anchor_valid: false, semantic_alignment: false, cross_module_interaction: false, hidden_tests_nontrivial: false, feature_claims_supported: false, findings: ["TODO: replace this placeholder with concrete independent review evidence."] })),
};
const reviewRoot = path.join(privateBase, "final-authoring", "reviews");
const fileName = `${shardId}.review-${reviewerId}.json`;
const targetPath = path.join(reviewRoot, fileName);
if (fs.existsSync(targetPath)) throw new Error(`${fileName}: review scaffold already exists; refusing to overwrite it`);
fs.mkdirSync(reviewRoot, { recursive: true });
const temporary = path.join(reviewRoot, `.${fileName}.tmp-${process.pid}-${crypto.randomUUID()}`);
try {
  fs.writeFileSync(temporary, `${JSON.stringify(review, null, 2)}\n`, { flag: "wx" });
  fs.renameSync(temporary, targetPath);
} catch (error) {
  if (fs.existsSync(temporary)) fs.rmSync(temporary, { force: true });
  throw error;
}
console.log(JSON.stringify({ status: "review_shard_scaffolded", shard_id: shardId, reviewer_id: reviewerId, task_ids: assignment.task_ids, private_review_path: targetPath, next: "Complete all five reviews, authorship-independent attestation, and completion timestamp; then run the review validator." }, null, 2));
