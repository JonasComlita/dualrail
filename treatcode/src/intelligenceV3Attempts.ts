import { createHash, randomUUID } from "node:crypto";
import type { IntelligenceV3Protocol } from "./intelligenceV3";

export const INTELLIGENCE_V3_ATTEMPT_LEDGER_SCHEMA = "treatcode.intelligence.attempt-ledger.v3" as const;

export interface IntelligenceV3Usage {
  tool_calls: number;
  input_tokens: number;
  output_tokens: number;
  repository_reads: number;
  test_runs: number;
  patch_attempts: number;
}

export interface IntelligenceV3AttemptOutcome {
  correctness_passed: boolean;
  robustness_passed: number;
  robustness_total: number;
  policy_violations: number;
}

export interface IntelligenceV3AttemptRecord {
  task_id: string;
  attempt_id: string;
  released_at_ms: number;
  submitted_at_ms: number | null;
  wall_clock_ms: number | null;
  usage: IntelligenceV3Usage;
  budget_exceeded: string[];
  outcome: IntelligenceV3AttemptOutcome | null;
}

export interface IntelligenceV3AttemptRun {
  schema: typeof INTELLIGENCE_V3_ATTEMPT_LEDGER_SCHEMA;
  run_id: string;
  provider: string;
  model: string;
  reasoning_effort: string;
  created_at_ms: number;
  expected_task_ids: string[];
  fresh_context_per_task: true;
  cross_task_memory_disabled: true;
  attempts: IntelligenceV3AttemptRecord[];
  complete: boolean;
  event_trace_hash: string;
}

interface InternalRun {
  run_id: string;
  provider: string;
  model: string;
  reasoning_effort: string;
  created_at_ms: number;
  expected_task_ids: string[];
  attempts: Map<string, IntelligenceV3AttemptRecord>;
  events: Array<Record<string, unknown>>;
}

const ZERO_USAGE: IntelligenceV3Usage = {
  tool_calls: 0,
  input_tokens: 0,
  output_tokens: 0,
  repository_reads: 0,
  test_runs: 0,
  patch_attempts: 0,
};

function canonical(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(canonical).join(",")}]`;
  if (value && typeof value === "object") return `{${Object.entries(value as Record<string, unknown>).sort(([left], [right]) => left.localeCompare(right)).map(([key, item]) => `${JSON.stringify(key)}:${canonical(item)}`).join(",")}}`;
  return JSON.stringify(value);
}

function hashEvents(events: Array<Record<string, unknown>>): string {
  let previous = "0".repeat(64);
  for (const event of events) previous = createHash("sha256").update(`${previous}\n${canonical(event)}`).digest("hex");
  return previous;
}

function safeCount(value: number, name: string): number {
  if (!Number.isSafeInteger(value) || value < 0) throw new Error(`${name} must be a nonnegative safe integer`);
  return value;
}

export class IntelligenceV3AttemptLedger {
  private readonly runs = new Map<string, InternalRun>();

  constructor(private readonly protocol: IntelligenceV3Protocol, private readonly now: () => number = Date.now) {}

  startRun(input: { provider: string; model: string; reasoning_effort: string; expected_task_ids: string[] }): IntelligenceV3AttemptRun {
    const expected = [...input.expected_task_ids];
    if (expected.length < this.protocol.official_requirements.minimum_distinct_tasks || new Set(expected).size !== expected.length) throw new Error("A v3 run requires the complete distinct task set");
    if (!input.provider || !input.model || !input.reasoning_effort) throw new Error("Provider, model, and reasoning effort are required");
    const createdAt = this.now();
    const run: InternalRun = {
      run_id: `intel_v3_${randomUUID().replace(/-/g, "")}`,
      provider: input.provider,
      model: input.model,
      reasoning_effort: input.reasoning_effort,
      created_at_ms: createdAt,
      expected_task_ids: expected,
      attempts: new Map(),
      events: [{ type: "run_started", at_ms: createdAt, task_count: expected.length, fresh_context_per_task: true, cross_task_memory_disabled: true }],
    };
    this.runs.set(run.run_id, run);
    return this.view(run);
  }

  releaseTask(runId: string, taskId: string): IntelligenceV3AttemptRecord {
    const run = this.requireRun(runId);
    if (!run.expected_task_ids.includes(taskId)) throw new Error("Task is outside the frozen run set");
    if (run.attempts.has(taskId)) throw new Error("Exactly one attempt is permitted per task");
    const releasedAt = this.now();
    const attempt: IntelligenceV3AttemptRecord = {
      task_id: taskId,
      attempt_id: `attempt_${randomUUID().replace(/-/g, "")}`,
      released_at_ms: releasedAt,
      submitted_at_ms: null,
      wall_clock_ms: null,
      usage: { ...ZERO_USAGE },
      budget_exceeded: [],
      outcome: null,
    };
    run.attempts.set(taskId, attempt);
    run.events.push({ type: "task_released", at_ms: releasedAt, task_id: taskId, attempt_id: attempt.attempt_id });
    return structuredClone(attempt);
  }

  recordUsage(runId: string, taskId: string, delta: Partial<IntelligenceV3Usage>): IntelligenceV3AttemptRecord {
    const run = this.requireRun(runId);
    const attempt = this.requireOpenAttempt(run, taskId);
    for (const key of Object.keys(ZERO_USAGE) as Array<keyof IntelligenceV3Usage>) attempt.usage[key] += safeCount(delta[key] ?? 0, key);
    attempt.budget_exceeded = this.exceeded(attempt);
    run.events.push({ type: "usage_recorded", at_ms: this.now(), task_id: taskId, delta: { ...delta }, totals: { ...attempt.usage }, budget_exceeded: [...attempt.budget_exceeded] });
    return structuredClone(attempt);
  }

  submitTask(runId: string, taskId: string, outcome: IntelligenceV3AttemptOutcome): IntelligenceV3AttemptRecord {
    const run = this.requireRun(runId);
    const attempt = this.requireOpenAttempt(run, taskId);
    safeCount(outcome.robustness_passed, "robustness_passed");
    safeCount(outcome.robustness_total, "robustness_total");
    safeCount(outcome.policy_violations, "policy_violations");
    if (outcome.robustness_passed > outcome.robustness_total) throw new Error("robustness passes exceed total");
    const submittedAt = this.now();
    attempt.submitted_at_ms = submittedAt;
    attempt.wall_clock_ms = Math.max(0, submittedAt - attempt.released_at_ms);
    attempt.outcome = { ...outcome };
    attempt.budget_exceeded = this.exceeded(attempt);
    run.events.push({ type: "task_submitted", at_ms: submittedAt, task_id: taskId, attempt_id: attempt.attempt_id, wall_clock_ms: attempt.wall_clock_ms, usage: { ...attempt.usage }, budget_exceeded: [...attempt.budget_exceeded], outcome: { ...outcome } });
    return structuredClone(attempt);
  }

  getRun(runId: string): IntelligenceV3AttemptRun {
    return this.view(this.requireRun(runId));
  }

  finalizeRun(runId: string): IntelligenceV3AttemptRun {
    const run = this.requireRun(runId);
    const missing = run.expected_task_ids.filter((taskId) => !run.attempts.get(taskId)?.outcome);
    if (missing.length > 0) throw new Error(`Full-suite execution is required; ${missing.length} tasks are incomplete`);
    run.events.push({ type: "run_finalized", at_ms: this.now(), task_count: run.expected_task_ids.length });
    return this.view(run);
  }

  private exceeded(attempt: IntelligenceV3AttemptRecord): string[] {
    const exceeded: string[] = [];
    const wall = Math.max(0, this.now() - attempt.released_at_ms);
    if (wall > this.protocol.budgets.wall_clock_ms) exceeded.push("wall_clock_ms");
    for (const key of Object.keys(ZERO_USAGE) as Array<keyof IntelligenceV3Usage>) if (attempt.usage[key] > this.protocol.budgets[key]) exceeded.push(key);
    return exceeded;
  }

  private requireRun(runId: string): InternalRun {
    const run = this.runs.get(runId);
    if (!run) throw new Error("Unknown v3 attempt run");
    return run;
  }

  private requireOpenAttempt(run: InternalRun, taskId: string): IntelligenceV3AttemptRecord {
    const attempt = run.attempts.get(taskId);
    if (!attempt) throw new Error("Task must be released before usage or submission");
    if (attempt.outcome) throw new Error("Task attempt is already sealed");
    return attempt;
  }

  private view(run: InternalRun): IntelligenceV3AttemptRun {
    const attempts = run.expected_task_ids.flatMap((taskId) => run.attempts.has(taskId) ? [structuredClone(run.attempts.get(taskId)!)] : []);
    return {
      schema: INTELLIGENCE_V3_ATTEMPT_LEDGER_SCHEMA,
      run_id: run.run_id,
      provider: run.provider,
      model: run.model,
      reasoning_effort: run.reasoning_effort,
      created_at_ms: run.created_at_ms,
      expected_task_ids: [...run.expected_task_ids],
      fresh_context_per_task: true,
      cross_task_memory_disabled: true,
      attempts,
      complete: attempts.length === run.expected_task_ids.length && attempts.every((attempt) => Boolean(attempt.outcome)),
      event_trace_hash: hashEvents(run.events),
    };
  }
}
