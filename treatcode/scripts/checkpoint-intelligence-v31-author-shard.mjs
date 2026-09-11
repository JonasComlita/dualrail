import path from "node:path";
import { fileURLToPath } from "node:url";
import { checkpointAuthorShard } from "./intelligence-v31-author-shards.mjs";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const shardArgument = process.argv.slice(2).find((argument) => argument.startsWith("--shard="));
const shardId = shardArgument?.slice("--shard=".length);
if (!shardId) throw new Error("usage: node scripts/checkpoint-intelligence-v31-author-shard.mjs --shard=V31-SHARD-001");
const result = checkpointAuthorShard({ repositoryRoot, shardId });
console.log(JSON.stringify({
  status: result.status,
  shard_id: result.shard_id,
  task_ids: result.receipt.task_ids,
  contributor_id: result.receipt.contributor_id,
  model: result.receipt.model,
  contribution_sha256: result.receipt.contribution_sha256,
  canonical_tasks_sha256: result.receipt.canonical_tasks_sha256,
  shard_plan_sha256: result.receipt.shard_plan_sha256,
}, null, 2));
