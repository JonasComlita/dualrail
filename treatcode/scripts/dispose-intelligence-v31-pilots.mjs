import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const argument = (name) => process.argv.find((value) => value.startsWith(`--${name}=`))?.slice(name.length + 3);
const runId = argument("run");
if (!/^[A-Za-z0-9._-]+$/.test(runId || "")) throw new Error("usage: --run=<completed-pilot-run-id>");

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const corpusRoot = path.join(repositoryRoot, "benchmarks", "intelligence-v3.1");
const participantRoot = path.resolve(corpusRoot, "pilots");
const privateRoot = path.resolve(process.env.LOCALAPPDATA || "", "TreatCode", "intelligence-v31-private", "pilot");
const comparisonPath = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v31-pilot-comparison.json");
const corpusPath = path.join(corpusRoot, "pilot-corpus.v3.1.json");
const runRoot = path.join(repositoryRoot, "build", "intelligence-v31-pilot-runs", runId);
const evidencePath = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v31-pilot-disposal-manifest.json");
const sha = (value) => crypto.createHash("sha256").update(value).digest("hex");
const relative = (target, parent) => {
  const value = path.relative(parent, target);
  if (!value || value.startsWith("..") || path.isAbsolute(value)) throw new Error(`refusing unsafe disposal target: ${target}`);
  return value;
};

relative(participantRoot, repositoryRoot);
relative(privateRoot, path.resolve(process.env.LOCALAPPDATA || ""));
if (path.basename(participantRoot) !== "pilots" || path.basename(privateRoot) !== "pilot") throw new Error("pilot disposal roots do not have exact expected names");
if (!fs.existsSync(comparisonPath)) throw new Error("completed pilot comparison evidence is missing");
if (fs.existsSync(evidencePath)) throw new Error("pilot disposal manifest already exists; disposal is one-shot");

const comparisonText = fs.readFileSync(comparisonPath, "utf8");
const comparison = JSON.parse(comparisonText);
if (comparison.run_id !== runId || comparison.status !== "complete" || comparison.terminal_attempts !== 60 || comparison.paired_tasks_complete !== 30) throw new Error("pilot run is not complete and cannot be disposed");
const corpus = JSON.parse(fs.readFileSync(corpusPath, "utf8"));
if (corpus.task_count !== 30 || corpus.tasks.length !== 30) throw new Error("pilot corpus is incomplete");

const tasks = corpus.tasks.map((task) => {
  const row = comparison.task_matrix.find((item) => item.task_id === task.task_id);
  if (!row?.luna_max || !row?.sol_high) throw new Error(`missing terminal subject evidence for ${task.task_id}`);
  const grades = ["gpt-5.6-luna-max", "gpt-5.6-sol-high"].map((directory) => {
    const gradePath = path.join(runRoot, task.task_id, directory, "sealed-grade.json");
    const text = fs.readFileSync(gradePath, "utf8");
    const grade = JSON.parse(text);
    return { configuration: directory, grade_sha256: sha(text), evidence_hash: grade.sealed_report?.evidence_hash ?? null, participant_bundle_hash: grade.sealed_report?.participant_bundle_hash ?? null, grader_bundle_hash: grade.sealed_report?.grader_bundle_hash ?? null, correctness_observed: grade.correctness_observed !== false };
  });
  const observed = grades.find((grade) => grade.correctness_observed && grade.participant_bundle_hash && grade.grader_bundle_hash);
  if (!observed) throw new Error(`no package hashes survived for ${task.task_id}`);
  return {
    task_id: task.task_id,
    category: task.category,
    disposition: "discarded_after_ceiling_analysis",
    participant_bundle_hash: observed.participant_bundle_hash,
    grader_bundle_hash: observed.grader_bundle_hash,
    subject_grades: grades,
  };
});

const disposal = {
  schema: "treatcode.intelligence.pilot-disposal.v3.1",
  version: "3.1",
  official: false,
  disposable: true,
  run_id: runId,
  disposed_at: new Date().toISOString(),
  comparison_sha256: sha(comparisonText),
  reason: "The task family produced 28/28 observed pass/pass pairs and no diagnostic discordance; exposed pilot packages are permanently ineligible for a holdout.",
  removed_roots: [path.relative(repositoryRoot, participantRoot).replaceAll(path.sep, "/"), "LOCALAPPDATA/TreatCode/intelligence-v31-private/pilot"],
  retained_evidence: [path.relative(repositoryRoot, comparisonPath).replaceAll(path.sep, "/"), path.relative(repositoryRoot, runRoot).replaceAll(path.sep, "/")],
  derived_design_patterns: comparison.derived_design_patterns,
  tasks,
};
fs.writeFileSync(evidencePath, `${JSON.stringify(disposal, null, 2)}\n`, { flag: "wx" });

corpus.status = "pilot_analysis_complete_packages_disposed";
corpus.warning = "All pilot packages were disposed after complete analysis. The tasks are exposed development evidence and permanently ineligible for any holdout.";
for (const task of corpus.tasks) {
  const record = tasks.find((item) => item.task_id === task.task_id);
  task.package_status = "disposed";
  task.disposition = "discarded_after_ceiling_analysis";
  task.disposed_participant_bundle_hash = record.participant_bundle_hash;
  task.disposed_grader_bundle_hash = record.grader_bundle_hash;
  delete task.participant_package;
}
fs.writeFileSync(corpusPath, `${JSON.stringify(corpus, null, 2)}\n`);

if (fs.existsSync(participantRoot)) fs.rmSync(participantRoot, { recursive: true, force: false });
if (fs.existsSync(privateRoot)) fs.rmSync(privateRoot, { recursive: true, force: false });
console.log(JSON.stringify({ status: "disposed", task_count: tasks.length, evidence_path: evidencePath, participant_root_removed: !fs.existsSync(participantRoot), private_grader_root_removed: !fs.existsSync(privateRoot) }, null, 2));
