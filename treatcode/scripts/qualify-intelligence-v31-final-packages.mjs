import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const corpusRoot = path.join(repositoryRoot, "benchmarks", "intelligence-v3.1");
const participantRoot = path.join(corpusRoot, "final-candidates");
const privateRoot = process.env.TREATCODE_V31_FINAL_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v31-private");
const graderRoot = path.join(privateRoot, "final-graders");
const evaluator = path.join(appRoot, "scripts", "intelligence-v31-final-task-evaluator.mjs");
const workspaceRoot = path.join(repositoryRoot, "build", "intelligence-v31-qualification-workspaces");
const evidenceRoot = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v31-final-qualification");
const argument = (name) => process.argv.find((value) => value.startsWith(`--${name}=`))?.slice(name.length + 3);
const requestedTask = argument("task");
const start = Math.max(1, Number(argument("start") || 1));
const limit = Math.max(1, Number(argument("limit") || 100));
const sha256 = (value) => crypto.createHash("sha256").update(value).digest("hex");

function safeTaskId(value) {
  return /^TC-V31-FINAL-\d{3}$/.test(value);
}

function safelyRemove(root) {
  const resolved = path.resolve(root);
  const allowed = path.resolve(workspaceRoot);
  const relative = path.relative(allowed, resolved);
  if (!relative || relative === ".." || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) throw new Error(`refusing unsafe qualification cleanup: ${resolved}`);
  fs.rmSync(resolved, { recursive: true, force: true });
}

function cleanupSession(workspace) {
  const key = sha256(path.resolve(workspace)).slice(0, 32);
  const target = path.join(os.tmpdir(), `treatcode-v31-final-${key}`);
  const relative = path.relative(path.resolve(os.tmpdir()), path.resolve(target));
  if (!relative.startsWith("treatcode-v31-final-") || relative.includes(path.sep)) throw new Error("refusing unsafe evaluator-session cleanup");
  fs.rmSync(target, { recursive: true, force: true });
}

function replaceWorkspace(from, to) {
  if (!fs.existsSync(from) || !fs.statSync(from).isDirectory()) throw new Error(`qualification variant is missing: ${from}`);
  if (fs.existsSync(to)) safelyRemove(to);
  fs.mkdirSync(path.dirname(to), { recursive: true });
  fs.cpSync(from, to, { recursive: true, errorOnExist: true, force: false });
}

function runSuite(taskId, workspace, suite) {
  const started = Date.now();
  const result = spawnSync(process.execPath, [evaluator, `--workspace=${workspace}`, `--private-root=${privateRoot}`, `--task=${taskId}`, `--suite=${suite}`], {
    cwd: appRoot,
    encoding: "utf8",
    shell: false,
    windowsHide: true,
    timeout: 1_150_000,
    maxBuffer: 8 * 1_048_576,
  });
  return {
    suite,
    passed: result.status === 0 && !result.error,
    exit_code: result.status,
    error: result.error?.message || null,
    wall_clock_ms: Date.now() - started,
    output_sha256: sha256(`${result.stdout || ""}\n${result.stderr || ""}`),
  };
}

function runUntilFailure(taskId, workspace, suites) {
  const outcomes = [];
  for (const suite of suites) {
    const outcome = runSuite(taskId, workspace, suite);
    outcomes.push(outcome);
    if (!outcome.passed) break;
  }
  return outcomes;
}

if (!fs.existsSync(participantRoot) || !fs.existsSync(graderRoot)) throw new Error("final participant and private grader packages must be generated first");
const allTaskIds = fs.readdirSync(participantRoot).filter(safeTaskId).sort();
if (allTaskIds.length !== 100) throw new Error("qualification requires exactly 100 generated candidates");
const selectedTaskIds = requestedTask
  ? (safeTaskId(requestedTask) && allTaskIds.includes(requestedTask) ? [requestedTask] : (() => { throw new Error("requested task is invalid or unavailable"); })())
  : allTaskIds.slice(start - 1, start - 1 + limit);
fs.mkdirSync(workspaceRoot, { recursive: true });
fs.mkdirSync(evidenceRoot, { recursive: true });

const completed = [];
for (const taskId of selectedTaskIds) {
  const taskGraderRoot = path.join(graderRoot, taskId);
  const graderManifest = JSON.parse(fs.readFileSync(path.join(taskGraderRoot, "grader.repository.v3.1.json"), "utf8"));
  if (!/^[a-f0-9]{64}$/.test(graderManifest.baseline_bundle_sha256)) throw new Error(`${taskId}: private grader has no valid participant hash`);
  const evidencePath = path.join(evidenceRoot, `${taskId}-${graderManifest.baseline_bundle_sha256.slice(0, 16)}.json`);
  if (fs.existsSync(evidencePath)) {
    const existing = JSON.parse(fs.readFileSync(evidencePath, "utf8"));
    if (existing.task_id !== taskId || existing.participant_bundle_sha256 !== graderManifest.baseline_bundle_sha256 || !existing.complete) throw new Error(`${taskId}: existing qualification evidence is invalid`);
    completed.push(existing);
    continue;
  }
  const workspace = path.join(workspaceRoot, taskId);
  const privateTask = JSON.parse(fs.readFileSync(path.join(taskGraderRoot, "task.private.v3.1.json"), "utf8"));
  const variants = [];
  try {
    replaceWorkspace(path.join(participantRoot, taskId, "participant"), workspace);
    const starterOutcomes = runUntilFailure(taskId, workspace, ["public", "behavioral", "adversarial"]);
    variants.push({ id: "starter", expected: "fail", caught: starterOutcomes.some((item) => !item.passed), outcomes: starterOutcomes });

    for (const mutantRelative of privateTask.mutant_roots) {
      const mutantRoot = path.resolve(taskGraderRoot, mutantRelative);
      const relative = path.relative(taskGraderRoot, mutantRoot);
      if (!relative || relative === ".." || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) throw new Error(`${taskId}: unsafe mutant root`);
      replaceWorkspace(mutantRoot, workspace);
      const outcomes = runUntilFailure(taskId, workspace, ["behavioral", "adversarial"]);
      variants.push({ id: mutantRelative.replaceAll("/", "-"), expected: "fail", caught: outcomes.some((item) => !item.passed), outcomes });
    }

    replaceWorkspace(path.join(taskGraderRoot, "reference"), workspace);
    const referenceOutcomes = ["public", "behavioral", "adversarial", "performance"].map((suite) => runSuite(taskId, workspace, suite));
    variants.push({ id: "reference", expected: "pass", caught: referenceOutcomes.every((item) => item.passed), outcomes: referenceOutcomes });
  } finally {
    cleanupSession(workspace);
    if (fs.existsSync(workspace)) safelyRemove(workspace);
  }
  const starter = variants.find((item) => item.id === "starter");
  const reference = variants.find((item) => item.id === "reference");
  const mutants = variants.filter((item) => item.id.startsWith("mutants-"));
  const record = {
    schema: "treatcode.intelligence.final-qualification-evidence.v3.1",
    version: "3.1",
    official: false,
    task_id: taskId,
    participant_bundle_sha256: graderManifest.baseline_bundle_sha256,
    completed_at: new Date().toISOString(),
    complete: true,
    passed: Boolean(starter?.caught && reference?.caught && mutants.length === privateTask.mutant_roots.length && mutants.every((item) => item.caught)),
    starter_failed: Boolean(starter?.caught),
    reference_passed: Boolean(reference?.caught),
    mutants_caught: mutants.filter((item) => item.caught).length,
    mutants_total: mutants.length,
    variants,
  };
  fs.writeFileSync(evidencePath, `${JSON.stringify(record, null, 2)}\n`, { flag: "wx", mode: 0o444 });
  completed.push(record);
  console.log(JSON.stringify({ task_id: taskId, passed: record.passed, starter_failed: record.starter_failed, reference_passed: record.reference_passed, mutants: `${record.mutants_caught}/${record.mutants_total}` }));
}

const evidenceRecords = fs.readdirSync(evidenceRoot).filter((file) => /^TC-V31-FINAL-\d{3}-[a-f0-9]{16}\.json$/.test(file)).map((file) => JSON.parse(fs.readFileSync(path.join(evidenceRoot, file), "utf8")));
const currentEvidence = allTaskIds.map((taskId) => {
  const grader = JSON.parse(fs.readFileSync(path.join(graderRoot, taskId, "grader.repository.v3.1.json"), "utf8"));
  return evidenceRecords.find((item) => item.task_id === taskId && item.participant_bundle_sha256 === grader.baseline_bundle_sha256);
});
if (currentEvidence.every(Boolean)) {
  const hashSet = sha256(currentEvidence.map((item) => `${item.task_id}\0${item.participant_bundle_sha256}\n`).join(""));
  const aggregate = { schema: "treatcode.intelligence.final-qualification-aggregate.v3.1", version: "3.1", official: false, completed_at: new Date().toISOString(), task_count: 100, passed_tasks: currentEvidence.filter((item) => item.passed).length, failed_tasks: currentEvidence.filter((item) => !item.passed).map((item) => item.task_id), complete: true, participant_hash_set_sha256: hashSet };
  const aggregatePath = path.join(path.dirname(evidenceRoot), `intelligence-v31-final-qualification-${hashSet.slice(0, 16)}.json`);
  if (!fs.existsSync(aggregatePath)) fs.writeFileSync(aggregatePath, `${JSON.stringify(aggregate, null, 2)}\n`, { flag: "wx", mode: 0o444 });
  console.log(JSON.stringify(aggregate, null, 2));
}
