import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { canonicalHash, sha256, validateAuthorShardSet } from "./intelligence-v31-author-shards.mjs";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const corpusRoot = path.join(repositoryRoot, "benchmarks", "intelligence-v3.1");
const stagedRoot = path.join(corpusRoot, "authoring", "contributions");
const privateBase = process.env.TREATCODE_V31_FINAL_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v31-private");
const privateRoot = path.join(privateBase, "final-authoring", "contributions");
const evidenceRoot = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14");
const validationPath = path.join(evidenceRoot, "intelligence-v31-authoring-validation.json");
const ingestPath = path.join(evidenceRoot, "intelligence-v31-authoring-ingest.json");
const contractPath = path.join(corpusRoot, "final-corpus-contract.v3.1.json");

function within(parent, child) {
  const relative = path.relative(parent, child);
  return relative && !relative.startsWith("..") && !path.isAbsolute(relative);
}

function durableWriteNew(filePath, content, mode = 0o444) {
  const descriptor = fs.openSync(filePath, "wx", mode);
  try {
    fs.writeFileSync(descriptor, content);
    fs.fsyncSync(descriptor);
  } finally {
    fs.closeSync(descriptor);
  }
  try { fs.chmodSync(filePath, mode); } catch {}
}

function copyResumable(sourcePath, targetPath, expectedHash) {
  if (fs.existsSync(targetPath)) {
    if (!fs.statSync(targetPath).isFile() || sha256(fs.readFileSync(targetPath)) !== expectedHash) throw new Error(`${path.basename(targetPath)}: resumable ingest target binds different bytes`);
    return "already_copied";
  }
  const temporary = `${targetPath}.tmp-${process.pid}-${crypto.randomUUID()}`;
  try {
    durableWriteNew(temporary, fs.readFileSync(sourcePath));
    fs.renameSync(temporary, targetPath);
  } catch (error) {
    if (fs.existsSync(temporary)) fs.rmSync(temporary, { force: true });
    if (fs.existsSync(targetPath) && sha256(fs.readFileSync(targetPath)) === expectedHash) return "already_copied";
    throw error;
  }
  return "copied";
}

if (!within(repositoryRoot, stagedRoot) || path.basename(stagedRoot) !== "contributions") throw new Error("unsafe public contribution root");
if (!within(privateBase, privateRoot) || path.basename(privateRoot) !== "contributions") throw new Error("unsafe private contribution root");
const validation = validateAuthorShardSet({ repositoryRoot, requireCheckpoints: true });
if (validation.errors.length || validation.taskCount !== 100 || validation.records.length !== 20 || validation.records.some((record) => record.status !== "checkpointed")) throw new Error(`twenty valid immutable author checkpoints are required before private ingest:\n${validation.errors.slice(0, 20).join("\n")}`);

const validationReport = {
  schema: "treatcode.intelligence.authoring-validation.v3.1",
  version: "3.1",
  official: false,
  status: "authored_shards_checkpointed_pending_review_and_qualification",
  shard_plan_sha256: validation.loaded.planSha256,
  shard_count: validation.records.length,
  checkpointed_shards: validation.records.length,
  task_count: validation.taskCount,
  category_counts: validation.categoryCounts,
  design_feature_counts: validation.featureCounts,
  shards: validation.records.map((record) => ({ shard_id: record.shard_id, contributor_id: record.contributor_id, model: record.model, task_count: record.task_count, contribution_sha256: record.contribution_sha256, canonical_tasks_sha256: record.canonical_tasks_sha256 })),
  errors: [],
};
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(validationPath, `${JSON.stringify(validationReport, null, 2)}\n`);

fs.mkdirSync(privateRoot, { recursive: true });
const copied = [];
for (const record of validation.records) {
  const targetPath = path.join(privateRoot, `${record.shard_id}.json`);
  const copyStatus = copyResumable(record.source_path, targetPath, record.contribution_sha256);
  copied.push({
    shard_id: record.shard_id,
    file: `${record.shard_id}.json`,
    contributor_id: record.contributor_id,
    model: record.model,
    task_count: record.task_count,
    contribution_sha256: record.contribution_sha256,
    canonical_tasks_sha256: record.canonical_tasks_sha256,
    copy_status: copyStatus,
  });
}
const expectedFiles = validation.loaded.assignments.map((assignment) => assignment.file).sort();
const actualFiles = fs.readdirSync(privateRoot).filter((file) => file.endsWith(".json")).sort();
if (JSON.stringify(actualFiles) !== JSON.stringify(expectedFiles)) throw new Error("private author ingest contains unexpected or missing shard files");

for (const assignment of validation.loaded.assignments) {
  const stagedPath = path.join(stagedRoot, assignment.file);
  if (!fs.existsSync(stagedPath)) continue;
  const expected = copied.find((record) => record.shard_id === assignment.shard_id);
  if (sha256(fs.readFileSync(stagedPath)) !== expected.contribution_sha256) throw new Error(`${assignment.shard_id}: refusing to remove staged bytes that differ from the checkpoint`);
  fs.rmSync(stagedPath, { force: false });
}
const acceptedPublicMutationSpecsRemoved = validation.loaded.assignments.every((assignment) => !fs.existsSync(path.join(stagedRoot, assignment.file)));

const ingest = {
  schema: "treatcode.intelligence.authoring-ingest.v3.1",
  version: "3.1",
  official: false,
  ingested_at: new Date().toISOString(),
  task_count: 100,
  shard_count: 20,
  tasks_per_shard: 5,
  subject_model_families_excluded: ["gpt-5.6-luna", "gpt-5.6-sol"],
  private_root_id: "local-private/intelligence-v31-private/final-authoring/contributions",
  accepted_public_mutation_specs_removed: acceptedPublicMutationSpecsRemoved,
  shard_plan_sha256: validation.loaded.planSha256,
  validation_snapshot_sha256: canonicalHash(validationReport),
  contributions: copied.map(({ copy_status: _copyStatus, ...record }) => record),
};
if (fs.existsSync(ingestPath)) {
  const prior = JSON.parse(fs.readFileSync(ingestPath, "utf8"));
  if (prior.shard_plan_sha256 !== ingest.shard_plan_sha256 || canonicalHash(prior.contributions) !== canonicalHash(ingest.contributions)) throw new Error("one-shot ingest evidence already binds different author shards");
} else {
  durableWriteNew(ingestPath, `${JSON.stringify(ingest, null, 2)}\n`, 0o444);
}

const contract = JSON.parse(fs.readFileSync(contractPath, "utf8"));
if (![0, 100].includes(contract.readiness.candidate_tasks) || ![0, 100].includes(contract.readiness.independently_authored)) throw new Error("final contract contains a non-resumable partial authoring state");
contract.status = "100_authored_candidates_pending_package_review_and_calibration";
contract.readiness.candidate_tasks = 100;
contract.readiness.independently_authored = 100;
fs.writeFileSync(contractPath, `${JSON.stringify(contract, null, 2)}\n`);
console.log(JSON.stringify({ status: fs.existsSync(ingestPath) ? "ingested_private" : "ingest_failed", task_count: 100, shard_count: 20, newly_copied_shards: copied.filter((record) => record.copy_status === "copied").length, resumed_shards: copied.filter((record) => record.copy_status === "already_copied").length, accepted_public_mutation_specs_removed: ingest.accepted_public_mutation_specs_removed }, null, 2));
