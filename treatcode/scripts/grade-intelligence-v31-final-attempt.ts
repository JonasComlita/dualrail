import { createHash } from "node:crypto";
import { existsSync } from "node:fs";
import { mkdir, readFile, stat, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { IntelligenceV31FinalCommandRegistry } from "../src/intelligenceV31FinalRegistry";
import { intelligenceV31AttemptId, verifyIntelligenceV31InfrastructureAttestation, verifyIntelligenceV31ModelExecutionAttestation } from "../src/intelligenceV31Infrastructure";
import { gradeIntelligenceV31RepositoryAttempt, verifyIntelligenceV31FrozenSuite } from "../src/intelligenceV31Repository";
import { selectIntelligenceV31CalibrationFamilies } from "./intelligence-v31-calibration-assignments.mjs";
import { TRACK_IDS } from "./intelligence-v31-author-shards.mjs";

const argument = (name: string): string | undefined => process.argv.find((value) => value.startsWith(`--${name}=`))?.slice(name.length + 3);
const attemptArgument = argument("attempt-root");
const attestationArgument = argument("infrastructure-attestation") || process.env.TREATCODE_V31_INFRA_ATTESTATION;
const executionAttestationArgument = argument("execution-attestation") || process.env.TREATCODE_V31_EXECUTION_ATTESTATION;
const externalEvidenceArgument = argument("evidence-root") || process.env.TREATCODE_V31_EVIDENCE_ROOT;
if (!attemptArgument) throw new Error("usage: --attempt-root=<path> [--infrastructure-attestation=<signed JSON> --execution-attestation=<task-specific signed JSON> --evidence-root=<provider-mounted root>]");
const repositoryRoot = path.resolve(import.meta.dir, "..", "..");
const corpusRoot = path.join(repositoryRoot, "benchmarks", "intelligence-v3.1");
const attemptRoot = path.resolve(attemptArgument);
const attestationPath = attestationArgument ? path.resolve(attestationArgument) : null;
const executionAttestationPath = executionAttestationArgument ? path.resolve(executionAttestationArgument) : null;
const defaultEvidenceRoot = path.join(repositoryRoot, "build", "intelligence-v31-development-evidence");
const externalEvidenceRoot = path.resolve(externalEvidenceArgument || defaultEvidenceRoot);
const attempt = JSON.parse(await readFile(path.join(attemptRoot, "attempt.json"), "utf8"));
if (attempt.schema !== "treatcode.intelligence.final-attempt.v3.1" || attempt.version !== "3.1" || !["calibration", "subject", "development"].includes(attempt.phase) || !/^TC-V31-FINAL-\d{3}$/.test(attempt.task_id) || !/^[A-Za-z0-9._-]+$/.test(attempt.run_id)) throw new Error("final attempt metadata is invalid");
if (attempt.phase === "calibration") {
  if (!["weak", "medium", "frontier"].includes(attempt.cohort) || !/^[A-Za-z0-9._-]+$/.test(attempt.configuration || "") || ![1, 2, 3].includes(attempt.run_number) || attempt.suite_sha256 !== null) throw new Error("calibration attempt slot is invalid");
} else if (attempt.phase === "subject" && (attempt.cohort !== null || attempt.configuration !== null || attempt.run_number !== null || !/^[a-f0-9]{64}$/.test(attempt.suite_sha256 || "") || !((attempt.model === "gpt-5.6-luna" && attempt.reasoning_effort === "max") || (attempt.model === "gpt-5.6-sol" && attempt.reasoning_effort === "high")))) {
  throw new Error("subject attempt slot is invalid");
} else if (attempt.phase === "development" && (attempt.cohort !== null || attempt.configuration !== null || attempt.run_number !== null || attempt.suite_sha256 !== null || !((attempt.model === "gpt-5.6-luna" && attempt.reasoning_effort === "max") || (attempt.model === "gpt-5.6-sol" && attempt.reasoning_effort === "high")))) {
  throw new Error("development attempt slot is invalid");
}
const completionPath = path.join(attemptRoot, "completion.json");
const discussionPath = path.join(attemptRoot, "discussion.md");
const privateRoot = process.env.TREATCODE_V31_FINAL_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v31-private");
const graderRoot = path.join(privateRoot, "final-graders", attempt.task_id);
const grader = JSON.parse(await readFile(path.join(graderRoot, "grader.repository.v3.1.json"), "utf8"));
const privateTask = JSON.parse(await readFile(path.join(graderRoot, "task.private.v3.1.json"), "utf8"));
const taskTrack = privateTask.task?.track;
if (!TRACK_IDS.includes(taskTrack)) throw new Error("private final task is missing a valid portfolio track");
const completion = JSON.parse(await readFile(completionPath, "utf8"));
if (completion.status !== "completed" || completion.fresh_context !== true || completion.cross_task_memory_disabled !== true || completion.attempt_count !== 1 || !/^[a-f0-9]{64}$/.test(completion.tool_trace_sha256 || "")) throw new Error("incomplete final attempt is not gradeable");
const metricKeys = ["tool_calls", "successful_tool_calls", "tool_errors", "cpu_ms", "memory_mb", "input_tokens", "output_tokens", "repository_reads", "test_runs", "patch_attempts"];
const metrics = completion.metrics === undefined ? null : completion.metrics;
if (metrics !== null && (!metrics || typeof metrics !== "object" || Array.isArray(metrics) || Object.keys(metrics).some((key) => !metricKeys.includes(key)) || metricKeys.some((key) => metrics[key] !== undefined && (!Number.isSafeInteger(metrics[key]) || metrics[key] < 0)))) throw new Error("completion metrics must be nonnegative safe integers from the allowlisted telemetry fields");
const discussion = await readFile(discussionPath, "utf8");
if (discussion.trim().split(/\s+/).filter(Boolean).length < 20) throw new Error("final attempt discussion is incomplete");
const [completionStat, discussionStat] = await Promise.all([stat(completionPath), stat(discussionPath)]);
const submittedAtMs = Math.max(completionStat.mtimeMs, discussionStat.mtimeMs);
const releasedAtMs = Date.parse(attempt.released_at);
if (!Number.isFinite(releasedAtMs) || submittedAtMs < releasedAtMs) throw new Error("final attempt timing evidence is invalid");
const elapsedMs = Math.round(submittedAtMs - releasedAtMs);
const withinBudget = elapsedMs <= 1_200_000;
if (attempt.phase !== "development" && (!attestationPath || !executionAttestationPath || !externalEvidenceArgument || !path.isAbsolute(attestationArgument!) || !path.isAbsolute(executionAttestationArgument!) || !path.isAbsolute(externalEvidenceArgument!) || !existsSync(attestationPath) || !existsSync(executionAttestationPath) || !existsSync(externalEvidenceRoot))) throw new Error("official infrastructure and execution inputs must be existing absolute provider paths");
const infrastructure = attempt.phase === "development" ? null : verifyIntelligenceV31InfrastructureAttestation(JSON.parse(await readFile(attestationPath!, "utf8")), JSON.parse(await readFile(path.join(corpusRoot, "trusted-infrastructure-keys.v3.1.json"), "utf8")));
const executionAttestationBytes = attempt.phase === "development" ? Buffer.from("") : await readFile(executionAttestationPath!);
const execution = attempt.phase === "development" ? null : verifyIntelligenceV31ModelExecutionAttestation(JSON.parse(executionAttestationBytes.toString("utf8")), JSON.parse(await readFile(path.join(corpusRoot, "trusted-infrastructure-keys.v3.1.json"), "utf8")), {
  phase: attempt.phase,
  released_at: attempt.released_at,
  run_id: attempt.run_id,
  task_id: attempt.task_id,
  track: taskTrack,
  model: attempt.model,
  reasoning_effort: attempt.reasoning_effort,
  participant_bundle_sha256: attempt.participant_bundle_sha256,
  suite_sha256: attempt.suite_sha256 || null,
  cohort: attempt.cohort,
  configuration: attempt.configuration,
  run_number: attempt.run_number,
  fresh_context: true,
  cross_task_memory_disabled: true,
  attempt_count: 1,
  tool_trace_sha256: completion.tool_trace_sha256,
});
if (grader.baseline_bundle_sha256 !== attempt.participant_bundle_sha256) throw new Error("attempt participant bundle does not match its private grader");
if (attempt.phase === "calibration") {
  const plan = JSON.parse(await readFile(path.join(corpusRoot, "calibration-plan.v3.1.json"), "utf8"));
  const assignment = selectIntelligenceV31CalibrationFamilies(attempt.task_id, privateTask.authorship.model, plan.cohorts[attempt.cohort].assignments).find((item: { model_family: string; reasoning_effort: string }) => item.model_family === attempt.model && item.reasoning_effort === attempt.reasoning_effort);
  if (!assignment || attempt.configuration !== `${attempt.model}-${attempt.reasoning_effort}`) throw new Error("calibration attempt is not assigned by the frozen development plan");
}
if (attempt.phase === "subject") {
  const frozen = JSON.parse(await readFile(path.join(corpusRoot, "frozen-suite.v3.1.json"), "utf8"));
  const issues = verifyIntelligenceV31FrozenSuite(frozen);
  if (issues.length || frozen.suite_sha256 !== attempt.suite_sha256 || frozen.tasks.find((item: { task_id: string }) => item.task_id === attempt.task_id)?.participant_bundle_hash !== attempt.participant_bundle_sha256) throw new Error("subject attempt is not bound to the verified frozen suite");
}
const attemptId = intelligenceV31AttemptId(attempt);
const evidenceRoot = path.join(externalEvidenceRoot, attempt.phase, attempt.run_id, attempt.task_id, attemptId);
const relativeEvidence = path.relative(externalEvidenceRoot, evidenceRoot);
if (!relativeEvidence || relativeEvidence === ".." || relativeEvidence.startsWith(`..${path.sep}`) || path.isAbsolute(relativeEvidence)) throw new Error("final evidence root escaped its provider mount");
await mkdir(evidenceRoot, { recursive: true });
const report = await gradeIntelligenceV31RepositoryAttempt({
  task_id: attempt.task_id,
  track: taskTrack,
  attempt_id: attemptId,
  repository_root: repositoryRoot,
  subject_workspace_root: attemptRoot,
  baseline_root: path.join(attemptRoot, "baseline"),
  submission_root: path.join(attemptRoot, "submission"),
  grader_root: graderRoot,
  evidence_root: evidenceRoot,
  released_bundle_hash: attempt.participant_bundle_sha256,
  command_registry: new IntelligenceV31FinalCommandRegistry(repositoryRoot, privateRoot),
  require_verified_infrastructure: attempt.phase !== "development",
  official_run: false,
  network_isolation: infrastructure?.network_isolation,
  resource_isolation: infrastructure?.resource_isolation,
  evidence_store: infrastructure?.evidence_store,
  metrics: metrics || undefined,
});
const completedAt = new Date(submittedAtMs).toISOString();
const grade = {
  schema: "treatcode.intelligence.final-subject-grade.v3.1",
  version: "3.1",
  official: false,
  phase: attempt.phase,
  run_id: attempt.run_id,
  task_id: attempt.task_id,
  track: taskTrack,
  model: attempt.model,
  reasoning_effort: attempt.reasoning_effort,
  suite_sha256: attempt.suite_sha256 || null,
  participant_bundle_sha256: attempt.participant_bundle_sha256,
  released_at: attempt.released_at,
  submitted_at: completedAt,
  elapsed_ms: elapsedMs,
  budget_ms: 1_200_000,
  within_budget: withinBudget,
  passed: report.passed && withinBudget,
  completion_status: withinBudget ? "completed" : "timeout",
  sealed_report: report,
  efficiency: metrics,
  agent_execution: { fresh_context: true, cross_task_memory_disabled: true, attempt_count: 1, tool_trace_sha256: completion.tool_trace_sha256, model_execution_verified: Boolean(execution), execution_provider_id: execution?.provider_id || null, execution_key_id: execution?.key_id || null, execution_attestation_sha256: execution ? createHash("sha256").update(executionAttestationBytes).digest("hex") : null },
  discussion: { scored: false, word_count: discussion.trim().split(/\s+/).filter(Boolean).length, sha256: createHash("sha256").update(discussion).digest("hex") },
};
await writeFile(path.join(attemptRoot, "sealed-grade.json"), `${JSON.stringify(grade, null, 2)}\n`, { flag: "wx", mode: 0o444 });
if (attempt.phase === "calibration") {
  const observation = {
    schema: "treatcode.intelligence.calibration-observation.v3.1",
    version: "3.1",
    task_id: attempt.task_id,
    track: taskTrack,
    participant_bundle_hash: attempt.participant_bundle_sha256,
    attempt_id: attemptId,
    cohort: attempt.cohort,
    model_family: attempt.model,
    configuration: attempt.configuration,
    run_number: attempt.run_number,
    fresh_context: true,
    subject_family_excluded: true,
    model_execution_verified: true,
    execution_attestation_sha256: grade.agent_execution.execution_attestation_sha256 || null,
    network_isolation_verified: report.network_isolation_verified,
    resource_limits_verified: report.resource_limits_verified,
    append_only_evidence_verified: report.append_only_evidence_verified,
    passed: grade.passed,
    completion_status: grade.completion_status,
    released_at: attempt.released_at,
    submitted_at: completedAt,
    evidence_hash: report.evidence_hash,
    official: false,
  };
  const observationRoot = path.join(externalEvidenceRoot, "observations");
  await mkdir(observationRoot, { recursive: true });
  await writeFile(path.join(observationRoot, `${attemptId}.json`), `${JSON.stringify(observation, null, 2)}\n`, { flag: "wx", mode: 0o444 });
}
console.log(JSON.stringify({ status: "graded", official: grade.official, phase: grade.phase, task_id: grade.task_id, model: grade.model, passed: grade.passed, elapsed_ms: grade.elapsed_ms, evidence_hash: report.evidence_hash }));
