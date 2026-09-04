import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { loadCourse, referenceExists, writeEvidence } from "./learning-test-utils.mjs";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const apiRoot = path.join(appRoot, "public", "api", "v1");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P15");
const resources = ["projects", "stack_nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps"];
const errors = [];
const checks = [];
let inventorySummary = { records: 0, resources: resources.length, stack_phases: 0 };
let provenanceAudit = {
  snapshot_commit: null,
  static_resource_commits: {},
  static_resource_parity: false,
  coverage_parity: false,
  relationship_index_parity: false,
  freshness_parity: false,
  source_link_integrity: false,
  public_relationships: 0,
  unresolved_external_edges: 0,
};

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function readJson(name) {
  return JSON.parse(fs.readFileSync(path.join(apiRoot, name), "utf8"));
}

function commitAtHead() {
  return execFileSync("git", ["rev-parse", "HEAD"], { cwd: repoRoot, encoding: "utf8" }).trim();
}

function safeRepositoryPath(value) {
  const normalized = String(value || "").replaceAll("\\", "/");
  return normalized && !normalized.startsWith("/") && !normalized.includes("..") && !/(^|\/)(?:build|node_modules|coverage|scratch|\.git|\.codex)(?:\/|$)/i.test(normalized) && !/(^|\/)(?:\.env(?:\.|$)|secrets?|credentials?|private)(?:\/|$)/i.test(normalized);
}

function reportFor(schema, ok, extra = {}) {
  return { schema, ok, checks, errors, ...extra };
}

try {
  const snapshot = readJson("snapshot.json");
  const coverage = readJson("coverage.json");
  const relationshipIndex = readJson("relationship-index.json");
  const freshness = readJson("freshness.json");
  const stackManifest = JSON.parse(fs.readFileSync(path.join(repoRoot, "STACK_MANIFEST.json"), "utf8"));
  const expectedStackIds = Array.isArray(stackManifest.layers) ? stackManifest.layers.map((layer) => layer.id) : [];
  const repositoryIndex = JSON.parse(fs.readFileSync(path.join(repoRoot, "build", "treatcode-index", "repository-index.v1.json"), "utf8"));
  const currentCommit = commitAtHead();
  assert(snapshot.schema_version === "treatcode.public.snapshot.v1", "snapshot schema is not v1");
  assert(/^[0-9a-f]{40}$/.test(snapshot.snapshot.commit), "snapshot commit is not a full repository commit");
  assert(snapshot.snapshot.commit === currentCommit, "snapshot commit does not match repository HEAD");
  assert(expectedStackIds.length > 0, "STACK_MANIFEST.json does not declare any stack phases");
  assert(snapshot.stack_nodes.length === expectedStackIds.length, `stack phase count differs from STACK_MANIFEST.json: expected ${expectedStackIds.length}, found ${snapshot.stack_nodes.length}`);
  assert(snapshot.stack_nodes.map((phase) => phase.source_id).join("\u0000") === expectedStackIds.join("\u0000"), "snapshot stack phase IDs are not in STACK_MANIFEST.json order");
  assert(coverage.stack_phase_count === snapshot.stack_nodes.length, "coverage stack count does not match snapshot");
  assert(coverage.all_records_reachable === true && coverage.all_stack_phases_useful === true, "coverage report does not prove full reachability");
  assert(coverage.no_hidden_first_n_limit === true, "coverage report does not declare the no-hidden-limit contract");
  assert(freshness.stale === false && freshness.p03_index_available === true, "public freshness report is stale or missing the P03 index");
  assert(freshness.snapshot_commit === snapshot.snapshot.commit, "freshness and snapshot commits differ");
  assert(freshness.p03_index_commit === snapshot.snapshot.commit, "P03 index and snapshot commits differ");
  assert(repositoryIndex.repository.commit === snapshot.snapshot.commit, "repository index payload has a stale commit");
  const staticResourceCommits = Object.fromEntries(resources.map((resource) => [resource, readJson(`${resource}.json`).snapshot?.commit || null]));
  assert(Object.values(staticResourceCommits).every((commit) => commit === snapshot.snapshot.commit), "a static resource envelope has a stale snapshot commit");
  for (const resource of resources) {
    const staticEnvelope = readJson(`${resource}.json`);
    assert(JSON.stringify(staticEnvelope.data) === JSON.stringify(snapshot[resource]), `${resource} static envelope disagrees with snapshot collection`);
  }
  assert(JSON.stringify(readJson("coverage.json")) === JSON.stringify(snapshot.coverage), "static coverage artifact disagrees with snapshot metadata");
  assert(relationshipIndex.schema === snapshot.relationship_index.schema, "static relationship index schema disagrees with snapshot metadata");
  assert(relationshipIndex.counts?.public_edges === snapshot.relationship_index.edge_count, "static relationship index edge count disagrees with snapshot metadata");
  assert(relationshipIndex.counts?.unresolved_external_edges === snapshot.relationship_index.unresolved_external_edge_count, "static relationship index unresolved-edge count disagrees with snapshot metadata");
  assert(JSON.stringify(readJson("freshness.json")) === JSON.stringify(snapshot.freshness), "static freshness artifact disagrees with snapshot metadata");
  provenanceAudit = {
    ...provenanceAudit,
    snapshot_commit: snapshot.snapshot.commit,
    static_resource_commits: staticResourceCommits,
    static_resource_parity: true,
    coverage_parity: true,
    relationship_index_parity: true,
    freshness_parity: true,
  };
  checks.push("snapshot, coverage, P03 index, and freshness commits agree");

  const ids = new Map();
  for (const resource of resources) {
    const collection = snapshot[resource];
    const resourceCoverage = coverage.resources?.[resource] || [];
    assert(Array.isArray(collection) && collection.length > 0, `${resource} collection is empty`);
    assert(coverage.collection_counts?.[resource] === collection.length, `${resource} coverage count is incorrect`);
    assert(resourceCoverage.length === collection.length, `${resource} coverage rows do not cover every record`);
    for (const record of collection) {
      assert(typeof record.id === "string" && record.id.startsWith("tc:"), `${resource} contains a record without a stable id`);
      assert(!ids.has(record.id), `record id is duplicated: ${record.id}`);
      ids.set(record.id, resource);
      assert(record.public_resource === resource, `${record.id} has the wrong public_resource`);
      assert(record.public_route === `/resources/${resource}/${encodeURIComponent(record.id)}`, `${record.id} has a non-deterministic public route`);
      assert(record.evidence_route === `/evidence/${encodeURIComponent(record.id)}`, `${record.id} has a non-deterministic evidence route`);
      assert(Array.isArray(record.source_refs) && record.source_refs.length > 0, `${record.id} has no source provenance`);
      assert(Array.isArray(record.evidence_refs) && record.evidence_refs.length > 0, `${record.id} has no evidence provenance`);
      for (const reference of [...record.source_refs, ...record.evidence_refs]) {
        assert(/^https:\/\//.test(String(reference.repository || "")), `${record.id} has invalid provenance repository`);
        assert(/^[0-9a-f]{7,64}$/i.test(String(reference.commit || "")), `${record.id} has invalid provenance commit`);
        if (reference.path) assert(safeRepositoryPath(reference.path), `${record.id} exposes an unsafe provenance path: ${reference.path}`);
      }
      const covered = resourceCoverage.find((row) => row.id === record.id);
      assert(covered?.reachable === true && covered.route === record.public_route && covered.evidence_route === record.evidence_route, `${record.id} is not covered by its resource route row`);
    }
  }
  inventorySummary = { records: ids.size, resources: resources.length, stack_phases: expectedStackIds.length };
  provenanceAudit.source_link_integrity = true;
  provenanceAudit.public_relationships = (snapshot.relations || []).length;
  provenanceAudit.unresolved_external_edges = (relationshipIndex.unresolved_external_edges || []).length;
  checks.push(`all ${resources.length} public collections expose ${ids.size} uniquely routed records`);

  const stackCoverage = coverage.stack_phases || [];
  assert(stackCoverage.length === snapshot.stack_nodes.length, "stack coverage does not include every phase");
  for (const phase of snapshot.stack_nodes) {
    const row = stackCoverage.find((candidate) => candidate.id === phase.id);
    assert(row?.reachable === true && Array.isArray(row.required_fields) && row.missing_fields.length === 0, `${phase.id} is missing required phase detail coverage`);
    for (const field of ["problem", "inputs", "outputs", "depends_on", "dependent_ids", "status_summary", "implementation_status", "source_refs", "evidence_refs", "component_ids", "capability_ids", "contract_ids", "decision_ids", "test_ids", "benchmark_ids", "gap_ids", "release_ids"]) assert(phase[field] !== undefined, `${phase.id} is missing phase field ${field}`);
  }
  checks.push(`all ${expectedStackIds.length} stack phases expose problem, inputs, outputs, dependencies, status, and evidence links`);

  const publicRelations = snapshot.relations || [];
  assert(relationshipIndex.edges.length === publicRelations.length, "relationship index edge count differs from snapshot");
  for (const relation of publicRelations) assert(ids.has(relation.from) && ids.has(relation.to), `public relationship endpoint is unresolved: ${relation.from} -> ${relation.to}`);
  for (const relation of relationshipIndex.unresolved_external_edges || []) assert(relation.required === false && relation.reason, "an excluded repository relationship lacks an explicit non-public reason");
  assert(relationshipIndex.counts.public_edges === publicRelations.length, "relationship index public edge count is incorrect");
  checks.push(`${publicRelations.length} public relationship edges resolve to public records; excluded P03 references carry explicit reasons`);

  const publicApp = fs.readFileSync(path.join(appRoot, "src", "PublicApp.tsx"), "utf8");
  const main = fs.readFileSync(path.join(appRoot, "src", "main.tsx"), "utf8");
  assert(!publicApp.includes("FALLBACK_SNAPSHOT") && !publicApp.includes("fallbackSnapshot"), "public app still contains a reduced fallback dataset");
  assert(!publicApp.includes("records.slice(0,") && !publicApp.includes("provenance.slice(0,"), "public app contains a hidden first-N truncation");
  assert(main.includes("/resources") && main.includes("/evidence"), "public resource and evidence routes are not selected by the public entry point");
  checks.push("public UI has no reduced fallback or hidden first-N related/provenance truncation");

  const learningReport = { schema: "trit.treatcode_public_learning_coverage.v1", ok: false, counts: {}, checks: [], errors: [] };
  try {
    const course = loadCourse();
    const { catalog, matrix, pages, pagesById, stackNodes, tests, benchmarks, gaps } = course;
    const testIds = new Set(tests.map((record) => record.id));
    const benchmarkIds = new Set(benchmarks.map((record) => record.id));
    const gapIds = new Set(gaps.map((record) => record.id));
    assert(snapshot.stack_nodes.length === stackNodes.length && matrix.phase_count === stackNodes.length && snapshot.stack_nodes.length === expectedStackIds.length, "public Learn and the public snapshot disagree about stack phase count");
    assert(publicApp.includes("LEARNING_CATALOG") && publicApp.includes("LEARNING_MATRIX") && publicApp.includes("/api/public/v1/snapshot.json"), "public Learn is not backed by the generated curriculum and full snapshot API");
    assert(publicApp.includes("function MarkdownLearnPage") && publicApp.includes("/api/learn/exercises/run"), "public Learn is missing the complete renderer or bounded exercise transition");
    assert(!publicApp.includes("LegacyLearnPage") && !publicApp.includes("slice(0, 12)") && !publicApp.includes("slice(0, 2)"), "public Learn still contains a partial or truncating publication path");
    for (const phase of matrix.phases) {
      assert(stackNodes.some((node) => node.id === phase.phase_id && node.slug === phase.slug), `public snapshot lost phase ${phase.phase_id}`);
      assert(phase.source_paths.every(referenceExists), `${phase.slug} has an unresolved public source path`);
      assert(phase.test_ids.every((id) => testIds.has(id)), `${phase.slug} has an unresolved public test ID`);
      assert(phase.benchmark_ids.every((id) => benchmarkIds.has(id)), `${phase.slug} has an unresolved public benchmark ID`);
      assert(phase.gap_ids.every((id) => gapIds.has(id)), `${phase.slug} has an unresolved public gap ID`);
      for (const lesson of phase.lessons) {
        const page = pagesById.get(lesson.id);
        assert(page && page.metadata.stack_links.phase === `/stack/${phase.slug}`, `${lesson.id} has no Stack Explorer phase transition`);
        assert(page.metadata.stack_links.tests.startsWith("/stack?focus="), `${lesson.id} has no Stack Explorer test transition`);
        assert(lesson.sources.every((source) => page.metadata.sources.some((item) => item.path === source)), `${lesson.id} lost exact source provenance`);
      }
    }
    assert(catalog.glossary.length >= 75 && catalog.glossary.every((entry) => pagesById.has(entry.page_id)), "public glossary coverage is incomplete");
    assert(catalog.paths.every((pathItem) => pathItem.page_ids.length === pages.length && pathItem.page_ids.at(-1) === "full-system-validation"), "public learning paths do not reach closure");
    learningReport.counts = { stack_phases: stackNodes.length, matrix_phases: matrix.phase_count, lessons: pages.length, glossary_terms: catalog.glossary.length, tests: tests.length, benchmarks: benchmarks.length, gaps: gaps.length };
    learningReport.checks.push("full snapshot and generated matrix are consumed by public Learn");
    learningReport.checks.push("every phase and lesson retains exact source, test, benchmark, gap, and Stack Explorer links");
    learningReport.checks.push("all paths reach the closure lesson and glossary terms resolve to lessons");
    learningReport.ok = true;
  } catch (error) {
    learningReport.errors.push(String(error?.message || error));
  }
  writeEvidence("stack-integration.json", learningReport);
  writeEvidence("public-coverage.json", learningReport);
  if (!learningReport.ok) throw new Error(`P16 public learning coverage failed: ${learningReport.errors.join("; ")}`);
} catch (error) {
  errors.push(String(error?.message || error));
}

fs.mkdirSync(evidenceRoot, { recursive: true });
const ok = errors.length === 0;
const generatedCoverage = (() => { try { return readJson("coverage.json"); } catch { return {}; } })();
const reviewerState = { required_approvals: ["Product owner", "Architecture owner", "Accessibility reviewer"], approved: [], complete: false };
const pagination = Object.fromEntries(resources.map((resource) => {
  const total = Number(generatedCoverage.collection_counts?.[resource] || 0);
  return [resource, { total_records: total, page_size: 50, pages: Math.max(1, Math.ceil(total / 50)), route_template: `/resources/${resource}?page={page}` }];
}));
fs.writeFileSync(path.join(evidenceRoot, "resource-coverage.json"), `${JSON.stringify(reportFor("treatcode.public.resource-coverage.v1", ok, { ...inventorySummary, collection_counts: generatedCoverage.collection_counts || {}, records_by_resource: generatedCoverage.resources || {}, reviewer_state: reviewerState }), null, 2)}\n`);
fs.writeFileSync(path.join(evidenceRoot, "stack-coverage.json"), `${JSON.stringify(reportFor("treatcode.public.stack-coverage.v1", ok, { stack_phases: inventorySummary.stack_phases, phases: generatedCoverage.stack_phases || [], reviewer_state: reviewerState }), null, 2)}\n`);
fs.writeFileSync(path.join(evidenceRoot, "no-truncation.json"), `${JSON.stringify(reportFor("treatcode.public.no-truncation.v1", ok, { no_hidden_first_n_limit: ok, pagination }), null, 2)}\n`);
fs.writeFileSync(path.join(evidenceRoot, "provenance-audit.json"), `${JSON.stringify(reportFor("treatcode.public.provenance-audit.v1", ok, provenanceAudit), null, 2)}\n`);
console.log(`P15 public coverage: ${ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = ok ? 0 : 1;
