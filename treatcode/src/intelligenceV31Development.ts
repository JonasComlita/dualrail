import { createHash } from "node:crypto";
import { existsSync, readdirSync, readFileSync } from "node:fs";
import path from "node:path";
import { compareIntelligenceV31, type IntelligenceV31ComparisonResult, type IntelligenceV31TaskOutcome } from "./intelligenceV31Comparison";
import { compareIntelligenceV31Dimensions, INTELLIGENCE_V31_TRACK_IDS, type IntelligenceV31DimensionComparison, type IntelligenceV31DimensionObservation, type IntelligenceV31TrackId } from "./intelligenceV31Tracks";

export const INTELLIGENCE_V31_DEVELOPMENT_COMPARISON_SCHEMA = "treatcode.intelligence.development-comparison.v3.1" as const;

const GRADE_SCHEMA = "treatcode.intelligence.final-subject-grade.v3.1";
const PILOT_GRADE_SCHEMA = "treatcode.intelligence.pilot-subject-grade.v3.1";
const SAFE_RUN_ID = /^[A-Za-z0-9._-]+$/;
const TASK_ID = /^TC-V31-(?:FINAL|PILOT)-\d{3}$/;
const HASH = /^[a-f0-9]{64}$/;

export type IntelligenceV31DevelopmentSubject = {
  model: "gpt-5.6-luna" | "gpt-5.6-sol";
  reasoning_effort: "max" | "high";
  directory: string;
};

export interface IntelligenceV31DevelopmentComparison {
  schema: typeof INTELLIGENCE_V31_DEVELOPMENT_COMPARISON_SCHEMA;
  version: "3.1";
  phase: "development";
  official: boolean;
  run_id: string;
  generated_at: string;
  complete: true;
  source: {
    run_root: string;
    grade_count: number;
    grade_sha256: string[];
    task_ids: string[];
    participant_bundle_hashes: string[];
  };
  luna: { model: "gpt-5.6-luna"; reasoning_effort: "max"; outcomes: IntelligenceV31TaskOutcome[] };
  sol: { model: "gpt-5.6-sol"; reasoning_effort: "high"; outcomes: IntelligenceV31TaskOutcome[] };
  comparison: IntelligenceV31ComparisonResult;
  task_count: number;
  luna_score: number;
  sol_score: number;
  sol_lead: number;
  target_reproduced: boolean;
  paired_confidence_interval: IntelligenceV31ComparisonResult["paired_confidence_interval"];
  exact_sign_test: IntelligenceV31ComparisonResult["exact_sign_test"];
  discrimination: IntelligenceV31ComparisonResult["discrimination"];
  dimensions: IntelligenceV31DimensionComparison;
  task_matrix: IntelligenceV31ComparisonResult["task_matrix"];
  blockers: string[];
}

function sha256File(filePath: string): string {
  return createHash("sha256").update(readFileSync(filePath)).digest("hex");
}

function fail(message: string): never {
  throw new Error(`development comparison: ${message}`);
}

function requireBoolean(value: unknown, label: string): asserts value is boolean {
  if (typeof value !== "boolean") fail(`${label} must be a boolean`);
}

function requireHash(value: unknown, label: string): asserts value is string {
  if (typeof value !== "string" || !HASH.test(value)) fail(`${label} must be a lowercase SHA-256 hash`);
}

function participantBundleHash(grade: any, label: string): string {
  const value = grade.participant_bundle_sha256 || grade.sealed_report?.participant_bundle_hash;
  // Bundle hashes are useful provenance when a grader supplies them, but they
  // are no longer a prerequisite for producing a paired score.  Development
  // and pilot evidence written by older runners may not carry one.
  if (value === undefined || value === null || value === "") return "unbound";
  if (typeof value !== "string" || !HASH.test(value)) fail(`${label} must be a lowercase SHA-256 hash when supplied`);
  return value;
}

function assertGrade(grade: any, runId: string, taskId: string, subject: IntelligenceV31DevelopmentSubject): void {
  if (!grade || typeof grade !== "object") fail(`${taskId}/${subject.directory}: grade is not an object`);
  const pilot = grade.schema === PILOT_GRADE_SCHEMA;
  if ((!pilot && (grade.schema !== GRADE_SCHEMA || !["development", "subject", "holdout"].includes(grade.phase) || grade.version !== "3.1")) || (pilot && grade.version !== undefined && grade.version !== "3.1")) fail(`${taskId}/${subject.directory}: grade envelope is invalid`);
  if (pilot && (!grade.subject_timing || grade.subject_timing.within_budget !== true)) fail(`${taskId}/${subject.directory}: pilot grade lacks completed timing evidence`);
  if (grade.run_id !== runId || grade.task_id !== taskId || grade.model !== subject.model || grade.reasoning_effort !== subject.reasoning_effort) fail(`${taskId}/${subject.directory}: grade identity does not match the run slot`);
  const bundleHash = participantBundleHash(grade, `${taskId}/${subject.directory} participant bundle hash`);
  if (!grade.sealed_report || grade.sealed_report.sealed !== true || grade.sealed_report.task_id !== taskId) fail(`${taskId}/${subject.directory}: sealed repository report is invalid`);
  requireBoolean(grade.passed, `${taskId}/${subject.directory} passed`);
  if (!pilot) requireBoolean(grade.within_budget, `${taskId}/${subject.directory} within_budget`);
  requireBoolean(grade.sealed_report.passed, `${taskId}/${subject.directory} sealed_report.passed`);
  const withinBudget = pilot ? grade.subject_timing.within_budget : grade.within_budget;
  if (grade.passed !== (grade.sealed_report.passed && withinBudget)) fail(`${taskId}/${subject.directory}: grade pass flag disagrees with sealed report and budget`);
  if ((grade.track !== undefined && !INTELLIGENCE_V31_TRACK_IDS.includes(grade.track)) || (grade.sealed_report.track !== undefined && !INTELLIGENCE_V31_TRACK_IDS.includes(grade.sealed_report.track))) fail(`${taskId}/${subject.directory}: grade track id is invalid`);
  if (!['completed', 'timeout', 'subject_deadline_or_completion_evidence_failed'].includes(grade.completion_status)) fail(`${taskId}/${subject.directory}: completion status is invalid`);
  if (grade.agent_execution && (!Number.isInteger(grade.agent_execution.attempt_count) || grade.agent_execution.attempt_count < 1)) fail(`${taskId}/${subject.directory}: execution attempt count is invalid`);
  if (grade.sealed_report.participant_bundle_hash !== undefined && grade.sealed_report.participant_bundle_hash !== "unbound") requireHash(grade.sealed_report.participant_bundle_hash, `${taskId}/${subject.directory} sealed participant bundle hash`);
  if (grade.sealed_report.submission_bundle_hash !== undefined) requireHash(grade.sealed_report.submission_bundle_hash, `${taskId}/${subject.directory} submission bundle hash`);
  if (grade.sealed_report.grader_bundle_hash !== undefined) requireHash(grade.sealed_report.grader_bundle_hash, `${taskId}/${subject.directory} grader bundle hash`);
  if (grade.sealed_report.evidence_hash !== undefined) requireHash(grade.sealed_report.evidence_hash, `${taskId}/${subject.directory} evidence hash`);
  if (grade.sealed_report.participant_bundle_hash && bundleHash !== "unbound" && grade.sealed_report.participant_bundle_hash !== bundleHash) fail(`${taskId}/${subject.directory}: grade and sealed report participant hashes differ`);
}

function dimensionObservation(grade: any, taskId: string): IntelligenceV31DimensionObservation {
  const report = grade.sealed_report || {};
  const candidateTrack = grade.track || report.track;
  const track: IntelligenceV31TrackId = INTELLIGENCE_V31_TRACK_IDS.includes(candidateTrack) ? candidateTrack : "repository_repair";
  const robustnessTotal = Number(report.behavioral_total || 0) + Number(report.adversarial_total || 0);
  const robustness = robustnessTotal > 0
    ? (Number(report.behavioral_passed || 0) + Number(report.adversarial_passed || 0)) / robustnessTotal
    : null;
  const efficiency = grade.efficiency || grade.agent_execution?.efficiency || {};
  const discussion = grade.discussion;
  return {
    task_id: taskId,
    track,
    correctness: Boolean(grade.passed),
    robustness,
    wall_clock_ms: Number.isFinite(report.wall_clock_ms) ? report.wall_clock_ms : Number.isFinite(grade.elapsed_ms) ? grade.elapsed_ms : null,
    latency_budget_ms: Number.isFinite(grade.budget_ms) ? grade.budget_ms : Number.isFinite(grade.subject_timing?.budget_ms) ? grade.subject_timing.budget_ms : 1_200_000,
    cpu_ms: Number.isFinite(report.cpu_ms) ? report.cpu_ms : null,
    cpu_budget_ms: Number.isFinite(report.cpu_budget_ms) ? report.cpu_budget_ms : null,
    memory_mb: Number.isFinite(report.memory_mb) ? report.memory_mb : null,
    memory_budget_mb: Number.isFinite(report.memory_budget_mb) ? report.memory_budget_mb : null,
    tool_calls: Number.isFinite(efficiency.tool_calls) ? efficiency.tool_calls : null,
    successful_tool_calls: Number.isFinite(efficiency.successful_tool_calls) ? efficiency.successful_tool_calls : null,
    tool_errors: Number.isFinite(efficiency.tool_errors) ? efficiency.tool_errors : null,
    discussion_quality: discussion?.scored === true && Number.isFinite(discussion.quality_percent) ? discussion.quality_percent : null,
    telemetry_source: report.telemetry_source || (grade.efficiency ? "participant_reported" : undefined),
  };
}

function taskDirectories(runRoot: string): string[] {
  if (!existsSync(runRoot)) fail(`run root is unavailable: ${runRoot}`);
  const entries = readdirSync(runRoot, { withFileTypes: true });
  const unexpected = entries.filter((entry) => !entry.isDirectory() && entry.name !== "run.json").map((entry) => entry.name);
  if (unexpected.length) fail(`run root contains unexpected files: ${unexpected.join(", ")}`);
  const tasks = entries.filter((entry) => entry.isDirectory()).map((entry) => entry.name).sort();
  if (!tasks.length) fail("no task attempts were found");
  if (tasks.some((taskId) => !TASK_ID.test(taskId))) fail(`run contains an invalid task directory: ${tasks.find((taskId) => !TASK_ID.test(taskId))}`);
  return tasks;
}

/**
 * Aggregate one disposable development run from the two privileged sealed
 * grades written by the v3.1 grader. This intentionally accepts any non-zero
 * paired task count so a complete run over the tasks that were actually
 * released can produce a score. Publication artifacts are metadata, not
 * prerequisites for scoring.
 */
export function collectIntelligenceV31DevelopmentComparison(runRoot: string, runId: string, generatedAt = new Date().toISOString()): IntelligenceV31DevelopmentComparison {
  if (!SAFE_RUN_ID.test(runId)) fail("run id is unsafe");
  const resolvedRoot = path.resolve(runRoot);
  const taskIds = taskDirectories(resolvedRoot);
  const subjects: [IntelligenceV31DevelopmentSubject, IntelligenceV31DevelopmentSubject] = [
    { model: "gpt-5.6-luna", reasoning_effort: "max", directory: "gpt-5.6-luna-max" },
    { model: "gpt-5.6-sol", reasoning_effort: "high", directory: "gpt-5.6-sol-high" },
  ];
  const outcomes: [IntelligenceV31TaskOutcome[], IntelligenceV31TaskOutcome[]] = [[], []];
  const dimensionObservations: [IntelligenceV31DimensionObservation[], IntelligenceV31DimensionObservation[]] = [[], []];
  const participantHashes = new Set<string>();
  const gradeHashes: string[] = [];
  for (const taskId of taskIds) {
    const taskRoot = path.join(resolvedRoot, taskId);
    const entries = readdirSync(taskRoot, { withFileTypes: true });
    const actualDirectories = entries.filter((entry) => entry.isDirectory()).map((entry) => entry.name).sort();
    const expectedDirectories = subjects.map((subject) => subject.directory).sort();
    if (actualDirectories.join("\0") !== expectedDirectories.join("\0")) fail(`${taskId}: both Luna max and Sol high attempt directories are required and no extras are allowed`);
    let taskParticipantHash: string | null = null;
    subjects.forEach((subject, index) => {
      const gradePath = path.join(taskRoot, subject.directory, "sealed-grade.json");
      if (!existsSync(gradePath)) fail(`${taskId}/${subject.directory}: sealed grade is missing`);
      let grade: any;
      try {
        grade = JSON.parse(readFileSync(gradePath, "utf8"));
      } catch (error) {
        fail(`${taskId}/${subject.directory}: sealed grade is not valid JSON (${error instanceof Error ? error.message : String(error)})`);
      }
      // Pilot grades predate the development-grade envelope and therefore
      // carry their run identity in the immutable attempt manifest beside the
      // sealed grade. Bind that identity before applying the common checks.
      if (grade?.schema === PILOT_GRADE_SCHEMA && grade.run_id === undefined) {
        const attemptPath = path.join(taskRoot, subject.directory, "attempt.json");
        if (!existsSync(attemptPath)) fail(`${taskId}/${subject.directory}: pilot attempt manifest is missing`);
        try {
          const attempt = JSON.parse(readFileSync(attemptPath, "utf8"));
          if (attempt.run_id !== runId || attempt.task_id !== taskId || attempt.model !== subject.model || attempt.reasoning_effort !== subject.reasoning_effort) fail(`${taskId}/${subject.directory}: pilot attempt identity does not match the run slot`);
          grade = { ...grade, run_id: attempt.run_id };
        } catch (error) {
          if (error instanceof Error && error.message.startsWith("development comparison:")) throw error;
          fail(`${taskId}/${subject.directory}: pilot attempt manifest is not valid JSON (${error instanceof Error ? error.message : String(error)})`);
        }
      }
      assertGrade(grade, runId, taskId, subject);
      const bundleHash = participantBundleHash(grade, `${taskId}/${subject.directory} participant bundle hash`);
      if (taskParticipantHash && taskParticipantHash !== bundleHash) fail(`${taskId}: Luna and Sol were not bound to the same participant bundle`);
      taskParticipantHash = bundleHash;
      participantHashes.add(bundleHash);
      gradeHashes.push(sha256File(gradePath));
      const observation = dimensionObservation(grade, taskId);
      outcomes[index].push({ task_id: taskId, passed: grade.passed, track: observation.track });
      dimensionObservations[index].push(observation);
    });
  }
  const comparison = compareIntelligenceV31({
    mode: "development",
    suite_frozen: false,
    suite_hash: null,
    frozen_task_order: taskIds,
    publication_ready: false,
    luna: { model: "gpt-5.6-luna", reasoning_effort: "max", outcomes: outcomes[0] },
    sol: { model: "gpt-5.6-sol", reasoning_effort: "high", outcomes: outcomes[1] },
    dimension_observations: { luna: dimensionObservations[0], sol: dimensionObservations[1] },
  });
  if (!comparison.complete || comparison.blockers.length) fail(`paired development comparison is incomplete: ${comparison.blockers.join("; ")}`);
  const dimensions = comparison.dimensions || compareIntelligenceV31Dimensions(dimensionObservations[0], dimensionObservations[1]);
  return {
    schema: INTELLIGENCE_V31_DEVELOPMENT_COMPARISON_SCHEMA,
    version: "3.1",
    phase: "development",
    official: comparison.official,
    run_id: runId,
    generated_at: generatedAt,
    complete: true,
    source: { run_root: resolvedRoot, grade_count: gradeHashes.length, grade_sha256: [...gradeHashes].sort(), task_ids: taskIds, participant_bundle_hashes: [...participantHashes].sort() },
    luna: { model: "gpt-5.6-luna", reasoning_effort: "max", outcomes: outcomes[0] },
    sol: { model: "gpt-5.6-sol", reasoning_effort: "high", outcomes: outcomes[1] },
    comparison,
    task_count: comparison.task_count,
    luna_score: comparison.luna_score,
    sol_score: comparison.sol_score,
    sol_lead: comparison.sol_lead,
    target_reproduced: comparison.target_reproduced,
    paired_confidence_interval: comparison.paired_confidence_interval,
    exact_sign_test: comparison.exact_sign_test,
    discrimination: comparison.discrimination,
    dimensions,
    task_matrix: comparison.task_matrix,
    blockers: comparison.blockers,
  };
}
