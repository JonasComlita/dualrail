import assert from "node:assert/strict";
import { describe, test } from "bun:test";
import { selectIntelligenceV31CalibrationFamilies } from "./intelligence-v31-calibration-assignments.mjs";

const assignments = [
  { model_family: "gpt-5.4-mini", reasoning_effort: "low" },
  { model_family: "gpt-5.5", reasoning_effort: "low" },
  { model_family: "gpt-5.6-terra", reasoning_effort: "low" },
];

describe("Intelligence v3.1 calibration-family assignment", () => {
  test("uses the two remaining families when the author belongs to the calibration pool", () => {
    assert.deepEqual(selectIntelligenceV31CalibrationFamilies("TC-V31-FINAL-001", "gpt-5.4-mini", assignments).map((item) => item.model_family), ["gpt-5.5", "gpt-5.6-terra"]);
  });

  test("rotates a deterministic pair when the author is outside the pool", () => {
    assert.deepEqual(selectIntelligenceV31CalibrationFamilies("TC-V31-FINAL-001", "gpt-5.4", assignments).map((item) => item.model_family), ["gpt-5.4-mini", "gpt-5.5"]);
    assert.deepEqual(selectIntelligenceV31CalibrationFamilies("TC-V31-FINAL-002", "gpt-5.4", assignments).map((item) => item.model_family), ["gpt-5.5", "gpt-5.6-terra"]);
    assert.deepEqual(selectIntelligenceV31CalibrationFamilies("TC-V31-FINAL-003", "gpt-5.4", assignments).map((item) => item.model_family), ["gpt-5.6-terra", "gpt-5.4-mini"]);
  });

  test("rejects duplicate or insufficient family pools", () => {
    assert.throws(() => selectIntelligenceV31CalibrationFamilies("TC-V31-FINAL-001", "gpt-5.5", assignments.slice(0, 2)), /fewer than two/);
    assert.throws(() => selectIntelligenceV31CalibrationFamilies("TC-V31-FINAL-001", "gpt-5.5", [assignments[0], assignments[0], assignments[2]]), /duplicate/);
  });
});
