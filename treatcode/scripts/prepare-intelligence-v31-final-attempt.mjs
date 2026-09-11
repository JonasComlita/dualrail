import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { isExcludedSubjectModel, TRACK_IDS } from "./intelligence-v31-author-shards.mjs";
import { selectIntelligenceV31CalibrationFamilies } from "./intelligence-v31-calibration-assignments.mjs";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const corpusRoot = path.join(repositoryRoot, "benchmarks", "intelligence-v3.1");
const participantRoot = path.join(corpusRoot, "final-candidates");
const privateRoot = process.env.TREATCODE_V31_FINAL_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v31-private");
const graderRoot = path.join(privateRoot, "final-graders");
const frozenPath = path.join(corpusRoot, "frozen-suite.v3.1.json");
const argument = (name) => process.argv.find((value) => value.startsWith(`--${name}=`))?.slice(name.length + 3);
const phase = argument("phase");
const runId = argument("run-id") || "";
const taskId = argument("task") || "";
const model = argument("model") || "";
const reasoningEffort = argument("reasoning") || "";
const cohort = argument("cohort");
const configuration = argument("configuration");
const runNumber = Number(argument("run-number") || 0);
const SAFE = /^[A-Za-z0-9._-]+$/;

if (!SAFE.test(runId) || !/^TC-V31-FINAL-\d{3}$/.test(taskId) || !SAFE.test(model) || !SAFE.test(reasoningEffort) || !["calibration", "subject", "development"].includes(phase)) throw new Error("usage: --phase=calibration|subject|development --run-id=<safe> --task=TC-V31-FINAL-NNN --model=<safe> --reasoning=<safe>");
if (phase === "calibration") {
  if (isExcludedSubjectModel(model)) throw new Error("subject families cannot receive calibration tasks");
  if (!["weak", "medium", "frontier"].includes(cohort) || !SAFE.test(configuration || "") || ![1, 2, 3].includes(runNumber)) throw new Error("calibration attempts require a cohort, configuration, and run number 1..3");
} else if (phase === "subject") {
  const validSubject = (model === "gpt-5.6-luna" && reasoningEffort === "max") || (model === "gpt-5.6-sol" && reasoningEffort === "high");
  if (!validSubject || cohort || configuration || runNumber) throw new Error("subject attempts are fixed to Luna max or Sol high and cannot carry calibration metadata");
  if (!fs.existsSync(frozenPath)) throw new Error("subject release is blocked until the v3.1 suite is frozen");
} else {
  const validDevelopmentModel = (model === "gpt-5.6-luna" && reasoningEffort === "max") || (model === "gpt-5.6-sol" && reasoningEffort === "high");
  if (!validDevelopmentModel || cohort || configuration || runNumber) throw new Error("development attempts are fixed to Luna max or Sol high and cannot carry calibration metadata");
}

const source = path.join(participantRoot, taskId, "participant");
const privateGrader = path.join(graderRoot, taskId, "grader.repository.v3.1.json");
const privateTaskPath = path.join(graderRoot, taskId, "task.private.v3.1.json");
if (!fs.existsSync(source) || !fs.existsSync(privateGrader) || !fs.existsSync(privateTaskPath)) throw new Error("the requested final participant package is unavailable");
const grader = JSON.parse(fs.readFileSync(privateGrader, "utf8"));
const privateTask = JSON.parse(fs.readFileSync(privateTaskPath, "utf8"));
const taskTrack = privateTask.task?.track;
if (!TRACK_IDS.includes(taskTrack)) throw new Error("final task package is missing a valid portfolio track");
if (phase === "calibration") {
  const plan = JSON.parse(fs.readFileSync(path.join(corpusRoot, "calibration-plan.v3.1.json"), "utf8"));
  const selectedFamilies = selectIntelligenceV31CalibrationFamilies(taskId, privateTask.authorship.model, plan.cohorts[cohort].assignments);
  const assignment = selectedFamilies.find((item) => item.model_family === model && item.reasoning_effort === reasoningEffort);
  if (!assignment || configuration !== `${model}-${reasoningEffort}`) throw new Error("calibration configuration is not assigned by the frozen development plan");
}
let suiteSha256 = null;
if (phase === "subject") {
  const frozen = JSON.parse(fs.readFileSync(frozenPath, "utf8"));
  const frozenTask = frozen.tasks.find((task) => task.task_id === taskId);
  if (!frozenTask || frozenTask.track !== taskTrack || frozenTask.participant_bundle_hash !== grader.baseline_bundle_sha256 || frozen.task_order.indexOf(taskId) < 0) throw new Error("requested task does not match the frozen suite");
  suiteSha256 = frozen.suite_sha256;
}
const runsRoot = path.join(repositoryRoot, "build", phase === "subject" ? "intelligence-v31-final-subject-runs" : phase === "development" ? "intelligence-v31-development-runs" : "intelligence-v31-final-calibration-runs");
const attemptRoot = path.join(runsRoot, runId, taskId, `${model}-${reasoningEffort}${phase === "calibration" ? `-${cohort}-${runNumber}` : ""}`);
const relative = path.relative(runsRoot, attemptRoot);
if (!relative || relative === ".." || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) throw new Error("attempt root escaped its controlled run directory");
if (fs.existsSync(attemptRoot)) throw new Error("attempt already exists; final attempts are one-shot");
const releasedAt = new Date().toISOString();
fs.mkdirSync(attemptRoot, { recursive: true });
const metadata = {
  schema: "treatcode.intelligence.final-attempt.v3.1",
  version: "3.1",
  official: false,
  phase,
  run_id: runId,
  task_id: taskId,
  track: taskTrack,
  model,
  reasoning_effort: reasoningEffort,
  cohort: phase === "calibration" ? cohort : null,
  configuration: phase === "calibration" ? configuration : null,
  run_number: phase === "calibration" ? runNumber : null,
  suite_sha256: suiteSha256,
  participant_bundle_sha256: grader.baseline_bundle_sha256,
  released_at: releasedAt,
  fresh_context_required: true,
  cross_task_memory_allowed: false,
  attempt_count: 1,
  retry_permitted: false,
  completion_contract: { required_files: ["submission", "discussion.md", "completion.json"], discussion_scored: false, optional_metrics: ["tool_calls", "successful_tool_calls", "tool_errors", "cpu_ms", "memory_mb", "input_tokens", "output_tokens", "repository_reads", "test_runs", "patch_attempts"] },
};
fs.writeFileSync(path.join(attemptRoot, "attempt.json"), `${JSON.stringify(metadata, null, 2)}\n`, { flag: "wx" });
fs.cpSync(source, path.join(attemptRoot, "baseline"), { recursive: true, errorOnExist: true, force: false });
fs.cpSync(source, path.join(attemptRoot, "submission"), { recursive: true, errorOnExist: true, force: false });
fs.writeFileSync(path.join(attemptRoot, "INSTRUCTIONS.md"), `# ${taskId}\n\nTrack: ${taskTrack}\n\nWork only in \`submission/\`. Run the public command through the benchmark service, save a concise plain-English solution discussion in \`discussion.md\`, then write \`completion.json\` with {"status":"completed","fresh_context":true,"cross_task_memory_disabled":true,"attempt_count":1,"tool_trace_sha256":"<sha256>","metrics":{"tool_calls":0,"successful_tool_calls":0,"tool_errors":0}}. The optional metrics object may include only measured tool, CPU, memory, token, repository-read, test-run, and patch-attempt counts; unknown or invented fields are rejected. The clock began at ${releasedAt}; this attempt is one-shot and must remain isolated from every other task. This is a disposable development run and cannot create an official holdout result.\n`, { flag: "wx" });
console.log(JSON.stringify({ status: "released", official: false, phase, run_id: runId, task_id: taskId, model, reasoning_effort: reasoningEffort, attempt_root: attemptRoot, participant_bundle_sha256: grader.baseline_bundle_sha256, suite_sha256: suiteSha256 }, null, 2));
