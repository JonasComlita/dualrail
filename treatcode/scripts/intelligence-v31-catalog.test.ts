import assert from "node:assert/strict";
import { describe, test } from "bun:test";
import { IntelligenceV31CatalogService } from "../src/intelligenceV31Catalog";

describe("Intelligence v3.1 public catalog", () => {
  test("surfaces every development phase and publication blocker without overclaiming readiness", () => {
    const catalog = new IntelligenceV31CatalogService().catalog();
    assert.equal(catalog.official, false);
    assert.equal(catalog.phases.diagnostic.task_count, 2);
    assert.equal(catalog.phases.pilot.task_count, 30);
    assert.equal(catalog.phases.pilot.executable_packages, 0);
    assert.equal(catalog.phases.pilot.qualified_then_disposed, 30);
    assert.equal(catalog.phases.frozen.required, 100);
    assert.equal(catalog.phases.official.status, "blocked");
    assert.equal(catalog.final_contract.task_count, 100);
    assert.equal(catalog.final_contract.infrastructure_readiness.verified_os_or_container_network_isolation, false);
    assert.equal(catalog.replication_target.score_band.minimum, 60);
    assert.equal(catalog.replication_target.score_band.maximum, 75);
    assert.equal(catalog.replication_target.sol_lead_tasks.minimum, 1);
    assert.equal(catalog.replication_target.sol_lead_tasks.maximum, 4);
    assert.equal(catalog.pilot_tasks.length, 30);
    assert.ok(catalog.blockers.some((item) => item.includes("independent authorship")));
    assert.ok(catalog.blockers.some((item) => item.includes("held-out 100-task")));
    assert.ok(catalog.blockers.some((item) => item.includes("network isolation")));
  });

  test("returns only aggregate pilot evidence", () => {
    const service = new IntelligenceV31CatalogService();
    const serialized = JSON.stringify(service.latestComparison());
    assert.ok(serialized.includes("pilot"));
    assert.ok(serialized.includes("ceiling_family_discarded"));
    assert.ok(!serialized.includes("grader_bundle_hash"));
    assert.ok(!serialized.includes("private.json"));
    assert.ok(!serialized.includes("stdout"));
    assert.ok(!serialized.includes("stderr"));
  });
});
