import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { authorShardPaths, createAuthorShardScaffold } from "./intelligence-v31-author-shards.mjs";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const argumentsByName = Object.fromEntries(process.argv.slice(2).filter((argument) => argument.startsWith("--") && argument.includes("=")).map((argument) => {
  const separator = argument.indexOf("=");
  return [argument.slice(2, separator), argument.slice(separator + 1)];
}));
const shardId = argumentsByName.shard;
const scaffold = createAuthorShardScaffold({ repositoryRoot, shardId, contributorId: argumentsByName.contributor, model: argumentsByName.model, reasoningEffort: argumentsByName.reasoning });
const roots = authorShardPaths(repositoryRoot);
const targetPath = path.join(roots.stagedRoot, `${shardId}.json`);
if (fs.existsSync(targetPath)) throw new Error(`${shardId}: staged scaffold or contribution already exists; refusing to overwrite it`);
fs.mkdirSync(roots.stagedRoot, { recursive: true });
const temporary = path.join(roots.stagedRoot, `.${shardId}.tmp-${process.pid}-${crypto.randomUUID()}`);
try {
  fs.writeFileSync(temporary, `${JSON.stringify(scaffold, null, 2)}\n`, { flag: "wx" });
  fs.renameSync(temporary, targetPath);
} catch (error) {
  if (fs.existsSync(temporary)) fs.rmSync(temporary, { force: true });
  throw error;
}
console.log(JSON.stringify({ status: "author_shard_scaffolded", shard_id: shardId, task_ids: scaffold.tasks.map((task) => task.task_id), contribution_path: path.relative(repositoryRoot, targetPath).replaceAll(path.sep, "/"), next: `Complete all fields, validate this shard, then checkpoint ${shardId}.` }, null, 2));
