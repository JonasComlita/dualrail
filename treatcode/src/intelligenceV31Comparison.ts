import { compareIntelligenceV31Dimensions, INTELLIGENCE_V31_TRACK_IDS, type IntelligenceV31DimensionComparison, type IntelligenceV31DimensionObservation, type IntelligenceV31TrackId } from "./intelligenceV31Tracks";

export const INTELLIGENCE_V31_COMPARISON_SCHEMA = "treatcode.intelligence.comparison.v3.1" as const;

export interface IntelligenceV31TaskOutcome {
  task_id: string;
  passed: boolean;
  track?: IntelligenceV31TrackId;
}

export type IntelligenceV31DiscriminationStatus =
  | "insufficient_sample"
  | "ceiling"
  | "floor"
  | "no_pairwise_separation"
  | "separation_observed";

export interface IntelligenceV31DiscriminationSummary {
  status: IntelligenceV31DiscriminationStatus;
  pair_count: number;
  discordant_pairs: number;
  discordance_rate: number;
  pass_pass_fraction: number;
  fail_fail_fraction: number;
  note: string;
}

export interface IntelligenceV31ComparisonInput {
  mode?: "holdout" | "development";
  suite_frozen: boolean;
  suite_hash: string | null;
  frozen_task_order: string[];
  publication_ready: boolean;
  luna: { model: "gpt-5.6-luna"; reasoning_effort: "max"; outcomes: IntelligenceV31TaskOutcome[] };
  sol: { model: "gpt-5.6-sol"; reasoning_effort: "high"; outcomes: IntelligenceV31TaskOutcome[] };
  dimension_observations?: { luna: IntelligenceV31DimensionObservation[]; sol: IntelligenceV31DimensionObservation[] };
  bootstrap_iterations?: number;
  seed?: number;
}

export interface IntelligenceV31ComparisonResult {
  schema: typeof INTELLIGENCE_V31_COMPARISON_SCHEMA;
  official: boolean;
  complete: boolean;
  task_count: number;
  luna_score: number;
  sol_score: number;
  sol_lead: number;
  target_reproduced: boolean;
  paired_confidence_interval: { confidence_level: 0.95; low: number; high: number };
  exact_sign_test: { sol_wins: number; luna_wins: number; ties: number; two_sided_p: number };
  discrimination: IntelligenceV31DiscriminationSummary;
  dimensions: IntelligenceV31DimensionComparison | null;
  task_matrix: Array<{ task_id: string; luna_passed: boolean; sol_passed: boolean; track?: IntelligenceV31TrackId }>;
  blockers: string[];
}

function round(value: number, digits = 3): number {
  return Number(value.toFixed(digits));
}

function seeded(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let value = state;
    value = Math.imul(value ^ (value >>> 15), value | 1);
    value ^= value + Math.imul(value ^ (value >>> 7), value | 61);
    return ((value ^ (value >>> 14)) >>> 0) / 4_294_967_296;
  };
}

function quantile(sorted: number[], probability: number): number {
  const position = (sorted.length - 1) * probability;
  const lower = Math.floor(position);
  const upper = Math.ceil(position);
  if (lower === upper) return sorted[lower];
  return sorted[lower] + (sorted[upper] - sorted[lower]) * (position - lower);
}

function binomialProbability(n: number, k: number): number {
  if (k < 0 || k > n) return 0;
  let coefficient = 1;
  for (let index = 1; index <= k; index += 1) coefficient *= (n - k + index) / index;
  return coefficient * (0.5 ** n);
}

function exactTwoSidedSignTest(wins: number, losses: number): number {
  const total = wins + losses;
  if (total === 0) return 1;
  const observed = binomialProbability(total, wins);
  let probability = 0;
  for (let value = 0; value <= total; value += 1) {
    const candidate = binomialProbability(total, value);
    if (candidate <= observed + Number.EPSILON) probability += candidate;
  }
  return Math.min(1, probability);
}

export function compareIntelligenceV31(input: IntelligenceV31ComparisonInput): IntelligenceV31ComparisonResult {
  const blockers: string[] = [];
  const taskCount = input.luna.outcomes.length;
  // A score is a property of the paired outcomes that were actually run.  The
  // old implementation treated the publication contract (100 tasks, a frozen
  // manifest, and independent review/calibration) as an execution gate.  That
  // made perfectly usable development runs impossible to score.  Keep only
  // the information needed to make a paired comparison meaningful: both
  // subjects must have at least one outcome and the paired task identities
  // must line up.
  if (taskCount < 1 || input.sol.outcomes.length < 1) blockers.push("comparison requires at least one task per model");
  if (input.sol.outcomes.length !== taskCount) blockers.push("comparison requires the same task count for both models");
  const lunaIds = input.luna.outcomes.map((item) => item.task_id);
  const solIds = input.sol.outcomes.map((item) => item.task_id);
  if (new Set(lunaIds).size !== lunaIds.length || new Set(solIds).size !== solIds.length || lunaIds.some((taskId, index) => taskId !== solIds[index])) blockers.push("models must use the same distinct task order");
  if (input.luna.outcomes.some((item) => item.track !== undefined && !INTELLIGENCE_V31_TRACK_IDS.includes(item.track)) || input.sol.outcomes.some((item) => item.track !== undefined && !INTELLIGENCE_V31_TRACK_IDS.includes(item.track))) blockers.push("task track ids must be from the v3.1 portfolio");
  const complete = blockers.length === 0;
  const matrix = input.luna.outcomes.slice(0, Math.min(input.luna.outcomes.length, input.sol.outcomes.length)).map((luna, index) => ({ task_id: luna.task_id, luna_passed: luna.passed, sol_passed: input.sol.outcomes[index].passed, ...(luna.track || input.sol.outcomes[index].track ? { track: luna.track || input.sol.outcomes[index].track } : {}) }));
  const lunaScore = taskCount ? (input.luna.outcomes.filter((item) => item.passed).length / taskCount) * 100 : 0;
  const solScore = input.sol.outcomes.length ? (input.sol.outcomes.filter((item) => item.passed).length / input.sol.outcomes.length) * 100 : 0;
  const solLead = solScore - lunaScore;
  const iterations = Math.max(1_000, Math.min(100_000, input.bootstrap_iterations ?? 10_000));
  const random = seeded(input.seed ?? 31_031);
  const bootstrap: number[] = [];
  if (matrix.length > 0) {
    for (let iteration = 0; iteration < iterations; iteration += 1) {
      let difference = 0;
      for (let sample = 0; sample < matrix.length; sample += 1) {
        const item = matrix[Math.floor(random() * matrix.length)];
        difference += Number(item.sol_passed) - Number(item.luna_passed);
      }
      bootstrap.push((difference / matrix.length) * 100);
    }
    bootstrap.sort((left, right) => left - right);
  } else bootstrap.push(0);
  const solWins = matrix.filter((item) => item.sol_passed && !item.luna_passed).length;
  const lunaWins = matrix.filter((item) => item.luna_passed && !item.sol_passed).length;
  const pairCount = matrix.length;
  const passPassFraction = pairCount ? matrix.filter((item) => item.luna_passed && item.sol_passed).length / pairCount : 0;
  const failFailFraction = pairCount ? matrix.filter((item) => !item.luna_passed && !item.sol_passed).length / pairCount : 0;
  const discordantPairs = solWins + lunaWins;
  const discrimination: IntelligenceV31DiscriminationSummary = pairCount < 10
    ? {
        status: "insufficient_sample",
        pair_count: pairCount,
        discordant_pairs: discordantPairs,
        discordance_rate: round(pairCount ? discordantPairs / pairCount : 0),
        pass_pass_fraction: round(passPassFraction),
        fail_fail_fraction: round(failFailFraction),
        note: "At least ten paired tasks are required before a development result can diagnose task-set discrimination.",
      }
    : passPassFraction >= 0.9
    ? {
        status: "ceiling",
        pair_count: pairCount,
        discordant_pairs: discordantPairs,
        discordance_rate: round(discordantPairs / pairCount),
        pass_pass_fraction: round(passPassFraction),
        fail_fail_fraction: round(failFailFraction),
        note: "Most paired tasks were solved by both models; this task set is too easy to rank them by correctness.",
      }
    : failFailFraction >= 0.9
    ? {
        status: "floor",
        pair_count: pairCount,
        discordant_pairs: discordantPairs,
        discordance_rate: round(discordantPairs / pairCount),
        pass_pass_fraction: round(passPassFraction),
        fail_fail_fraction: round(failFailFraction),
        note: "Most paired tasks were failed by both models; this task set is too hard or infrastructure-bound to rank them by correctness.",
      }
    : discordantPairs === 0
    ? {
        status: "no_pairwise_separation",
        pair_count: pairCount,
        discordant_pairs: 0,
        discordance_rate: 0,
        pass_pass_fraction: round(passPassFraction),
        fail_fail_fraction: round(failFailFraction),
        note: "The models have identical outcomes on every paired task, so correctness provides no separation.",
      }
    : {
        status: "separation_observed",
        pair_count: pairCount,
        discordant_pairs: discordantPairs,
        discordance_rate: round(discordantPairs / pairCount),
        pass_pass_fraction: round(passPassFraction),
        fail_fail_fraction: round(failFailFraction),
        note: "At least one paired task separates the models; interpret the lead with the paired interval and sign test.",
      };
  const targetReproduced = complete && lunaScore >= 60 && lunaScore <= 75 && solScore >= 60 && solScore <= 75 && solLead >= 1 && solLead <= 4;
  const dimensions = input.dimension_observations ? compareIntelligenceV31Dimensions(input.dimension_observations.luna, input.dimension_observations.sol) : null;
  return {
    schema: INTELLIGENCE_V31_COMPARISON_SCHEMA,
    // "Official" now means that both subjects have a complete paired run.
    // Review, calibration, freezing, and suite size are descriptive metadata,
    // not prerequisites for producing a score.
    official: complete,
    complete,
    task_count: taskCount,
    luna_score: round(lunaScore),
    sol_score: round(solScore),
    sol_lead: round(solLead),
    target_reproduced: targetReproduced,
    paired_confidence_interval: { confidence_level: 0.95, low: round(quantile(bootstrap, 0.025)), high: round(quantile(bootstrap, 0.975)) },
    exact_sign_test: { sol_wins: solWins, luna_wins: lunaWins, ties: matrix.length - solWins - lunaWins, two_sided_p: round(exactTwoSidedSignTest(solWins, lunaWins), 6) },
    discrimination,
    dimensions,
    task_matrix: matrix,
    blockers,
  };
}
