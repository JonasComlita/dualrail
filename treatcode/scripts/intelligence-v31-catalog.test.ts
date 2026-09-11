import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { describe, test } from "bun:test";
import { IntelligenceV31CatalogService } from "../src/intelligenceV31Catalog";

const corpusRoot = path.resolve(import.meta.dir, "..", "..", "benchmarks", "intelligence-v3.1");

describe("Intelligence v3.1 public catalog", () => {
  test("surfaces every development phase and publication blocker without overclaiming readiness", () => {
    const catalog = new IntelligenceV31CatalogService().catalog();
    assert.equal(catalog.official, false);
    assert.equal(catalog.phases.diagnostic.task_count, 2);
    assert.equal(catalog.phases.pilot.task_count, 30);
    assert.equal(catalog.phases.pilot.executable_packages, 0);
    assert.equal(catalog.phases.pilot.qualified_then_disposed, 30);
    assert.equal(catalog.phases.authoring.required_shards, 20);
    assert.equal(catalog.phases.authoring.tasks_per_shard, 5);
    assert.equal(catalog.phases.authoring.track_mix.frontier_math, 10);
    assert.equal(catalog.phases.frozen.required, 100);
    assert.equal(catalog.phases.official.status, "blocked");
    assert.equal(catalog.final_contract.task_count, 100);
    assert.equal(catalog.final_contract.authoring_recovery.checkpoint_publication, "atomic_directory_rename");
    assert.equal(catalog.final_contract.execution_identity_readiness.required_calibration_observations, 1800);
    assert.equal(catalog.final_contract.execution_identity_readiness.required_subject_attempts, 200);
    assert.equal(catalog.final_contract.infrastructure_readiness.verified_os_or_container_network_isolation, false);
    assert.equal(catalog.replication_target.score_band.minimum, 60);
    assert.equal(catalog.replication_target.score_band.maximum, 75);
    assert.equal(catalog.replication_target.sol_lead_tasks.minimum, 1);
    assert.equal(catalog.replication_target.sol_lead_tasks.maximum, 4);
    assert.equal(catalog.score_scope, "held_out_multi_track_task_pass_rate");
    assert.equal(catalog.dimension_observation_schema, "score-observation.schema.v3.1.json");
    assert.equal(catalog.execution_modes.join(","), "scalar,repository,terminal");
    assert.equal(catalog.tracks.length, 8);
    assert.equal(catalog.score_dimensions.no_iq_composite, true);
    assert.equal(catalog.final_contract.track_mix.frontier_math, 10);
    assert.equal(catalog.pilot_tasks.length, 30);
    assert.ok(catalog.blockers.some((item) => item.includes("independent authorship")));
    assert.ok(catalog.blockers.some((item) => item.includes("held-out 100-task")));
    assert.ok(catalog.blockers.some((item) => item.includes("network isolation")));
    assert.ok(catalog.blockers.some((item) => item.includes("provider-signed model execution identity")));
  });

  test("returns only aggregate non-official evidence", () => {
    const service = new IntelligenceV31CatalogService();
    const latest = service.latestComparison();
    const serialized = JSON.stringify(latest);
    assert.ok(["pilot", "development"].includes(latest.phase));
    if (latest.phase === "pilot") assert.ok(serialized.includes("ceiling_family_discarded"));
    else assert.ok(serialized.includes("development"));
    assert.ok(!serialized.includes("grader_bundle_hash"));
    assert.ok(!serialized.includes("private.json"));
    assert.ok(!serialized.includes("stdout"));
    assert.ok(!serialized.includes("stderr"));
  });

  test("surfaces a completed disposable development comparison without private grader details", () => {
    const tempRoot = mkdtempSync(path.join(os.tmpdir(), "treatcode-v31-catalog-test-"));
    try {
      const evidenceRoot = path.join(tempRoot, "build", "treatcode-plan-evidence", "P14");
      mkdirSync(evidenceRoot, { recursive: true });
      writeFileSync(path.join(evidenceRoot, "intelligence-v31-development-comparison-dev-catalog.json"), JSON.stringify({
        schema: "treatcode.intelligence.development-comparison.v3.1",
        version: "3.1",
        phase: "development",
        official: false,
        run_id: "dev-catalog",
        generated_at: "2026-09-06T00:00:00.000Z",
        complete: true,
        task_count: 1,
        luna_score: 100,
        sol_score: 0,
        sol_lead: -100,
        target_reproduced: false,
        paired_confidence_interval: { confidence_level: 0.95, low: -100, high: -100 },
        exact_sign_test: { sol_wins: 0, luna_wins: 1, ties: 0, two_sided_p: 1 },
        task_matrix: [{ task_id: "TC-V31-FINAL-001", luna_passed: true, sol_passed: false }],
        grader_bundle_hash: "private-detail-should-not-be-served",
      }) + "\n");
      const latest = new IntelligenceV31CatalogService(corpusRoot, tempRoot).latestComparison();
      assert.equal(latest.phase, "development");
      assert.equal(latest.comparison.scores.luna_max, 100);
      assert.equal(latest.comparison.scores.sol_high, 0);
      assert.equal(JSON.stringify(latest).includes("private-detail-should-not-be-served"), false);
    } finally {
      rmSync(tempRoot, { recursive: true, force: true });
    }
  });
});
