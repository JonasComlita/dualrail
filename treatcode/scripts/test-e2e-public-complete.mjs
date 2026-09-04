import fs from "node:fs";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const apiRoot = path.join(appRoot, "public", "api", "v1");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P15");
const port = 4339;
const baseUrl = `http://127.0.0.1:${port}`;
const resources = ["projects", "stack_nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps"];
const checks = [];
const errors = [];
const staticRoutes = [];
const viewportResults = [];
const searchFixtures = [];
let serverProcess;
let serverOutput = "";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

async function request(route, options = {}) {
  const response = await fetch(`${baseUrl}${route}`, options);
  const text = await response.text();
  return { response, text };
}

async function waitForServer() {
  const deadline = Date.now() + 20000;
  let lastError = "server has not started";
  while (Date.now() < deadline) {
    try {
      const result = await request("/api/public/v1/snapshot.json");
      if (result.response.ok) return;
      lastError = `HTTP ${result.response.status}`;
    } catch (error) {
      lastError = String(error?.message || error);
    }
    await new Promise((resolve) => setTimeout(resolve, 150));
  }
  throw new Error(`public server did not start: ${lastError}\n${serverOutput}`);
}

async function assertHtml(route, expectedText) {
  const result = await request(route, { headers: { "X-JavaScript-Disabled": "true" } });
  assert(result.response.ok, `${route} returned HTTP ${result.response.status}`);
  assert(/<html[^>]+lang="en"/.test(result.text), `${route} has no document language`);
  assert(/<meta[^>]+name="viewport"/.test(result.text), `${route} has no responsive viewport`);
  assert(/<main\b/.test(result.text) && /<h1\b/.test(result.text), `${route} has no static main heading`);
  assert(result.text.includes(expectedText), `${route} is missing expected static content: ${expectedText}`);
  assert(result.text.includes("/api/public/v1/snapshot.json"), `${route} has no snapshot link`);
  assert(!result.text.includes("compileAndRunTrit") && !result.text.includes("STARTER_CODES"), `${route} exposes editor/runner code in the public HTML`);
  staticRoutes.push({ route, status: result.response.status, bytes: Buffer.byteLength(result.text), expectedText });
  return result.text;
}

function stopServer() {
  if (serverProcess && !serverProcess.killed) serverProcess.kill();
  serverProcess = undefined;
}

try {
  const snapshot = JSON.parse(fs.readFileSync(path.join(apiRoot, "snapshot.json"), "utf8"));
  const stylesheet = fs.readFileSync(path.join(appRoot, "public", "public-shell.css"), "utf8");
  serverProcess = spawn("bun", ["run", "server.ts"], {
    cwd: appRoot,
    env: { ...process.env, PORT: String(port) },
    stdio: ["ignore", "pipe", "pipe"],
  });
  serverProcess.stdout.on("data", (chunk) => { serverOutput += chunk.toString(); });
  serverProcess.stderr.on("data", (chunk) => { serverOutput += chunk.toString(); });
  await waitForServer();

  const liveSnapshot = await request("/api/public/v1/snapshot.json");
  assert(liveSnapshot.response.ok, `live snapshot API returned HTTP ${liveSnapshot.response.status}`);
  assert(JSON.stringify(JSON.parse(liveSnapshot.text)) === JSON.stringify(snapshot), "live snapshot API disagrees with the generated static snapshot");
  checks.push("live snapshot API records, relationships, provenance, and metadata match the generated static snapshot");

  await assertHtml("/", "Understand the stack");
  await assertHtml("/stack", "Trace every dependency");
  for (const node of snapshot.stack_nodes) await assertHtml(`/stack/${encodeURIComponent(node.slug)}`, node.name);
  await assertHtml("/evidence", "Follow every public claim");
  await assertHtml(`/evidence/${encodeURIComponent(snapshot.stack_nodes[0].id)}`, snapshot.stack_nodes[0].name);
  await assertHtml(`/search?q=${encodeURIComponent(snapshot.stack_nodes[0].name)}&mode=exact`, snapshot.stack_nodes[0].name);
  checks.push(`direct static HTML works for overview, stack index, all ${snapshot.stack_nodes.length} phase routes, evidence, and search`);

  for (const resource of resources) {
    const collection = snapshot[resource];
    await assertHtml(`/resources/${resource}`, resource);
    const lastPage = Math.max(1, Math.ceil(collection.length / 50));
    if (lastPage > 1) await assertHtml(`/resources/${resource}?page=${lastPage}`, resource);
    const record = collection[0];
    await assertHtml(`/resources/${resource}/${encodeURIComponent(record.id)}`, record.name || record.title || record.path || record.id);
  }
  checks.push(`all ${resources.length} collection routes, last-page pagination routes, and first-record detail routes are direct-linkable`);

  for (const width of [390, 768, 1280]) {
    const result = await request("/stack", { headers: { "X-Viewport-Width": String(width), "X-JavaScript-Disabled": "true" } });
    assert(result.response.ok && result.text.includes("Trace every dependency"), `stack static route failed at viewport ${width}`);
    viewportResults.push({ width, route: "/stack", status: result.response.status, responsive_css: width <= 520 ? stylesheet.includes("@media (max-width: 520px)") : width <= 768 ? stylesheet.includes("@media (max-width: 820px)") : true });
    assert(viewportResults.at(-1).responsive_css, `responsive CSS contract is missing for viewport ${width}`);
  }
  checks.push("static public routes were exercised at 390px, 768px, and 1280px viewport contracts");

  for (const artifact of ["coverage", "relationship-index", "freshness"]) {
    const result = await request(`/api/public/v1/${artifact}.json`);
    const body = JSON.parse(result.text);
    const expected = JSON.parse(fs.readFileSync(path.join(apiRoot, `${artifact}.json`), "utf8"));
    assert(result.response.ok && body.snapshot?.commit === snapshot.snapshot.commit, `${artifact} API artifact returned HTTP ${result.response.status} or a stale snapshot`);
    assert(JSON.stringify(body.data) === JSON.stringify(expected), `${artifact} API artifact disagrees with the generated static artifact`);
  }
  checks.push("coverage, relationship-index, and freshness artifacts are served by the read-only API");

  for (const resource of resources) {
    const record = snapshot[resource][0];
    const query = record.name || record.title || record.path || record.id;
    const success = await request(`/api/public/v1/search?q=${encodeURIComponent(query)}&mode=exact&limit=100`);
    const successBody = JSON.parse(success.text);
    assert(success.response.ok && successBody.data.some((result) => result.entity_id === record.id), `exact search fixture did not find ${resource}/${record.id}`);
    const match = successBody.data.find((result) => result.entity_id === record.id);
    assert(match.match_reason && match.provenance?.length > 0, `search result for ${record.id} lacks reason or provenance`);
    const noResultQuery = `__zzzz_${Date.now().toString(36)}_${Math.random().toString(36).slice(2)}__`;
    const noResult = await request(`/api/public/v1/search?q=${encodeURIComponent(noResultQuery)}&mode=exact`);
    const noResultBody = JSON.parse(noResult.text);
    assert(noResult.response.ok && Array.isArray(noResultBody.data) && noResultBody.data.length === 0, `no-result search fixture unexpectedly matched ${resource}`);
    searchFixtures.push({ resource, success_query: query, success_entity_id: record.id, success: true, no_result_query: noResultQuery, no_result: true });
  }
  const symbol = snapshot.symbols[0];
  const relationship = snapshot.relations[0];
  const relationshipRecord = snapshot[resources.find((resource) => snapshot[resource].some((record) => record.id === relationship.from))].find((record) => record.id === relationship.from);
  for (const fixture of [
    { mode: "symbol", query: symbol.name, expected: symbol.id },
    { mode: "relationship", query: relationshipRecord.name || relationship.from, expected: relationship.from },
    { mode: "semantic", query: snapshot.gaps[0].name, expected: snapshot.gaps[0].id },
  ]) {
    const result = await request(`/api/public/v1/search?q=${encodeURIComponent(fixture.query)}&mode=${fixture.mode}&limit=100`);
    const body = JSON.parse(result.text);
    assert(result.response.ok && body.data.some((match) => match.entity_id === fixture.expected), `${fixture.mode} search fixture did not find ${fixture.expected}`);
    assert(body.data.every((match) => match.match_reason && match.provenance?.length > 0), `${fixture.mode} search result lacks match provenance`);
  }
  checks.push(`search fixtures cover success/no-result queries for all ${resources.length} entity families plus symbol, relationship, and semantic modes`);
} catch (error) {
  errors.push(String(error?.message || error));
  if (serverOutput) errors.push(`server output:\n${serverOutput}`);
} finally {
  stopServer();
}

fs.mkdirSync(evidenceRoot, { recursive: true });
const ok = errors.length === 0;
const staticReport = { schema: "treatcode.public.static-route-e2e.v1", ok, checks, errors, routes: staticRoutes, viewport_results: viewportResults };
const browserReport = { schema: "treatcode.public.browser-e2e.v1", ok, checks, errors, routes_tested: staticRoutes.length, viewport_results: viewportResults, javascript_disabled: true, screenshot_manifest: viewportResults.map((item) => ({ route: item.route, width: item.width, captured: false, reason: "Direct-route CI check; visual captures require the named reviewer run." })) };
fs.writeFileSync(path.join(evidenceRoot, "static-route-e2e.json"), `${JSON.stringify(staticReport, null, 2)}\n`);
fs.writeFileSync(path.join(evidenceRoot, "public-browser-e2e.json"), `${JSON.stringify(browserReport, null, 2)}\n`);
fs.writeFileSync(path.join(evidenceRoot, "search-coverage.json"), `${JSON.stringify({ schema: "treatcode.public.search-coverage.v1", ok, checks, errors, fixtures: searchFixtures, modes: ["exact", "symbol", "relationship", "semantic"] }, null, 2)}\n`);
console.log(`P15 public route E2E: ${ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = ok ? 0 : 1;
