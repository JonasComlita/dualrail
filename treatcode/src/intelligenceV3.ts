export const INTELLIGENCE_V3_PROTOCOL_SCHEMA = "treatcode.intelligence.protocol.v3" as const;
export const INTELLIGENCE_V3_RESULT_SCHEMA = "treatcode.intelligence.run-result.v3" as const;

export interface IntelligenceV3Protocol {
  schema: typeof INTELLIGENCE_V3_PROTOCOL_SCHEMA;
  version: 3;
  status: "development" | "frozen";
  score_scope: "independent_repository_task_pass_rate";
  headline_metric: "correctness_task_pass_rate";
  official_requirements: {
    minimum_distinct_tasks: number;
    attempts_per_task: 1;
    minimum_independent_authors: number;
    minimum_reviewers_per_task: number;
    require_subject_model_independence: boolean;
    require_hidden_grader_isolation: boolean;
    require_identical_task_set_for_pairwise_comparison: boolean;
    require_complete_suite: boolean;
    require_human_calibrated_discussion_rubric: boolean;
  };
  task_mix: Record<string, number>;
  task_design_coverage: Record<string, number>;
  budgets: {
    wall_clock_ms: number;
    tool_calls: number;
    input_tokens: number;
    output_tokens: number;
    repository_reads: number;
    test_runs: number;
    patch_attempts: number;
  };
  dimensions: {
    correctness: { weight: number };
    robustness: { weight: number };
    efficiency: { weight: number; requires_correctness: boolean };
    agent_execution: { weight: number };
    discussion: { weight: 0; length_controlled: boolean; maximum_words: number };
  };
  statistics: {
    confidence_level: number;
    bootstrap_iterations: number;
    ceiling_threshold: number;
    floor_threshold: number;
    maximum_ceiling_fraction: number;
    maximum_floor_fraction: number;
    minimum_item_discrimination: number;
  };
  calibration: {
    required_cohorts: string[];
    minimum_models_per_cohort: number;
    minimum_runs_per_configuration: number;
    status: "not_started" | "in_progress" | "complete";
  };
}

export interface IntelligenceV3TaskResult {
  task_id: string;
  category: string;
  attempt_id: string;
  attempt_count: number;
  attempt: {
    fresh_context: boolean;
    clock_started_before_task_release: boolean;
    cross_task_memory_disabled: boolean;
    budget_evidence_hash: string;
    tool_trace_hash: string;
  };
  provenance: {
    author_id: string;
    reviewer_ids: string[];
    subject_model_independent: boolean;
    provenance_bundle_hash: string;
  };
  isolation: {
    participant_root: string;
    grader_root: string;
    participant_bundle_hash: string;
    grader_bundle_hash: string;
    hidden_grader_isolated: boolean;
    isolation_audit_hash: string;
  };
  holdout: {
    frozen: boolean;
    manifest_hash: string;
  };
  calibration: {
    accepted: boolean;
    cohort_pass_rates: Record<string, number>;
    item_discrimination: number;
    human_reviewed: boolean;
  };
  correctness: {
    passed: boolean;
    required_suites_passed: number;
    required_suites_total: number;
  };
  robustness: {
    passed: number;
    total: number;
  };
  efficiency: {
    wall_clock_ms: number;
    tool_calls: number;
    input_tokens: number;
    output_tokens: number;
    repository_reads: number;
    test_runs: number;
    patch_attempts: number;
  };
  agent_execution: {
    valid_actions: number;
    total_actions: number;
    policy_violations: number;
    completed: boolean;
  };
  discussion?: {
    word_count: number;
    human_score: number | null;
    maximum_score: number | null;
    rubric_version: string | null;
    rater_count: number;
    blind_review: boolean;
    length_controlled: boolean;
    calibration_evidence_hash: string;
  };
}

export interface IntelligenceV3TaskScore {
  task_id: string;
  category: string;
  correctness: number;
  robustness: number;
  efficiency: number;
  agent_execution: number;
  executable_score: number;
  discussion_score: number | null;
}

export interface IntelligenceV3RunScore {
  schema: typeof INTELLIGENCE_V3_RESULT_SCHEMA;
  task_count: number;
  distinct_task_count: number;
  official: boolean;
  official_blockers: string[];
  dimensions: {
    correctness: number;
    robustness: number;
    efficiency: number;
    agent_execution: number;
    executable_score: number;
  };
  headline_score: number;
  confidence_interval: {
    confidence_level: number;
    low: number;
    high: number;
    iterations: number;
    unit: "task";
  };
  discussion: {
    included_in_executable_score: false;
    scored_tasks: number;
    mean_percent: number | null;
  };
  task_scores: IntelligenceV3TaskScore[];
}

function finiteNonnegative(value: number): boolean {
  return Number.isFinite(value) && value >= 0;
}

function clamp01(value: number): number {
  return Math.max(0, Math.min(1, value));
}

function mean(values: number[]): number {
  return values.length === 0 ? 0 : values.reduce((sum, value) => sum + value, 0) / values.length;
}

function roundScore(value: number): number {
  return Math.round(value * 100) / 100;
}

function isHash(value: string): boolean {
  return /^(?:sha256:)?[a-f0-9]{64}$/i.test(value);
}

function normalizedPath(value: string): string {
  return value.replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase();
}

function rootsOverlap(left: string, right: string): boolean {
  const a = normalizedPath(left);
  const b = normalizedPath(right);
  return a === b || a.startsWith(`${b}/`) || b.startsWith(`${a}/`);
}

export function validateIntelligenceV3Protocol(protocol: IntelligenceV3Protocol): string[] {
  const issues: string[] = [];
  if (protocol.schema !== INTELLIGENCE_V3_PROTOCOL_SCHEMA || protocol.version !== 3) issues.push("unsupported protocol schema or version");
  if (protocol.score_scope !== "independent_repository_task_pass_rate") issues.push("score scope must describe independent repository tasks");
  if (protocol.headline_metric !== "correctness_task_pass_rate") issues.push("headline metric must remain the independent correctness task pass rate");
  if (protocol.official_requirements.minimum_distinct_tasks < 100) issues.push("official suite must require at least 100 distinct tasks");
  if (protocol.official_requirements.attempts_per_task !== 1) issues.push("exactly one attempt per task is required");
  if (protocol.official_requirements.minimum_reviewers_per_task < 2) issues.push("at least two independent reviewers per task are required");
  const taskMixCount = Object.values(protocol.task_mix).reduce((sum, count) => sum + count, 0);
  if (taskMixCount < protocol.official_requirements.minimum_distinct_tasks) issues.push("task mix does not cover the official minimum task count");
  for (const [feature, count] of Object.entries(protocol.task_design_coverage)) if (!Number.isSafeInteger(count) || count < 0 || count > taskMixCount) issues.push(`task design coverage ${feature} is invalid`);
  const executableWeight = protocol.dimensions.correctness.weight + protocol.dimensions.robustness.weight + protocol.dimensions.efficiency.weight + protocol.dimensions.agent_execution.weight;
  if (Math.abs(executableWeight - 1) > 1e-9) issues.push("executable dimension weights must sum to one");
  if (protocol.dimensions.discussion.weight !== 0) issues.push("discussion must remain outside the executable score");
  if (!protocol.dimensions.discussion.length_controlled || protocol.dimensions.discussion.maximum_words <= 0) issues.push("discussion grading must be length controlled");
  for (const [name, value] of Object.entries(protocol.budgets)) if (!finiteNonnegative(value) || value === 0) issues.push(`budget ${name} must be positive`);
  if (protocol.statistics.bootstrap_iterations < 1000) issues.push("bootstrap requires at least 1000 iterations");
  if (protocol.statistics.confidence_level <= 0 || protocol.statistics.confidence_level >= 1) issues.push("confidence level must be between zero and one");
  if (protocol.statistics.floor_threshold < 0 || protocol.statistics.floor_threshold >= protocol.statistics.ceiling_threshold || protocol.statistics.ceiling_threshold > 1) issues.push("calibration floor and ceiling thresholds are invalid");
  if (protocol.statistics.maximum_ceiling_fraction < 0 || protocol.statistics.maximum_ceiling_fraction > 1 || protocol.statistics.maximum_floor_fraction < 0 || protocol.statistics.maximum_floor_fraction > 1) issues.push("calibration ceiling/floor fractions are invalid");
  if (protocol.statistics.minimum_item_discrimination <= 0 || protocol.statistics.minimum_item_discrimination > 1) issues.push("minimum item discrimination must be in (0, 1]");
  if (protocol.calibration.required_cohorts.join(",") !== "weak,medium,frontier") issues.push("calibration cohorts must be ordered weak, medium, frontier");
  if (protocol.calibration.minimum_models_per_cohort < 2) issues.push("calibration requires at least two models per cohort");
  if (protocol.calibration.minimum_runs_per_configuration < 3) issues.push("calibration requires at least three runs per configuration");
  return issues;
}

function budgetRatios(protocol: IntelligenceV3Protocol, result: IntelligenceV3TaskResult): number[] {
  const measured = result.efficiency;
  const budgets = protocol.budgets;
  return [
    budgets.wall_clock_ms / Math.max(1, measured.wall_clock_ms),
    budgets.tool_calls / Math.max(1, measured.tool_calls),
    budgets.input_tokens / Math.max(1, measured.input_tokens),
    budgets.output_tokens / Math.max(1, measured.output_tokens),
    budgets.repository_reads / Math.max(1, measured.repository_reads),
    budgets.test_runs / Math.max(1, measured.test_runs),
    budgets.patch_attempts / Math.max(1, measured.patch_attempts),
  ].map(clamp01);
}

function exceededBudget(protocol: IntelligenceV3Protocol, result: IntelligenceV3TaskResult): boolean {
  return (Object.keys(protocol.budgets) as Array<keyof IntelligenceV3Protocol["budgets"]>).some(
    (key) => result.efficiency[key] > protocol.budgets[key],
  );
}

export function scoreIntelligenceV3Task(protocol: IntelligenceV3Protocol, result: IntelligenceV3TaskResult): IntelligenceV3TaskScore {
  const correctness = result.correctness.passed && result.correctness.required_suites_total > 0 && result.correctness.required_suites_passed === result.correctness.required_suites_total ? 1 : 0;
  const robustness = result.robustness.total > 0 ? clamp01(result.robustness.passed / result.robustness.total) : 0;
  const efficiency = protocol.dimensions.efficiency.requires_correctness && correctness === 0 ? 0 : mean(budgetRatios(protocol, result));
  const actionValidity = result.agent_execution.total_actions === 0 ? Number(result.agent_execution.completed) : clamp01(result.agent_execution.valid_actions / result.agent_execution.total_actions);
  const agentExecution = result.agent_execution.completed && result.agent_execution.policy_violations === 0 && !exceededBudget(protocol, result) ? actionValidity : 0;
  const executableScore =
    correctness * protocol.dimensions.correctness.weight +
    robustness * protocol.dimensions.robustness.weight +
    efficiency * protocol.dimensions.efficiency.weight +
    agentExecution * protocol.dimensions.agent_execution.weight;
  const discussion = result.discussion;
  const discussionScore = discussion && discussion.human_score !== null && discussion.maximum_score !== null && discussion.maximum_score > 0
    ? clamp01(discussion.human_score / discussion.maximum_score) * 100
    : null;
  return {
    task_id: result.task_id,
    category: result.category,
    correctness: roundScore(correctness * 100),
    robustness: roundScore(robustness * 100),
    efficiency: roundScore(efficiency * 100),
    agent_execution: roundScore(agentExecution * 100),
    executable_score: roundScore(executableScore * 100),
    discussion_score: discussionScore === null ? null : roundScore(discussionScore),
  };
}

function xorshift32(seed: number): () => number {
  let state = seed >>> 0 || 0x9e3779b9;
  return () => {
    state ^= state << 13;
    state ^= state >>> 17;
    state ^= state << 5;
    return (state >>> 0) / 0x100000000;
  };
}

function percentile(sorted: number[], quantile: number): number {
  if (sorted.length === 0) return 0;
  const index = (sorted.length - 1) * clamp01(quantile);
  const lower = Math.floor(index);
  const upper = Math.ceil(index);
  if (lower === upper) return sorted[lower];
  return sorted[lower] + (sorted[upper] - sorted[lower]) * (index - lower);
}

export function bootstrapMeanConfidenceInterval(values: number[], confidenceLevel: number, iterations: number, seed = 0x54524954): { low: number; high: number } {
  if (values.length === 0) return { low: 0, high: 0 };
  const random = xorshift32(seed);
  const samples: number[] = [];
  for (let iteration = 0; iteration < iterations; iteration += 1) {
    let sum = 0;
    for (let index = 0; index < values.length; index += 1) sum += values[Math.floor(random() * values.length)];
    samples.push(sum / values.length);
  }
  samples.sort((a, b) => a - b);
  const alpha = 1 - confidenceLevel;
  return { low: percentile(samples, alpha / 2), high: percentile(samples, 1 - alpha / 2) };
}

function taskOfficialBlockers(protocol: IntelligenceV3Protocol, result: IntelligenceV3TaskResult): string[] {
  const prefix = result.task_id || "<missing-task-id>";
  const blockers: string[] = [];
  if (!result.task_id) blockers.push("task id is missing");
  if (result.attempt_count !== 1) blockers.push(`${prefix}: attempt count must be one`);
  if (!result.attempt.fresh_context || !result.attempt.clock_started_before_task_release || !result.attempt.cross_task_memory_disabled) blockers.push(`${prefix}: fresh-context clock and memory isolation evidence is incomplete`);
  if (!isHash(result.attempt.budget_evidence_hash) || !isHash(result.attempt.tool_trace_hash)) blockers.push(`${prefix}: budget or tool-trace evidence hash is missing`);
  if (!result.provenance.author_id || !result.provenance.subject_model_independent || !isHash(result.provenance.provenance_bundle_hash)) blockers.push(`${prefix}: signed independent author provenance is missing`);
  if (new Set(result.provenance.reviewer_ids).size < protocol.official_requirements.minimum_reviewers_per_task) blockers.push(`${prefix}: insufficient independent reviewers`);
  if (!result.isolation.hidden_grader_isolated || rootsOverlap(result.isolation.participant_root, result.isolation.grader_root)) blockers.push(`${prefix}: hidden grader is not isolated from participant workspace`);
  for (const hash of [result.isolation.participant_bundle_hash, result.isolation.grader_bundle_hash, result.isolation.isolation_audit_hash, result.holdout.manifest_hash]) if (!isHash(hash)) blockers.push(`${prefix}: missing or invalid evidence hash`);
  if (!result.holdout.frozen) blockers.push(`${prefix}: holdout is not frozen`);
  if (!result.calibration.accepted || !result.calibration.human_reviewed) blockers.push(`${prefix}: calibration and human review are incomplete`);
  for (const cohort of protocol.calibration.required_cohorts) {
    const rate = result.calibration.cohort_pass_rates[cohort];
    if (!finiteNonnegative(rate) || rate > 1) blockers.push(`${prefix}: calibration cohort ${cohort} is missing or invalid`);
  }
  if (!Number.isFinite(result.calibration.item_discrimination) || result.calibration.item_discrimination < protocol.statistics.minimum_item_discrimination) blockers.push(`${prefix}: empirical item discrimination is too low`);
  if (exceededBudget(protocol, result)) blockers.push(`${prefix}: fixed task budget was exceeded`);
  if (protocol.official_requirements.require_human_calibrated_discussion_rubric) {
    if (!result.discussion) blockers.push(`${prefix}: secondary discussion rating is missing`);
    else {
      if (result.discussion.word_count > protocol.dimensions.discussion.maximum_words || !result.discussion.length_controlled) blockers.push(`${prefix}: discussion violates the length-controlled rubric`);
      if (result.discussion.human_score === null || result.discussion.maximum_score === null || result.discussion.maximum_score <= 0 || result.discussion.rater_count < 2 || !result.discussion.blind_review || !isHash(result.discussion.calibration_evidence_hash) || result.discussion.rubric_version !== "treatcode-discussion-v3") blockers.push(`${prefix}: blinded human-calibrated discussion evidence is incomplete`);
    }
  }
  return blockers;
}

export function scoreIntelligenceV3Run(protocol: IntelligenceV3Protocol, results: IntelligenceV3TaskResult[], expectedTaskIds: string[], seed = 0x54524954): IntelligenceV3RunScore {
  const protocolIssues = validateIntelligenceV3Protocol(protocol);
  const taskScores = results.map((result) => scoreIntelligenceV3Task(protocol, result));
  const ids = results.map((result) => result.task_id);
  const distinctIds = new Set(ids);
  const expected = new Set(expectedTaskIds);
  const blockers = [...protocolIssues];
  if (distinctIds.size !== results.length) blockers.push("duplicate task attempts are not independent observations");
  if (distinctIds.size < protocol.official_requirements.minimum_distinct_tasks) blockers.push(`at least ${protocol.official_requirements.minimum_distinct_tasks} distinct tasks are required`);
  if (protocol.official_requirements.require_complete_suite && (expected.size !== distinctIds.size || [...expected].some((id) => !distinctIds.has(id)))) blockers.push("the complete frozen task set was not attempted");
  for (const [category, requiredCount] of Object.entries(protocol.task_mix)) {
    const observed = results.filter((result) => result.category === category).length;
    if (observed !== requiredCount) blockers.push(`category ${category} requires ${requiredCount} tasks but received ${observed}`);
  }
  const authorIds = new Set(results.map((result) => result.provenance.author_id).filter(Boolean));
  if (authorIds.size < protocol.official_requirements.minimum_independent_authors) blockers.push(`at least ${protocol.official_requirements.minimum_independent_authors} independent authors are required`);
  if (protocol.status !== "frozen") blockers.push("protocol is not frozen");
  if (protocol.calibration.status !== "complete") blockers.push("suite calibration is incomplete");
  for (const result of results) blockers.push(...taskOfficialBlockers(protocol, result));
  const calibrationRates = results.map((result) => Object.values(result.calibration.cohort_pass_rates));
  const ceilingFraction = calibrationRates.filter((rates) => rates.length > 0 && Math.min(...rates) >= protocol.statistics.ceiling_threshold).length / Math.max(1, results.length);
  const floorFraction = calibrationRates.filter((rates) => rates.length > 0 && Math.max(...rates) <= protocol.statistics.floor_threshold).length / Math.max(1, results.length);
  if (ceilingFraction > protocol.statistics.maximum_ceiling_fraction) blockers.push("calibration ceiling fraction exceeds the protocol limit");
  if (floorFraction > protocol.statistics.maximum_floor_fraction) blockers.push("calibration floor fraction exceeds the protocol limit");
  const executableValues = taskScores.map((task) => task.executable_score);
  const correctnessValues = taskScores.map((task) => task.correctness);
  const interval = bootstrapMeanConfidenceInterval(correctnessValues, protocol.statistics.confidence_level, protocol.statistics.bootstrap_iterations, seed);
  const discussionValues = taskScores.flatMap((task) => task.discussion_score === null ? [] : [task.discussion_score]);
  return {
    schema: INTELLIGENCE_V3_RESULT_SCHEMA,
    task_count: results.length,
    distinct_task_count: distinctIds.size,
    official: blockers.length === 0,
    official_blockers: [...new Set(blockers)],
    headline_score: roundScore(mean(correctnessValues)),
    dimensions: {
      correctness: roundScore(mean(taskScores.map((task) => task.correctness))),
      robustness: roundScore(mean(taskScores.map((task) => task.robustness))),
      efficiency: roundScore(mean(taskScores.map((task) => task.efficiency))),
      agent_execution: roundScore(mean(taskScores.map((task) => task.agent_execution))),
      executable_score: roundScore(mean(executableValues)),
    },
    confidence_interval: {
      confidence_level: protocol.statistics.confidence_level,
      low: roundScore(interval.low),
      high: roundScore(interval.high),
      iterations: protocol.statistics.bootstrap_iterations,
      unit: "task",
    },
    discussion: {
      included_in_executable_score: false,
      scored_tasks: discussionValues.length,
      mean_percent: discussionValues.length === 0 ? null : roundScore(mean(discussionValues)),
    },
    task_scores: taskScores,
  };
}

function binomialCoefficient(n: number, k: number): number {
  let result = 1;
  for (let index = 1; index <= k; index += 1) result = (result * (n - index + 1)) / index;
  return result;
}

function exactTwoSidedSignPValue(wins: number, losses: number): number {
  const count = wins + losses;
  if (count === 0) return 1;
  const tail = Math.min(wins, losses);
  let cumulative = 0;
  for (let index = 0; index <= tail; index += 1) cumulative += binomialCoefficient(count, index) * 0.5 ** count;
  return Math.min(1, cumulative * 2);
}

export function compareIntelligenceV3Runs(left: IntelligenceV3RunScore, right: IntelligenceV3RunScore, iterations = 10000, confidenceLevel = 0.95, seed = 0x50414952) {
  const leftByTask = new Map(left.task_scores.map((task) => [task.task_id, task]));
  const rightByTask = new Map(right.task_scores.map((task) => [task.task_id, task]));
  const leftIds = [...leftByTask.keys()].sort();
  const rightIds = [...rightByTask.keys()].sort();
  if (leftIds.length !== rightIds.length || leftIds.some((id, index) => id !== rightIds[index])) throw new Error("Paired comparison requires identical task sets");
  const differences = leftIds.map((id) => leftByTask.get(id)!.correctness - rightByTask.get(id)!.correctness);
  const wins = differences.filter((value) => value > 0).length;
  const losses = differences.filter((value) => value < 0).length;
  const ties = differences.length - wins - losses;
  const interval = bootstrapMeanConfidenceInterval(differences, confidenceLevel, iterations, seed);
  return {
    schema: "treatcode.intelligence.paired-comparison.v3" as const,
    task_count: differences.length,
    metric: "correctness_task_pass_rate" as const,
    mean_delta: roundScore(mean(differences)),
    confidence_interval: { confidence_level: confidenceLevel, low: roundScore(interval.low), high: roundScore(interval.high), iterations },
    sign_test: { wins, losses, ties, two_sided_p_value: Math.round(exactTwoSidedSignPValue(wins, losses) * 1_000_000) / 1_000_000 },
    statistically_resolved: interval.low > 0 || interval.high < 0,
  };
}
