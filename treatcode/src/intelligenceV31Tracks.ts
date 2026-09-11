/**
 * The v3.1 benchmark is a portfolio, not an IQ-like scalar.  Tracks borrow
 * task-shape ideas from established evaluations while keeping the scored
 * signals observable and independent.  This module contains the small amount
 * of pure validation/scoring logic shared by the API, development runner, and
 * UI.
 */

export const INTELLIGENCE_V31_TRACKS_SCHEMA = "treatcode.intelligence.tracks.v3.1" as const;
export const INTELLIGENCE_V31_SCORE_OBSERVATION_SCHEMA = "treatcode.intelligence.score-observation.v3.1" as const;

export const INTELLIGENCE_V31_TRACK_IDS = [
  "fresh_coding",
  "repository_repair",
  "terminal_agent",
  "expert_reasoning",
  "frontier_math",
  "abstract_generalization",
  "multimodal_reasoning",
  "web_research",
] as const;

export type IntelligenceV31TrackId = typeof INTELLIGENCE_V31_TRACK_IDS[number];

/** The preregistered 100-task portfolio mix; frozen suites must match it exactly. */
export const INTELLIGENCE_V31_FINAL_TRACK_MIX: Readonly<Record<IntelligenceV31TrackId, number>> = Object.freeze({
  fresh_coding: 15,
  repository_repair: 25,
  terminal_agent: 15,
  expert_reasoning: 15,
  frontier_math: 10,
  abstract_generalization: 10,
  multimodal_reasoning: 5,
  web_research: 5,
});

export type IntelligenceV31ScoreDimension =
  | "correctness"
  | "robustness"
  | "latency"
  | "resource_use"
  | "tool_execution"
  | "discussion_quality";

export interface IntelligenceV31TrackDefinition {
  id: IntelligenceV31TrackId;
  label: string;
  inspiration: string[];
  execution_mode: "scalar" | "repository" | "terminal";
  primary_signal: "correctness" | "generalization" | "agent_execution";
  task_shape: string;
  freshness: "release_after_cutoff" | "unseen_repository_bundle" | "sealed_generated_instance";
  required_dimensions: IntelligenceV31ScoreDimension[];
}

export interface IntelligenceV31ScoreDimensionDefinition {
  id: IntelligenceV31ScoreDimension;
  label: string;
  unit: "percent";
  included_in_headline: boolean;
  included_in_executable_composite: false;
  missing_data_policy: "not_scored" | "fail_closed";
}

export interface IntelligenceV31TracksManifest {
  schema: typeof INTELLIGENCE_V31_TRACKS_SCHEMA;
  version: "3.1";
  status: "development" | "frozen";
  official: false;
  inspirations: string[];
  tracks: IntelligenceV31TrackDefinition[];
  score_dimensions: {
    headline: "correctness";
    no_iq_composite: true;
    discussion_in_executable_score: false;
    dimensions: IntelligenceV31ScoreDimensionDefinition[];
  };
}

export interface IntelligenceV31DimensionObservation {
  task_id: string;
  track: IntelligenceV31TrackId;
  correctness: boolean;
  /** Fraction of hidden/adversarial cases passed, in [0, 1]. */
  robustness?: number | null;
  wall_clock_ms?: number | null;
  latency_budget_ms?: number | null;
  cpu_ms?: number | null;
  cpu_budget_ms?: number | null;
  memory_mb?: number | null;
  memory_budget_mb?: number | null;
  tool_calls?: number | null;
  successful_tool_calls?: number | null;
  tool_errors?: number | null;
  /** Human/blind-rater score already normalized to [0, 100]. */
  discussion_quality?: number | null;
  telemetry_source?: "provider_attested" | "participant_reported" | "grader_measured";
}

export interface IntelligenceV31DimensionScores {
  observed_tasks: number;
  correctness: number;
  robustness: number | null;
  latency: number | null;
  resource_use: number | null;
  tool_execution: number | null;
  discussion_quality: number | null;
  notes: string[];
}

export interface IntelligenceV31DimensionComparison {
  luna_max: IntelligenceV31DimensionScores;
  sol_high: IntelligenceV31DimensionScores;
  by_track: {
    luna_max: Partial<Record<IntelligenceV31TrackId, IntelligenceV31DimensionScores>>;
    sol_high: Partial<Record<IntelligenceV31TrackId, IntelligenceV31DimensionScores>>;
  };
  deltas: {
    correctness: number;
    robustness: number | null;
    latency: number | null;
    resource_use: number | null;
    tool_execution: number | null;
    discussion_quality: number | null;
  };
  notes: string[];
}

const REQUIRED_TRACKS = new Set<IntelligenceV31TrackId>(INTELLIGENCE_V31_TRACK_IDS);
const SCORE_DIMENSIONS = new Set<IntelligenceV31ScoreDimension>([
  "correctness",
  "robustness",
  "latency",
  "resource_use",
  "tool_execution",
  "discussion_quality",
]);

function isFiniteNumber(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value);
}

function clamp(value: number, minimum = 0, maximum = 100): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function round(value: number): number {
  return Math.round(value * 100) / 100;
}

function mean(values: number[]): number | null {
  return values.length ? round(values.reduce((sum, value) => sum + value, 0) / values.length) : null;
}

function ratioScore(actual: number | null | undefined, budget: number | null | undefined): number | null {
  if (!isFiniteNumber(actual) || !isFiniteNumber(budget) || actual < 0 || budget <= 0) return null;
  return round(clamp((budget / Math.max(1, actual)) * 100));
}

function normalizedRobustness(value: number | null | undefined): number | null {
  if (!isFiniteNumber(value)) return null;
  return round(clamp(value * 100));
}

function normalizedDiscussion(value: number | null | undefined): number | null {
  if (!isFiniteNumber(value)) return null;
  return round(clamp(value));
}

/** Validate a track manifest without mutating it or reading any files. */
export function validateIntelligenceV31TracksManifest(manifest: unknown): string[] {
  const issues: string[] = [];
  if (!manifest || typeof manifest !== "object") return ["tracks manifest must be an object"];
  const value = manifest as Partial<IntelligenceV31TracksManifest>;
  if (value.schema !== INTELLIGENCE_V31_TRACKS_SCHEMA || value.version !== "3.1") issues.push("unsupported tracks manifest schema or version");
  if (value.status !== "development" && value.status !== "frozen") issues.push("tracks manifest status is invalid");
  if (value.official !== false) issues.push("v3.1 track manifest must remain non-official until the suite is frozen");
  if (!Array.isArray(value.inspirations) || value.inspirations.length < 4 || value.inspirations.some((item) => typeof item !== "string" || !item.trim())) issues.push("benchmark inspirations are incomplete");
  if (!Array.isArray(value.tracks) || value.tracks.length < 6) issues.push("at least six complementary tracks are required");
  else {
    const ids = value.tracks.map((track) => track?.id);
    if (new Set(ids).size !== ids.length) issues.push("track ids must be distinct");
    for (const required of REQUIRED_TRACKS) if (!ids.includes(required)) issues.push(`required track is missing: ${required}`);
    for (const track of value.tracks) {
      if (!track || typeof track !== "object") {
        issues.push("track definition is invalid");
        continue;
      }
      if (!REQUIRED_TRACKS.has(track.id as IntelligenceV31TrackId)) issues.push(`unknown track id: ${String(track.id)}`);
      if (typeof track.label !== "string" || !track.label.trim()) issues.push(`track ${String(track.id)} has no label`);
      if (!Array.isArray(track.inspiration) || track.inspiration.length === 0) issues.push(`track ${String(track.id)} has no inspiration`);
      if (!["scalar", "repository", "terminal"].includes(track.execution_mode || "")) issues.push(`track ${String(track.id)} has an invalid execution mode`);
      if (!["correctness", "generalization", "agent_execution"].includes(track.primary_signal || "")) issues.push(`track ${String(track.id)} has an invalid primary signal`);
      if (!["release_after_cutoff", "unseen_repository_bundle", "sealed_generated_instance"].includes(track.freshness || "")) issues.push(`track ${String(track.id)} has an invalid freshness policy`);
      if (!Array.isArray(track.required_dimensions) || track.required_dimensions.length === 0 || track.required_dimensions.some((dimension) => !SCORE_DIMENSIONS.has(dimension))) issues.push(`track ${String(track.id)} has invalid score dimensions`);
    }
  }
  const dimensions = value.score_dimensions;
  if (!dimensions || dimensions.headline !== "correctness" || dimensions.no_iq_composite !== true || dimensions.discussion_in_executable_score !== false || !Array.isArray(dimensions.dimensions)) issues.push("score dimension policy must keep correctness as the headline and prohibit an IQ composite");
  else {
    const ids = dimensions.dimensions.map((dimension) => dimension?.id);
    if (new Set(ids).size !== ids.length || ids.length !== SCORE_DIMENSIONS.size) issues.push("score dimension definitions must be complete and distinct");
    for (const required of SCORE_DIMENSIONS) if (!ids.includes(required)) issues.push(`score dimension is missing: ${required}`);
    for (const dimension of dimensions.dimensions) {
      if (!dimension || !SCORE_DIMENSIONS.has(dimension.id)) continue;
      if (dimension.unit !== "percent" || dimension.included_in_executable_composite !== false || !["not_scored", "fail_closed"].includes(dimension.missing_data_policy)) issues.push(`score dimension ${dimension.id} has an unsafe policy`);
    }
  }
  return issues;
}

/**
 * Score independent dimensions. Missing telemetry is represented as null and
 * called out in notes; it is never silently converted into a passing score.
 */
export function scoreIntelligenceV31Dimensions(observations: IntelligenceV31DimensionObservation[]): IntelligenceV31DimensionScores {
  const correctness = observations.length ? round((observations.filter((item) => item.correctness).length / observations.length) * 100) : 0;
  const robustness = mean(observations.map((item) => normalizedRobustness(item.robustness)).filter((value): value is number => value !== null));
  const latency = mean(observations.map((item) => ratioScore(item.wall_clock_ms, item.latency_budget_ms)).filter((value): value is number => value !== null));
  const resourceValues = observations.flatMap((item) => {
    const cpu = ratioScore(item.cpu_ms, item.cpu_budget_ms);
    const memory = ratioScore(item.memory_mb, item.memory_budget_mb);
    return cpu !== null && memory !== null ? [(cpu + memory) / 2] : cpu !== null ? [cpu] : memory !== null ? [memory] : [];
  });
  const resourceUse = mean(resourceValues);
  const toolValues = observations.map((item) => {
    const calls = isFiniteNumber(item.tool_calls) && item.tool_calls >= 0 ? item.tool_calls : null;
    const successes = isFiniteNumber(item.successful_tool_calls) && item.successful_tool_calls >= 0 ? item.successful_tool_calls : null;
    const errors = isFiniteNumber(item.tool_errors) && item.tool_errors >= 0 ? item.tool_errors : null;
    // A trace or raw call count cannot establish whether commands succeeded.
    // Only use a count when at least one success/error field is present.
    if (successes === null && errors === null) return null;
    const inferredSuccesses = successes ?? Math.max(0, (calls ?? 0) - (errors ?? 0));
    const inferredErrors = errors ?? Math.max(0, (calls ?? 0) - (successes ?? 0));
    const denominator = inferredSuccesses + inferredErrors;
    return denominator > 0 ? clamp((inferredSuccesses / denominator) * 100) : calls === 0 ? 100 : null;
  }).filter((value): value is number => value !== null);
  const toolExecution = mean(toolValues);
  const discussionQuality = mean(observations.map((item) => normalizedDiscussion(item.discussion_quality)).filter((value): value is number => value !== null));
  const notes: string[] = [];
  if (observations.some((item) => item.telemetry_source === "participant_reported")) notes.push("some efficiency telemetry is participant-reported and is not official without provider attestation");
  if (robustness === null) notes.push("robustness telemetry is unavailable for this run");
  if (latency === null) notes.push("latency telemetry is unavailable for this run");
  if (resourceUse === null) notes.push("CPU/memory resource telemetry is unavailable for this run");
  if (toolExecution === null) notes.push("tool success/error counts are unavailable; the trace hash alone is not a tool-execution score");
  if (discussionQuality === null) notes.push("discussion quality is secondary and requires independent human calibration");
  return { observed_tasks: observations.length, correctness, robustness, latency, resource_use: resourceUse, tool_execution: toolExecution, discussion_quality: discussionQuality, notes };
}

function delta(left: number | null, right: number | null): number | null {
  return left === null || right === null ? null : round(right - left);
}

export function compareIntelligenceV31Dimensions(lunaObservations: IntelligenceV31DimensionObservation[], solObservations: IntelligenceV31DimensionObservation[]): IntelligenceV31DimensionComparison {
  const luna_max = scoreIntelligenceV31Dimensions(lunaObservations);
  const sol_high = scoreIntelligenceV31Dimensions(solObservations);
  const observedTracks = [...new Set([...lunaObservations, ...solObservations].map((item) => item.track))];
  const byTrack = (observations: IntelligenceV31DimensionObservation[]) => Object.fromEntries(observedTracks
    .filter((track) => observations.some((item) => item.track === track))
    .map((track) => [track, scoreIntelligenceV31Dimensions(observations.filter((item) => item.track === track))])) as Partial<Record<IntelligenceV31TrackId, IntelligenceV31DimensionScores>>;
  const notes = [...new Set([...luna_max.notes, ...sol_high.notes])];
  return {
    luna_max,
    sol_high,
    by_track: { luna_max: byTrack(lunaObservations), sol_high: byTrack(solObservations) },
    deltas: {
      correctness: round(sol_high.correctness - luna_max.correctness),
      robustness: delta(luna_max.robustness, sol_high.robustness),
      latency: delta(luna_max.latency, sol_high.latency),
      resource_use: delta(luna_max.resource_use, sol_high.resource_use),
      tool_execution: delta(luna_max.tool_execution, sol_high.tool_execution),
      discussion_quality: delta(luna_max.discussion_quality, sol_high.discussion_quality),
    },
    notes,
  };
}
