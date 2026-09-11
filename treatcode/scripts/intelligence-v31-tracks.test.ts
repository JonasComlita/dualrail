import assert from "node:assert/strict";
import { describe, test } from "bun:test";
import manifest from "../../benchmarks/intelligence-v3.1/tracks.v3.1.json";
import observationSchema from "../../benchmarks/intelligence-v3.1/score-observation.schema.v3.1.json";
import {
  compareIntelligenceV31Dimensions,
  scoreIntelligenceV31Dimensions,
  validateIntelligenceV31TracksManifest,
} from "../src/intelligenceV31Tracks";

describe("Intelligence v3.1 track portfolio and independent dimensions", () => {
  test("validates the complementary benchmark inspirations", () => {
    assert.deepEqual(validateIntelligenceV31TracksManifest(manifest), []);
    assert.equal(observationSchema.properties.track.enum.length, 8);
    assert.equal(observationSchema.additionalProperties, false);
    assert.ok(manifest.tracks.some((track) => track.id === "fresh_coding" && track.inspiration.includes("LiveCodeBench")));
    assert.ok(manifest.tracks.some((track) => track.id === "repository_repair" && track.inspiration.includes("SWE-bench")));
    assert.ok(manifest.tracks.some((track) => track.id === "terminal_agent" && track.inspiration.includes("Terminal-Bench")));
    assert.ok(manifest.tracks.some((track) => track.id === "expert_reasoning" && track.inspiration.includes("GPQA")));
    assert.ok(manifest.tracks.some((track) => track.id === "frontier_math" && track.inspiration.includes("FrontierMath")));
    assert.ok(manifest.tracks.some((track) => track.id === "abstract_generalization" && track.inspiration.includes("ARC-AGI-2")));
  });

  test("keeps latency, resources, tools, and discussion independent from correctness", () => {
    const observations = [
      {
        task_id: "fresh-001",
        track: "fresh_coding" as const,
        correctness: true,
        robustness: 0.5,
        wall_clock_ms: 100,
        latency_budget_ms: 200,
        cpu_ms: 50,
        cpu_budget_ms: 100,
        memory_mb: 64,
        memory_budget_mb: 128,
        tool_calls: 4,
        successful_tool_calls: 3,
        tool_errors: 1,
        discussion_quality: 80,
      },
      {
        task_id: "repo-001",
        track: "repository_repair" as const,
        correctness: true,
        robustness: 1,
        wall_clock_ms: 400,
        latency_budget_ms: 200,
        cpu_ms: 150,
        cpu_budget_ms: 100,
        memory_mb: 192,
        memory_budget_mb: 128,
        tool_calls: 4,
        successful_tool_calls: 4,
        tool_errors: 0,
        discussion_quality: 40,
      },
    ];
    const score = scoreIntelligenceV31Dimensions(observations);
    assert.equal(score.correctness, 100);
    assert.equal(score.robustness, 75);
    assert.equal(score.latency, 75);
    assert.equal(score.resource_use, 83.34);
    assert.equal(score.tool_execution, 87.5);
    assert.equal(score.discussion_quality, 60);
    assert.deepEqual(score.notes, []);
  });

  test("reports null for telemetry that was not collected instead of inventing a score", () => {
    const score = scoreIntelligenceV31Dimensions([{ task_id: "repo-002", track: "repository_repair", correctness: false }]);
    assert.equal(score.correctness, 0);
    assert.equal(score.robustness, null);
    assert.equal(score.latency, null);
    assert.equal(score.resource_use, null);
    assert.equal(score.tool_execution, null);
    assert.equal(score.discussion_quality, null);
    assert.ok(score.notes.some((note) => note.includes("trace hash")));
  });

  test("computes paired dimension deltas without collapsing them", () => {
    const luna = [{ task_id: "repo-003", track: "repository_repair" as const, correctness: true, wall_clock_ms: 400, latency_budget_ms: 200 }];
    const sol = [{ task_id: "repo-003", track: "repository_repair" as const, correctness: true, wall_clock_ms: 100, latency_budget_ms: 200 }];
    const comparison = compareIntelligenceV31Dimensions(luna, sol);
    assert.equal(comparison.deltas.correctness, 0);
    assert.equal(comparison.deltas.latency, 50);
    assert.equal(comparison.deltas.robustness, null);
    assert.equal(comparison.by_track.luna_max.repository_repair?.correctness, 100);
    assert.equal(comparison.by_track.sol_high.repository_repair?.latency, 100);
  });
});
