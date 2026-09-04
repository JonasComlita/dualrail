import { createHash, randomUUID } from "node:crypto";
import type { IntelligenceV3Protocol } from "./intelligenceV3";
import { IntelligenceV3AttemptLedger, type IntelligenceV3AttemptOutcome, type IntelligenceV3Usage } from "./intelligenceV3Attempts";

export const INTELLIGENCE_V3_FULL_SUITE_SCHEMA = "treatcode.intelligence.full-suite-run.v3" as const;

export interface IntelligenceV3ParticipantRelease {
  task_id: string;
  participant_root: string;
  participant_bundle_hash: string;
  initial_signal: string;
  public_commands: string[];
}

export interface IntelligenceV3TaskDescriptor {
  task_id: string;
  releaseParticipantTask: () => Promise<IntelligenceV3ParticipantRelease>;
}

export interface IntelligenceV3SubjectContext {
  context_id: string;
  fresh_context: true;
  cross_task_memory_disabled: true;
  context_evidence_hash: string;
}

export interface IntelligenceV3SubjectSubmission {
  submission_artifact_hash: string;
  tool_trace_hash: string;
  budget_evidence_hash: string;
  usage: IntelligenceV3Usage;
  valid_actions: number;
  total_actions: number;
  policy_violations: number;
  discussion_artifact_hash?: string;
}

export interface IntelligenceV3SubjectAdapter {
  createFreshContext(input: { provider: string; model: string; reasoning_effort: string; task_id: string; attempt_id: string }): Promise<IntelligenceV3SubjectContext>;
  executeTask(context: IntelligenceV3SubjectContext, release: IntelligenceV3ParticipantRelease, controls: { budgets: IntelligenceV3Protocol["budgets"]; deadline_ms: number; signal: AbortSignal }): Promise<IntelligenceV3SubjectSubmission>;
  destroyContext(context: IntelligenceV3SubjectContext): Promise<void>;
}

export interface IntelligenceV3PrivilegedGrade {
  correctness_passed: boolean;
  robustness_passed: number;
  robustness_total: number;
  grader_evidence_hash: string;
}

export interface IntelligenceV3PrivilegedGrader {
  grade(taskId: string, submissionArtifactHash: string): Promise<IntelligenceV3PrivilegedGrade>;
}

export interface IntelligenceV3FullSuiteTaskRecord {
  task_id: string;
  attempt_id: string;
  context_id: string | null;
  context_evidence_hash: string | null;
  participant_bundle_hash: string | null;
  submission_artifact_hash: string | null;
  grader_evidence_hash: string | null;
  tool_trace_hash: string | null;
  budget_evidence_hash: string | null;
  usage: IntelligenceV3Usage;
  outcome: IntelligenceV3AttemptOutcome;
  error: string | null;
}

export interface IntelligenceV3FullSuiteRun {
  schema: typeof INTELLIGENCE_V3_FULL_SUITE_SCHEMA;
  run_id: string;
  provider: string;
  model: string;
  reasoning_effort: string;
  complete: boolean;
  one_attempt_per_task: true;
  fresh_context_per_task: true;
  cross_task_memory_disabled: true;
  task_count: number;
  failed_task_count: number;
  attempt_ledger_hash: string;
  full_suite_evidence_hash: string;
  tasks: IntelligenceV3FullSuiteTaskRecord[];
}

const HASH_PATTERN = /^(?:sha256:)?[a-f0-9]{64}$/i;
const ZERO_USAGE: IntelligenceV3Usage = { tool_calls: 0, input_tokens: 0, output_tokens: 0, repository_reads: 0, test_runs: 0, patch_attempts: 0 };

function canonical(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(canonical);
  if (value && typeof value === "object") return Object.fromEntries(Object.entries(value as Record<string, unknown>).sort(([left], [right]) => left.localeCompare(right)).map(([key, item]) => [key, canonical(item)]));
  return value;
}

function hash(value: unknown): string {
  return createHash("sha256").update(JSON.stringify(canonical(value))).digest("hex");
}

function assertHash(value: string, label: string): void {
  if (!HASH_PATTERN.test(value)) throw new Error(`${label} is missing or invalid`);
}

function assertRelease(release: IntelligenceV3ParticipantRelease, expectedTaskId: string): void {
  if (release.task_id !== expectedTaskId || !release.participant_root || !release.initial_signal || !Array.isArray(release.public_commands) || release.public_commands.length === 0) throw new Error("participant release is incomplete or bound to the wrong task");
  assertHash(release.participant_bundle_hash, "participant bundle hash");
  const serialized = JSON.stringify(release).toLowerCase();
  if (/grader_root|hidden_cases|reference_solution|answer_key|grader_bundle_hash/.test(serialized)) throw new Error("participant release leaks privileged evaluator material");
}

function assertUsage(usage: IntelligenceV3Usage): void {
  for (const [name, value] of Object.entries(usage)) if (!Number.isSafeInteger(value) || value < 0) throw new Error(`usage ${name} must be a nonnegative safe integer`);
}

export class IntelligenceV3FullSuiteHarness {
  private readonly ledger: IntelligenceV3AttemptLedger;

  constructor(
    private readonly protocol: IntelligenceV3Protocol,
    private readonly subject: IntelligenceV3SubjectAdapter,
    private readonly grader: IntelligenceV3PrivilegedGrader,
    private readonly now: () => number = Date.now,
  ) {
    this.ledger = new IntelligenceV3AttemptLedger(protocol, now);
  }

  async run(input: { provider: string; model: string; reasoning_effort: string; tasks: IntelligenceV3TaskDescriptor[] }): Promise<IntelligenceV3FullSuiteRun> {
    const taskIds = input.tasks.map((task) => task.task_id);
    if (taskIds.length < this.protocol.official_requirements.minimum_distinct_tasks || new Set(taskIds).size !== taskIds.length) throw new Error("full-suite harness requires the complete distinct task set");
    const ledgerRun = this.ledger.startRun({ provider: input.provider, model: input.model, reasoning_effort: input.reasoning_effort, expected_task_ids: taskIds });
    const contexts = new Set<string>();
    const records: IntelligenceV3FullSuiteTaskRecord[] = [];
    for (const task of input.tasks) {
      // This call is deliberately first: the fixed wall clock begins before
      // either the task payload or a task-aware model context exists.
      const attempt = this.ledger.releaseTask(ledgerRun.run_id, task.task_id);
      let context: IntelligenceV3SubjectContext | null = null;
      let participantBundleHash: string | null = null;
      let submission: IntelligenceV3SubjectSubmission | null = null;
      let grade: IntelligenceV3PrivilegedGrade | null = null;
      let error: string | null = null;
      try {
        const release = await task.releaseParticipantTask();
        assertRelease(release, task.task_id);
        participantBundleHash = release.participant_bundle_hash;
        context = await this.subject.createFreshContext({ provider: input.provider, model: input.model, reasoning_effort: input.reasoning_effort, task_id: task.task_id, attempt_id: attempt.attempt_id });
        if (!context.context_id || contexts.has(context.context_id) || context.fresh_context !== true || context.cross_task_memory_disabled !== true) throw new Error("subject adapter reused or failed to isolate a task context");
        assertHash(context.context_evidence_hash, "context evidence hash");
        contexts.add(context.context_id);
        const controller = new AbortController();
        const elapsed = Math.max(0, this.now() - attempt.released_at_ms);
        const remaining = Math.max(1, this.protocol.budgets.wall_clock_ms - elapsed);
        let timer: ReturnType<typeof setTimeout> | undefined;
        try {
          submission = await Promise.race([
            this.subject.executeTask(context, release, { budgets: this.protocol.budgets, deadline_ms: attempt.released_at_ms + this.protocol.budgets.wall_clock_ms, signal: controller.signal }),
            new Promise<never>((_, reject) => { timer = setTimeout(() => { controller.abort(); reject(new Error("task wall-clock budget expired")); }, remaining); }),
          ]);
        } finally {
          if (timer) clearTimeout(timer);
        }
        assertHash(submission.submission_artifact_hash, "submission artifact hash");
        assertHash(submission.tool_trace_hash, "tool trace hash");
        assertHash(submission.budget_evidence_hash, "budget evidence hash");
        assertUsage(submission.usage);
        if (![submission.valid_actions, submission.total_actions, submission.policy_violations].every((value) => Number.isSafeInteger(value) && value >= 0) || submission.valid_actions > submission.total_actions) throw new Error("subject adapter returned invalid action accounting");
        this.ledger.recordUsage(ledgerRun.run_id, task.task_id, submission.usage);
        grade = await this.grader.grade(task.task_id, submission.submission_artifact_hash);
        assertHash(grade.grader_evidence_hash, "grader evidence hash");
        if (!Number.isSafeInteger(grade.robustness_passed) || !Number.isSafeInteger(grade.robustness_total) || grade.robustness_passed < 0 || grade.robustness_total < 0 || grade.robustness_passed > grade.robustness_total) throw new Error("privileged grader returned invalid robustness totals");
      } catch (caught) {
        error = caught instanceof Error ? caught.message : String(caught);
      } finally {
        if (context) {
          try { await this.subject.destroyContext(context); }
          catch (caught) { error ||= `context destruction failed: ${caught instanceof Error ? caught.message : String(caught)}`; }
        }
      }
      const outcome: IntelligenceV3AttemptOutcome = grade && !error
        ? { correctness_passed: grade.correctness_passed, robustness_passed: grade.robustness_passed, robustness_total: grade.robustness_total, policy_violations: submission!.policy_violations }
        : { correctness_passed: false, robustness_passed: 0, robustness_total: grade?.robustness_total || 0, policy_violations: Math.max(1, submission?.policy_violations || 0) };
      this.ledger.submitTask(ledgerRun.run_id, task.task_id, outcome);
      records.push({
        task_id: task.task_id,
        attempt_id: attempt.attempt_id,
        context_id: context?.context_id || null,
        context_evidence_hash: context?.context_evidence_hash || null,
        participant_bundle_hash: participantBundleHash,
        submission_artifact_hash: submission?.submission_artifact_hash || null,
        grader_evidence_hash: grade?.grader_evidence_hash || null,
        tool_trace_hash: submission?.tool_trace_hash || null,
        budget_evidence_hash: submission?.budget_evidence_hash || null,
        usage: submission?.usage || { ...ZERO_USAGE },
        outcome,
        error,
      });
    }
    const finalLedger = this.ledger.finalizeRun(ledgerRun.run_id);
    const base = {
      schema: INTELLIGENCE_V3_FULL_SUITE_SCHEMA,
      run_id: ledgerRun.run_id,
      provider: input.provider,
      model: input.model,
      reasoning_effort: input.reasoning_effort,
      complete: finalLedger.complete,
      one_attempt_per_task: true as const,
      fresh_context_per_task: true as const,
      cross_task_memory_disabled: true as const,
      task_count: records.length,
      failed_task_count: records.filter((record) => !record.outcome.correctness_passed).length,
      attempt_ledger_hash: finalLedger.event_trace_hash,
      tasks: records,
    };
    return { ...base, full_suite_evidence_hash: hash(base) };
  }
}
