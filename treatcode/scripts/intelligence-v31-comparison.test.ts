import assert from "node:assert/strict";
import { describe, test } from "bun:test";
import { compareIntelligenceV31 } from "../src/intelligenceV31Comparison";

const outcomes = (passed: number) => Array.from({ length: 100 }, (_, index) => ({ task_id: `TC-V31-FINAL-${String(index + 1).padStart(3, "0")}`, passed: index < passed }));

describe("Intelligence v3.1 held-out comparison", () => {
  test("recognizes the requested held-out 69 versus 67 replication", () => {
    const result = compareIntelligenceV31({ suite_frozen: true, suite_hash: "a".repeat(64), publication_ready: true, luna: { model: "gpt-5.6-luna", reasoning_effort: "max", outcomes: outcomes(67) }, sol: { model: "gpt-5.6-sol", reasoning_effort: "high", outcomes: outcomes(69) }, seed: 7 });
    assert.equal(result.official, true);
    assert.equal(result.target_reproduced, true);
    assert.equal(result.sol_lead, 2);
    assert.equal(result.exact_sign_test.sol_wins, 2);
    assert.equal(result.exact_sign_test.luna_wins, 0);
    assert.equal(result.task_matrix.length, 100);
  });

  test("reports a miss without changing the frozen target", () => {
    const result = compareIntelligenceV31({ suite_frozen: true, suite_hash: "b".repeat(64), publication_ready: true, luna: { model: "gpt-5.6-luna", reasoning_effort: "max", outcomes: outcomes(80) }, sol: { model: "gpt-5.6-sol", reasoning_effort: "high", outcomes: outcomes(81) } });
    assert.equal(result.official, true);
    assert.equal(result.target_reproduced, false);
  });

  test("rejects incomplete, reordered, and unfrozen comparisons", () => {
    const incomplete = compareIntelligenceV31({ suite_frozen: false, suite_hash: null, publication_ready: false, luna: { model: "gpt-5.6-luna", reasoning_effort: "max", outcomes: outcomes(67).slice(0, 99) }, sol: { model: "gpt-5.6-sol", reasoning_effort: "high", outcomes: outcomes(69).slice(0, 99).reverse() } });
    assert.equal(incomplete.official, false);
    assert.equal(incomplete.target_reproduced, false);
    assert.ok(incomplete.blockers.some((item) => item.includes("exactly 100")));
    assert.ok(incomplete.blockers.some((item) => item.includes("identical")));
    assert.ok(incomplete.blockers.some((item) => item.includes("frozen")));
    assert.ok(incomplete.blockers.some((item) => item.includes("authorship")));
  });

  test("keeps complete development evidence non-official until publication gates pass", () => {
    const result = compareIntelligenceV31({ suite_frozen: true, suite_hash: "c".repeat(64), publication_ready: false, luna: { model: "gpt-5.6-luna", reasoning_effort: "max", outcomes: outcomes(67) }, sol: { model: "gpt-5.6-sol", reasoning_effort: "high", outcomes: outcomes(69) } });
    assert.equal(result.complete, true);
    assert.equal(result.official, false);
    assert.equal(result.target_reproduced, true);
    assert.ok(result.blockers.some((item) => item.includes("publication")));
  });
});
