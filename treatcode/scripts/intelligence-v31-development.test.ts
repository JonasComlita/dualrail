import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { describe, test } from "bun:test";
import { collectIntelligenceV31DevelopmentComparison } from "../src/intelligenceV31Development";

const hash = (letter: string) => letter.repeat(64);

function writeGrade(runRoot: string, runId: string, taskId: string, model: "gpt-5.6-luna" | "gpt-5.6-sol", effort: "max" | "high", passed: boolean, bundleHash: string): void {
  const directory = model === "gpt-5.6-luna" ? "gpt-5.6-luna-max" : "gpt-5.6-sol-high";
  const grade = {
    schema: "treatcode.intelligence.final-subject-grade.v3.1",
    version: "3.1",
    official: false,
    phase: "development",
    run_id: runId,
    task_id: taskId,
    model,
    reasoning_effort: effort,
    suite_sha256: null,
    participant_bundle_sha256: bundleHash,
    within_budget: true,
    passed,
    completion_status: "completed",
    sealed_report: {
      sealed: true,
      official: false,
      task_id: taskId,
      passed,
      participant_bundle_hash: bundleHash,
      submission_bundle_hash: hash("b"),
      grader_bundle_hash: hash("c"),
      evidence_hash: hash("d"),
    },
    agent_execution: {
      fresh_context: true,
      cross_task_memory_disabled: true,
      attempt_count: 1,
      tool_trace_sha256: hash("e"),
    },
  };
  const directoryRoot = path.join(runRoot, taskId, directory);
  mkdirSync(directoryRoot, { recursive: true });
  writeFileSync(path.join(directoryRoot, "attempt.json"), `${JSON.stringify({ run_id: runId, task_id: taskId, model, reasoning_effort: effort })}\n`);
  writeFileSync(path.join(directoryRoot, "sealed-grade.json"), `${JSON.stringify(grade)}\n`);
}

function writePilotGrade(runRoot: string, runId: string, taskId: string, model: "gpt-5.6-luna" | "gpt-5.6-sol", effort: "max" | "high", passed: boolean, bundleHash: string): void {
  const directory = model === "gpt-5.6-luna" ? "gpt-5.6-luna-max" : "gpt-5.6-sol-high";
  const grade = {
    schema: "treatcode.intelligence.pilot-subject-grade.v3.1",
    version: "3.1",
    official: false,
    disposable: true,
    task_id: taskId,
    model,
    reasoning_effort: effort,
    passed,
    completion_status: "completed",
    subject_timing: { within_budget: true, elapsed_ms: 42, budget_ms: 1_200_000 },
    sealed_report: {
      sealed: true,
      official: false,
      task_id: taskId,
      passed,
      participant_bundle_hash: bundleHash,
      submission_bundle_hash: hash("b"),
      grader_bundle_hash: hash("c"),
      evidence_hash: hash("d"),
    },
  };
  const directoryRoot = path.join(runRoot, taskId, directory);
  mkdirSync(directoryRoot, { recursive: true });
  writeFileSync(path.join(directoryRoot, "attempt.json"), `${JSON.stringify({ run_id: runId, task_id: taskId, model, reasoning_effort: effort })}\n`);
  writeFileSync(path.join(directoryRoot, "sealed-grade.json"), `${JSON.stringify(grade)}\n`);
}

describe("Intelligence v3.1 development comparison aggregation", () => {
  test("produces a paired score from one real-shaped sealed task per model", () => {
    const tempRoot = mkdtempSync(path.join(os.tmpdir(), "treatcode-v31-development-test-"));
    try {
      const runId = "dev-score-test";
      const taskId = "TC-V31-FINAL-001";
      const runRoot = path.join(tempRoot, runId);
      const bundleHash = hash("a");
      writeGrade(runRoot, runId, taskId, "gpt-5.6-luna", "max", true, bundleHash);
      writeGrade(runRoot, runId, taskId, "gpt-5.6-sol", "high", false, bundleHash);
      const result = collectIntelligenceV31DevelopmentComparison(runRoot, runId, "2026-09-06T00:00:00.000Z");
      assert.equal(result.schema, "treatcode.intelligence.development-comparison.v3.1");
      assert.equal(result.official, false);
      assert.equal(result.complete, true);
      assert.equal(result.task_count, 1);
      assert.equal(result.luna_score, 100);
      assert.equal(result.sol_score, 0);
      assert.equal(result.sol_lead, -100);
      assert.equal(result.exact_sign_test.luna_wins, 1);
      assert.equal(result.dimensions.luna_max.correctness, 100);
      assert.equal(result.dimensions.sol_high.correctness, 0);
      assert.equal(result.dimensions.luna_max.robustness, null);
      assert.equal(result.task_matrix[0].task_id, taskId);
      assert.equal(result.source.grade_count, 2);
      assert.equal(result.source.grade_sha256.length, 2);
      assert.ok(result.source.grade_sha256.every((value) => /^[a-f0-9]{64}$/.test(value)));
    } finally {
      rmSync(tempRoot, { recursive: true, force: true });
    }
  });

  test("rejects a one-sided or cross-bundle run", () => {
    const tempRoot = mkdtempSync(path.join(os.tmpdir(), "treatcode-v31-development-test-"));
    try {
      const runId = "dev-incomplete-test";
      const taskId = "TC-V31-FINAL-001";
      const runRoot = path.join(tempRoot, runId);
      writeGrade(runRoot, runId, taskId, "gpt-5.6-luna", "max", true, hash("a"));
      assert.throws(() => collectIntelligenceV31DevelopmentComparison(runRoot, runId), /both Luna max and Sol high/);
      writeGrade(runRoot, runId, taskId, "gpt-5.6-sol", "high", true, hash("b"));
      assert.throws(() => collectIntelligenceV31DevelopmentComparison(runRoot, runId), /same participant bundle/);
    } finally {
      rmSync(tempRoot, { recursive: true, force: true });
    }
  });

  test("accepts a freshly graded disposable pilot run as development evidence", () => {
    const tempRoot = mkdtempSync(path.join(os.tmpdir(), "treatcode-v31-development-pilot-test-"));
    try {
      const runId = "live-pilot-test";
      const taskId = "TC-V31-PILOT-001";
      const runRoot = path.join(tempRoot, runId);
      const bundleHash = hash("a");
      writePilotGrade(runRoot, runId, taskId, "gpt-5.6-luna", "max", true, bundleHash);
      writePilotGrade(runRoot, runId, taskId, "gpt-5.6-sol", "high", true, bundleHash);
      const result = collectIntelligenceV31DevelopmentComparison(runRoot, runId, "2026-09-06T00:00:00.000Z");
      assert.equal(result.complete, true);
      assert.equal(result.task_count, 1);
      assert.equal(result.luna_score, 100);
      assert.equal(result.sol_score, 100);
      assert.equal(result.task_matrix[0].track, "repository_repair");
    } finally {
      rmSync(tempRoot, { recursive: true, force: true });
    }
  });
});
