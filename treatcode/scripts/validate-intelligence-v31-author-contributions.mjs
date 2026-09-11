import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { sha256, validateAuthorShardSet } from "./intelligence-v31-author-shards.mjs";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const selectedArgument = process.argv.slice(2).find((argument) => argument.startsWith("--shard="));
const selectedShardId = selectedArgument?.slice("--shard=".length);
const result = validateAuthorShardSet({ repositoryRoot, selectedShardId, requireCheckpoints: !selectedShardId });
const complete = !selectedShardId && result.records.length === 20 && result.records.every((record) => record.status === "checkpointed") && result.taskCount === 100 && result.errors.length === 0;
const selectedValid = Boolean(selectedShardId) && result.records.length === 1 && ["valid_uncheckpointed", "checkpointed"].includes(result.records[0].status) && result.errors.length === 0;
const stagedNames = fs.existsSync(result.stagedRoot) ? fs.readdirSync(result.stagedRoot).filter((file) => file.endsWith(".json")).sort() : [];
const expectedNames = new Set(result.loaded.assignments.map((assignment) => assignment.file));
const report = {
  schema: "treatcode.intelligence.authoring-validation.v3.1",
  version: "3.1",
  official: false,
  status: complete ? "authored_shards_checkpointed_pending_review_and_qualification" : selectedValid ? "author_shard_valid" : "author_shards_incomplete_or_invalid",
  shard_plan_sha256: result.loaded.planSha256,
  selected_shard_id: selectedShardId || null,
  shard_count: result.records.length,
  checkpointed_shards: result.records.filter((record) => record.status === "checkpointed").length,
  valid_uncheckpointed_shards: result.records.filter((record) => record.status === "valid_uncheckpointed").length,
  missing_shards: result.records.filter((record) => record.status === "missing").length,
  invalid_shards: result.records.filter((record) => ["invalid", "invalid_checkpoint", "checkpoint_mismatch"].includes(record.status)).length,
  task_count: result.taskCount,
  category_counts: result.categoryCounts || {},
  track_counts: result.trackCounts || {},
  design_feature_counts: result.featureCounts || {},
  unexpected_staged_json_files: stagedNames.filter((name) => !expectedNames.has(name)),
  shards: result.records.map((record) => ({
    shard_id: record.shard_id,
    status: record.status,
    checkpointed: record.checkpointed || false,
    task_count: record.task_count,
    contributor_id: record.contributor_id,
    model: record.model,
    contribution_sha256: record.contribution_sha256,
    canonical_tasks_sha256: record.canonical_tasks_sha256,
    errors: record.errors,
  })),
  errors: result.errors,
};
const evidenceRoot = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14");
fs.mkdirSync(evidenceRoot, { recursive: true });
const reportName = selectedShardId ? `intelligence-v31-authoring-validation-${selectedShardId}.json` : "intelligence-v31-authoring-validation.json";
const reportText = `${JSON.stringify(report, null, 2)}\n`;
fs.writeFileSync(path.join(evidenceRoot, reportName), reportText);
console.log(JSON.stringify({
  status: report.status,
  selected_shard_id: report.selected_shard_id,
  shard_plan_sha256: report.shard_plan_sha256,
  checkpointed_shards: report.checkpointed_shards,
  valid_uncheckpointed_shards: report.valid_uncheckpointed_shards,
  missing_shards: report.missing_shards,
  invalid_shards: report.invalid_shards,
  task_count: report.task_count,
  unexpected_staged_json_files: report.unexpected_staged_json_files,
  errors: report.errors.slice(0, 50),
  error_count: report.errors.length,
  validation_sha256: sha256(reportText),
}, null, 2));
process.exitCode = complete || selectedValid ? 0 : 1;
