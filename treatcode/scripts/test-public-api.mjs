import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const apiRoot = path.join(appRoot, "public", "api", "v1");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P04");
const errors = [];
const checks = [];

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function readJson(name) {
  return JSON.parse(fs.readFileSync(path.join(apiRoot, name), "utf8"));
}

try {
  const snapshot = readJson("snapshot.json");
  const openapi = readJson("openapi.json");
  const stackManifest = JSON.parse(fs.readFileSync(path.join(repoRoot, "STACK_MANIFEST.json"), "utf8"));
  const expectedStackCount = Array.isArray(stackManifest.layers) ? stackManifest.layers.length : 0;
  assert(snapshot.schema_version === "treatcode.public.snapshot.v1", "snapshot schema version is incorrect");
  assert(/^[0-9a-f]{7,64}$/.test(snapshot.snapshot.commit), "snapshot does not carry a commit provenance value");
  assert(snapshot.snapshot.repository.startsWith("https://"), "snapshot repository provenance is not a URL");
  assert(expectedStackCount > 0 && snapshot.stack_nodes.length === expectedStackCount, "snapshot stack phase count must match STACK_MANIFEST.json");
  for (const resource of ["projects", "stack_nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps"]) {
    assert(Array.isArray(snapshot[resource]) && snapshot[resource].length > 0, `${resource} collection is empty`);
    for (const entity of snapshot[resource]) {
      assert(typeof entity.id === "string" && entity.id.startsWith("tc:"), `${resource} contains an entity without a stable id`);
      assert(Array.isArray(entity.source_refs) && entity.source_refs.length > 0, `${entity.id} has no source provenance`);
      for (const reference of entity.source_refs) assert(/^[0-9a-f]{7,64}$/.test(reference.commit), `${entity.id} has an invalid source commit`);
    }
  }
  const ids = new Set();
  for (const resource of ["projects", "stack_nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps"]) for (const entity of snapshot[resource]) ids.add(entity.id);
  for (const relation of snapshot.relations) assert(ids.has(relation.from) && ids.has(relation.to), `relation has an unresolved endpoint: ${relation.from} -> ${relation.to}`);
  const stackIds = new Set(snapshot.stack_nodes.map((node) => node.id));
  for (const node of snapshot.stack_nodes) for (const dependency of node.depends_on || []) assert(stackIds.has(dependency), `${node.id} has an unresolved dependency`);
  checks.push("snapshot schema, commit provenance, and stable entity ids");
  checks.push("projects, stack, components, evidence, and search collections are populated");
  checks.push("all relationship endpoints resolve to public entities");

  assert(openapi.openapi === "3.1.0", "OpenAPI version is not 3.1.0");
  assert(openapi["x-treatcode-schema-version"] === undefined || openapi["x-treatcode-schema-version"] === "treatcode.public.api.v1", "OpenAPI schema extension is incorrect");
  for (const requiredPath of ["/", "/snapshot.json", "/openapi.json", "/coverage.json", "/relationship-index.json", "/freshness.json", "/projects", "/stack-nodes", "/sources", "/symbols", "/search", "/{resource}/{id}", "/{resource}/{id}/relations"]) assert(requiredPath in openapi.paths, `OpenAPI is missing ${requiredPath}`);
  assert(openapi.components.schemas.Provenance && openapi.components.schemas.SnapshotInfo && openapi.components.schemas.SearchEnvelope, "OpenAPI provenance schemas are incomplete");
  checks.push("OpenAPI v1 paths and provenance schemas");

  const staticResources = ["projects", "stack_nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps"];
  for (const resource of staticResources) {
    const envelope = readJson(`${resource}.json`);
    assert(envelope.schema_version === "treatcode.public.api.v1", `${resource}.json is not an API envelope`);
    assert(envelope.snapshot.commit === snapshot.snapshot.commit, `${resource}.json has a different snapshot commit`);
  }
  checks.push("generated static resource envelopes retain the same snapshot provenance");
} catch (error) {
  errors.push(String(error?.message || error));
}

const report = {
  schema: "treatcode.public_api_conformance.v1",
  ok: errors.length === 0,
  snapshot: "treatcode/public/api/v1/snapshot.json",
  checks,
  errors,
};
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "api-conformance.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(`P04 API conformance: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
