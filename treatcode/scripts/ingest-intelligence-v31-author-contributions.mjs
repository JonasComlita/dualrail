import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const publicRoot = path.resolve(repositoryRoot, "benchmarks", "intelligence-v3.1", "authoring", "contributions");
const localRoot = path.resolve(process.env.LOCALAPPDATA || "");
const privateRoot = path.resolve(localRoot, "TreatCode", "intelligence-v31-private", "final-authoring", "contributions");
const evidenceRoot = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14");
const validationPath = path.join(evidenceRoot, "intelligence-v31-authoring-validation.json");
const contractPath = path.join(repositoryRoot, "benchmarks", "intelligence-v3.1", "final-corpus-contract.v3.1.json");
const ingestPath = path.join(evidenceRoot, "intelligence-v31-authoring-ingest.json");
const expectedFiles = ["author-a-gpt54.json", "author-b-gpt54.json", "author-c-gpt54-v2.json"];
const sha = (value) => crypto.createHash("sha256").update(value).digest("hex");
const within = (parent, child) => { const relative = path.relative(parent, child); return relative && !relative.startsWith("..") && !path.isAbsolute(relative); };

if (!within(repositoryRoot, publicRoot) || path.basename(publicRoot) !== "contributions") throw new Error("unsafe public contribution root");
if (!within(localRoot, privateRoot) || path.basename(privateRoot) !== "contributions") throw new Error("unsafe private contribution root");
if (fs.existsSync(ingestPath) || fs.existsSync(privateRoot)) throw new Error("author contribution ingest is one-shot and already exists");
const validation = JSON.parse(fs.readFileSync(validationPath, "utf8"));
if (validation.status !== "authored_candidates_validated_pending_review_and_qualification" || validation.task_count !== 100 || validation.errors?.length) throw new Error("100 valid author contributions are required before private ingest");
const publicNames = fs.readdirSync(publicRoot).sort();
if (JSON.stringify(publicNames) !== JSON.stringify(expectedFiles)) throw new Error("public contribution staging contains unexpected or missing files");

fs.mkdirSync(privateRoot, { recursive: true });
const contributions = [];
for (const file of expectedFiles) {
  const source = path.join(publicRoot, file);
  const target = path.join(privateRoot, file);
  const text = fs.readFileSync(source, "utf8");
  const parsed = JSON.parse(text);
  fs.writeFileSync(target, text, { flag: "wx", mode: 0o444 });
  try { fs.chmodSync(target, 0o444); } catch {}
  contributions.push({ file, contributor_id: parsed.authorship.contributor_id, model: parsed.authorship.model, task_count: parsed.tasks.length, contribution_sha256: sha(text) });
}
const ingest = {
  schema: "treatcode.intelligence.authoring-ingest.v3.1",
  version: "3.1",
  official: false,
  ingested_at: new Date().toISOString(),
  task_count: 100,
  subject_model_families_excluded: ["gpt-5.6-luna", "gpt-5.6-sol"],
  private_root_id: "LOCALAPPDATA/TreatCode/intelligence-v31-private/final-authoring/contributions",
  public_mutation_specs_removed: true,
  contributions,
  validation_sha256: sha(fs.readFileSync(validationPath)),
};
fs.writeFileSync(ingestPath, `${JSON.stringify(ingest, null, 2)}\n`, { flag: "wx" });

const contract = JSON.parse(fs.readFileSync(contractPath, "utf8"));
if (contract.readiness.candidate_tasks !== 0 || contract.readiness.independently_authored !== 0) throw new Error("final contract authoring readiness was already mutated");
contract.status = "100_authored_candidates_pending_package_review_and_calibration";
contract.readiness.candidate_tasks = 100;
contract.readiness.independently_authored = 100;
fs.writeFileSync(contractPath, `${JSON.stringify(contract, null, 2)}\n`);

fs.rmSync(publicRoot, { recursive: true, force: false });
const authoringRoot = path.dirname(publicRoot);
if (fs.existsSync(authoringRoot) && fs.readdirSync(authoringRoot).length === 0) fs.rmdirSync(authoringRoot);
console.log(JSON.stringify({ status: "ingested_private", task_count: 100, contributions, public_mutation_specs_removed: !fs.existsSync(publicRoot) }, null, 2));
