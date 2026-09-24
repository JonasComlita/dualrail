export const TI_SCHEMA = "treatcode.ternary-intelligence.v1" as const;
export const PILOT_LABEL = "Pilot · Windows-native execution";
export const CAPABILITIES = [
  { id: "representation", name: "Representation & arithmetic", description: "Encoding, carries, overflow, rounding, and packed trits." },
  { id: "logic", name: "Logical reasoning", description: "Unknown values, truth tables, equivalence, and state transitions." },
  { id: "learning", name: "Learning unfamiliar rules", description: "Learn new symbols and machines, then transfer to unseen cases." },
  { id: "algorithms", name: "Algorithm design", description: "Search, ordering, ternary structures, and resource tradeoffs." },
  { id: "debugging", name: "Debugging", description: "Locate and repair arithmetic, compiler, and integration defects." },
  { id: "systems", name: "Systems engineering", description: "ABI, alignment, memory, persistence, and scheduling contracts." },
] as const;
export type Capability = typeof CAPABILITIES[number]["id"];
export type Difficulty = "introductory" | "intermediate" | "advanced";
export type Condition = "prior" | "specification" | "learning";
export type ToolMode = "model-only" | "tools";
export type Provider = "openai" | "anthropic" | "google" | "codex";
export type RunState = "queued" | "running" | "completed" | "cancelled" | "interrupted";
export type EpisodeState = "pending" | "running" | "passed" | "failed" | "infrastructure_error" | "interrupted" | "cancelled";
export interface Limits {
  episodeMs: number; inputTokens: number; outputTokens: number; toolCalls: number;
  publicTestCalls: number; commandMs: number; memoryMiB: number; processes: number; outputBytes: number;
}
export const DEFAULT_LIMITS: Readonly<Limits> = Object.freeze({
  episodeMs: 900_000, inputTokens: 100_000, outputTokens: 24_000, toolCalls: 64,
  publicTestCalls: 12, commandMs: 120_000, memoryMiB: 512, processes: 8, outputBytes: 1_048_576,
});
export interface ModelConfig {
  id: string; provider: Provider; model: string; keyEnv: string;
  settings: Record<string, unknown>;
  supportedSettings: Record<string, Array<string | number | boolean>>;
  maxOutputTokens: number;
  pricing: { snapshot: string; date: string; inputUsdPerMillion: number; outputUsdPerMillion: number } | null;
}
export interface PublicFamily { id: string; capability: Capability; difficulty: Difficulty; title: string; qualified: boolean }
export interface Assignment { familyId: string; condition: Condition; mode: ToolMode; seed: number; repeat: number }
export interface Protocol {
  transport?: "provider-api" | "codex-harness";
  harness?: { version: string; policy: Record<string, unknown> };
  revision: string; profile: string; limits: Limits; familyHashes: Record<string, string>;
  checkerHash: string; workerHash: string; compilerHash: string; promptHash: string;
  controlledFamilies: string[]; assignments: Assignment[]; purpose: "calibration" | "evaluation";
  experiment: "default" | "controlled"; bootstrapSamples: 10000; label: typeof PILOT_LABEL;
}
export interface Usage { inputTokens: number; outputTokens: number; reasoningTokens: number | null; cachedInputTokens: number | null }
export interface Resources extends Usage {
  toolCalls: number; publicTestCalls: number; modelMs: number; programMs: number | null;
  peakMemoryBytes: number | null; costNanoUsd: number | null;
}
export interface LearningStage { round: 0 | 1 | 3; passed: number; total: number; success: boolean }
export interface Observation {
  familyId: string; modelConfigId: string; condition: Condition; mode: ToolMode; repeat: number;
  state: EpisodeState; success: boolean | null; category: string; resources: Resources;
  learning: LearningStage[]; robustness: { passed: number; total: number } | null;
  providerModels?: string[];
}
export interface RunView {
  id: string; state: RunState; createdAt: string; updatedAt: string; protocolHash: string;
  protocol: Protocol; models: ModelConfig[]; total: number; finished: number; activeEpisodeId: string | null;
  spendingCeilingUsd: number | null; spentNanoUsd: number; reservedNanoUsd: number;
  provenance: "live" | "fixture"; publicationFailures: string[];
}
export interface EpisodeView { id: string; runId: string; ordinal: number; modelConfigId: string; assignment: Assignment; state: EpisodeState; observation: Observation | null }
export interface RunEvent { sequence: number; episodeId: string | null; type: string; at: string; data: unknown }
export interface ScoreRow { modelConfigId: string; capability: Capability | "all"; condition: Condition; mode: ToolMode; successes: number; evaluated: number; expected: number; rate: number | null; interval: [number, number] | null }
export interface Publication {
  id: string; createdAt: string; label: typeof PILOT_LABEL; protocolHash: string; profileHash: string;
  revision: string; models: Array<Omit<ModelConfig, "keyEnv" | "supportedSettings">>;
  protocol?: Pick<Protocol,"limits"|"experiment"|"transport"|"harness"|"promptHash"|"checkerHash">;
  scores: ScoreRow[]; observations: Observation[]; coverage: { complete: boolean; expected: number; evaluated: number };
  environment: { platform: string; compilerHash: string; workerHash: string }; runId: string;
}
export interface PublicExample {
  id: string; capability: Capability; title: string; instructions: string;
  kind: "arithmetic" | "logic" | "rules" | "code";
  input: string; source?: string; workedSolution: string; recordedOutput?: string;
}
