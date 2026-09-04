import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const localRoot = path.resolve(process.env.LOCALAPPDATA || "");
const authorRoot = path.join(localRoot, "TreatCode", "intelligence-v31-private", "final-authoring", "contributions");
const baseRoot = path.join(localRoot, "TreatCode", "intelligence-v31-private", "final-base");
const sourceRoot = path.join(baseRoot, "source");
const manifestPath = path.join(baseRoot, "base-source.v3.1.json");
const evidencePath = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v31-final-base.json");
const sha = (value) => crypto.createHash("sha256").update(value).digest("hex");
const safe = (value) => value && !path.isAbsolute(value) && !value.includes("\\") && !value.split("/").some((part) => !part || part === "." || part === "..");
const excluded = (value) => value.startsWith("benchmarks/intelligence-v3") || value.startsWith("treatcode/benchmarks/intelligence-v3") || value.startsWith(".git/");

if (fs.existsSync(baseRoot)) throw new Error("final base snapshot already exists; preparation is one-shot");
if (!fs.existsSync(authorRoot)) throw new Error("private author contributions must be ingested before preparing the final base");
const git = spawnSync("git", ["-C", repositoryRoot, "ls-files", "-z"], { encoding: "buffer", shell: false, windowsHide: true, maxBuffer: 16 * 1_048_576 });
if (git.status !== 0) throw new Error(`git ls-files failed: ${git.stderr?.toString("utf8") || git.status}`);
const tracked = git.stdout.toString("utf8").split("\0").filter(Boolean).map((value) => value.replaceAll("\\", "/")).filter((value) => safe(value) && !excluded(value));
const authored = fs.readdirSync(authorRoot).filter((file) => file.endsWith(".json")).flatMap((file) => {
  const contribution = JSON.parse(fs.readFileSync(path.join(authorRoot, file), "utf8"));
  return contribution.tasks.flatMap((task) => [...task.participant_files, ...task.distractor_files, ...task.mutations.map((mutation) => mutation.path)]);
});
const files = [...new Set([...tracked, ...authored])].sort();
fs.mkdirSync(sourceRoot, { recursive: true });
const records = [];
for (const relative of files) {
  if (!safe(relative) || excluded(relative)) continue;
  const source = path.join(repositoryRoot, relative);
  if (!fs.existsSync(source) || !fs.statSync(source).isFile()) throw new Error(`base source file is missing: ${relative}`);
  const bytes = fs.readFileSync(source);
  const target = path.join(sourceRoot, relative);
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, bytes, { flag: "wx" });
  records.push({ path: relative, bytes: bytes.length, sha256: sha(bytes) });
}
const bundleSha = sha(records.map((file) => `${file.path}\0${file.bytes}\0${file.sha256}\n`).join(""));
const manifest = { schema: "treatcode.intelligence.final-base-source.v3.1", version: "3.1", official: false, created_at: new Date().toISOString(), file_count: records.length, total_bytes: records.reduce((sum, file) => sum + file.bytes, 0), bundle_sha256: bundleSha, files: records };
fs.writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`, { flag: "wx", mode: 0o444 });
try { fs.chmodSync(manifestPath, 0o444); } catch {}
fs.mkdirSync(path.dirname(evidencePath), { recursive: true });
fs.writeFileSync(evidencePath, `${JSON.stringify({ schema: "treatcode.intelligence.final-base-evidence.v3.1", version: "3.1", official: false, created_at: manifest.created_at, file_count: manifest.file_count, total_bytes: manifest.total_bytes, bundle_sha256: bundleSha, source_location: "private-local-final-base" }, null, 2)}\n`);
console.log(JSON.stringify({ status: "prepared", file_count: manifest.file_count, total_mb: Number((manifest.total_bytes / 1_048_576).toFixed(2)), bundle_sha256: bundleSha }, null, 2));
