import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const apiRoot = path.join(appRoot, "public", "api", "v1");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P15");
const resources = ["projects", "stack_nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps"];
const checks = [];
const errors = [];

function assert(condition, message) { if (!condition) throw new Error(message); }
function read(name) { return JSON.parse(fs.readFileSync(path.join(apiRoot, name), "utf8")); }
function safePath(value) {
  const normalized = String(value || "").replaceAll("\\", "/");
  return normalized && !normalized.startsWith("/") && !normalized.includes("..") && !/(^|\/)(?:build|node_modules|coverage|scratch|\.git|\.codex|dist)(?:\/|$)/i.test(normalized) && !/(^|\/)(?:\.env(?:\.|$)|secrets?|credentials?|private)(?:\/|$)/i.test(normalized);
}
function walk(value, visit, pathParts = []) {
  visit(value, pathParts);
  if (Array.isArray(value)) value.forEach((item, index) => walk(item, visit, [...pathParts, String(index)]));
  else if (value && typeof value === "object") Object.entries(value).forEach(([key, item]) => walk(item, visit, [...pathParts, key]));
}

try {
  const snapshot = read("snapshot.json");
  const relationshipIndex = read("relationship-index.json");
  const ids = new Set(resources.flatMap((resource) => snapshot[resource].map((record) => record.id)));
  for (const resource of resources) for (const record of snapshot[resource]) {
    for (const reference of [...(record.source_refs || []), ...(record.evidence_refs || [])]) {
      assert(reference.repository === snapshot.snapshot.repository, `${record.id} provenance points outside the declared public repository`);
      assert(/^[0-9a-f]{7,64}$/i.test(String(reference.commit || "")), `${record.id} provenance has no immutable commit`);
      if (reference.path) assert(safePath(reference.path), `${record.id} exposes a private or generated path: ${reference.path}`);
    }
    walk(record, (value, pathParts) => {
      const key = pathParts.at(-1) || "";
      if (typeof value === "string" && /(?:password|access[_-]?key|bearer|token|secret|private[_-]?key)/i.test(key)) throw new Error(`${record.id} appears to contain credential-like field ${key}`);
    });
  }
  for (const relation of snapshot.relations || []) assert(ids.has(relation.from) && ids.has(relation.to), `public relationship exposes an unresolved endpoint: ${relation.from} -> ${relation.to}`);
  for (const edge of relationshipIndex.unresolved_external_edges || []) {
    assert(edge.required === false && edge.reason, "unresolved P03 edges must be explicitly non-public and explained");
    if (edge.source?.path) assert(safePath(edge.source.path), `excluded relationship exposes an unsafe source path: ${edge.source.path}`);
  }
  walk(snapshot, (value, pathParts) => {
    const key = pathParts.at(-1) || "";
    if (typeof value === "string" && /(href|route|url|repository)/i.test(key)) assert(!/^(?:javascript|data|vbscript):/i.test(value), `unsafe URL scheme in ${pathParts.join(".")}`);
  });
  const serverSource = fs.readFileSync(path.join(appRoot, "server.ts"), "utf8");
  const publicApiSource = serverSource.slice(serverSource.indexOf("function registerPublicApi"), serverSource.indexOf("// Register the workspace alias"));
  assert(publicApiSource && !/app\.(post|put|patch|delete)\s*\(/.test(publicApiSource), "public API registration contains a mutating HTTP handler");
  assert(!publicApiSource.includes("compileAndRunTrit") && !publicApiSource.includes("SecureExecutionQueue"), "public API registration imports runner behavior");
  const publicApp = fs.readFileSync(path.join(appRoot, "src", "PublicApp.tsx"), "utf8");
  assert(!publicApp.includes("compileAndRunTrit") && !publicApp.includes("STARTER_CODES"), "public browser app contains editor/runner bundle markers");
  checks.push("source and evidence provenance is immutable, repository-scoped, and credential-free");
  checks.push("all public relationship endpoints resolve; excluded P03 edges are explicitly non-public");
  checks.push("public API handlers are GET-only and public browser markers exclude editor/runner code");
} catch (error) {
  errors.push(String(error?.message || error));
}

fs.mkdirSync(evidenceRoot, { recursive: true });
const ok = errors.length === 0;
const report = { schema: "treatcode.public.security.v1", ok, checks, errors, public_routes_read_only: ok, credential_scan: ok };
fs.writeFileSync(path.join(evidenceRoot, "security.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(`P15 public security: ${ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = ok ? 0 : 1;
