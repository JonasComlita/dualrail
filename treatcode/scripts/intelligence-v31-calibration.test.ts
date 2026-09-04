import assert from "node:assert/strict";
import { describe, test } from "bun:test";
import { calibrateIntelligenceV31Candidates, type IntelligenceV31CalibrationObservation } from "../src/intelligenceV31Calibration";

const hash = (digit: string) => digit.repeat(64);

function fixture() {
  const tasks = Array.from({ length: 100 }, (_, index) => ({
    task_id: `TC-V31-FINAL-${String(index + 1).padStart(3, "0")}`,
    participant_bundle_hash: hash((index % 9 + 1).toString()),
    author_model_family: "gpt-5.4",
    qualification: { reference_passed: true, starter_failed: true, mutants_caught: 2, mutants_total: 2 },
  }));
  let attempt = 0;
  const observations: IntelligenceV31CalibrationObservation[] = [];
  for (const task of tasks) for (const [cohortIndex, cohort] of (["weak", "medium", "frontier"] as const).entries()) for (const model of ["gpt-5.5", "gpt-5.6-terra"]) for (const run of [1, 2, 3] as const) {
    attempt += 1;
    const passed = cohortIndex === 2 || (cohortIndex === 1 && (run > 1 || model === "gpt-5.6-terra"));
    observations.push({ schema: "treatcode.intelligence.calibration-observation.v3.1", version: "3.1", task_id: task.task_id, participant_bundle_hash: task.participant_bundle_hash, attempt_id: `attempt-${attempt}`, cohort, model_family: model, configuration: `${model}-${cohort}`, run_number: run, fresh_context: true, subject_family_excluded: true, network_isolation_verified: true, resource_limits_verified: true, append_only_evidence_verified: true, passed, completion_status: "completed", released_at: "2026-08-29T00:00:00Z", submitted_at: "2026-08-29T00:01:00Z", evidence_hash: attempt.toString(16).padStart(64, "0"), official: false });
  }
  return { tasks, observations };
}

describe("Intelligence v3.1 calibration", () => {
  test("accepts a complete ordered, discriminating, non-subject matrix", () => {
    const report = calibrateIntelligenceV31Candidates(fixture());
    assert.equal(report.status, "ready_to_freeze");
    assert.equal(report.accepted_tasks, 100);
    assert.equal(report.observation_count, 1800);
    assert.equal(report.ceiling_fraction, 0);
    assert.equal(report.floor_fraction, 0);
  });

  test("rejects author-family contamination and incomplete evidence", () => {
    const value = fixture();
    value.observations[0] = { ...value.observations[0], model_family: "gpt-5.4" };
    value.observations.pop();
    const report = calibrateIntelligenceV31Candidates(value);
    assert.equal(report.status, "insufficient_data");
    assert.ok(report.data_issues.some((issue) => issue.includes("author-family")));
    assert.equal(report.insufficient_tasks, 2);
  });

  test("binds observations to the candidate hash and unique evidence", () => {
    const value = fixture();
    value.observations[1] = { ...value.observations[1], participant_bundle_hash: hash("f"), evidence_hash: value.observations[0].evidence_hash };
    const report = calibrateIntelligenceV31Candidates(value);
    assert.equal(report.status, "insufficient_data");
    assert.ok(report.data_issues.some((issue) => issue.includes("different participant bundle")));
    assert.ok(report.data_issues.some((issue) => issue.includes("evidence hash")));
  });
});
