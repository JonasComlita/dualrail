import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-participant-journey");
const stateRoot = fs.mkdtempSync(path.join(os.tmpdir(), "treatcode-participant-journey-"));
const port = 4328;
const baseUrl = `http://127.0.0.1:${port}`;
const checks = [];
const errors = [];
let server;

function assert(value, message) { if (!value) throw new Error(message); }
async function get(pathname) {
  const response = await fetch(`${baseUrl}${pathname}`);
  const body = await response.text();
  return { response, body };
}
async function waitForServer() {
  const deadline = Date.now() + 20_000;
  while (Date.now() < deadline) {
    try {
      const result = await get("/api/problems");
      if (result.response.ok) return;
    } catch { /* retry */ }
    await new Promise((resolve) => setTimeout(resolve, 150));
  }
  throw new Error("participant journey server did not start");
}

try {
  server = spawn("bun", ["run", "server.ts"], {
    cwd: appRoot,
    env: {
      ...process.env,
      PORT: String(port),
      TI_DATA_ROOT: path.join(stateRoot, "ternary-lab"),
      TREATCODE_AUTH_AUDIT_PATH: path.join(stateRoot, "auth-audit.jsonl"),
      TREATCODE_AUTH_STATE_PATH: path.join(stateRoot, "auth-state.json"),
      TREATCODE_COMMUNITY_STATE_PATH: path.join(stateRoot, "community-state.json"),
    },
    stdio: "ignore",
  });
  await waitForServer();

  for (const [route, marker] of [["/", "TreatCode"], ["/stack", "Stack"], ["/learn", "Learn"], ["/practice", "Practice"], ["/arena", "Arena"], ["/account", "Account"], ["/intelligence", "Ternary Intelligence"]]) {
    const page = await get(route);
    assert(page.response.ok, `${route} returned HTTP ${page.response.status}`);
    assert(page.body.toLowerCase().includes(marker.toLowerCase()) || route === "/practice", `${route} did not return its participant shell`);
  }
  checks.push("participant can navigate the public home, stack, learn, practice, arena, and account pages");

  const search = await get("/api/public/v1/search?q=ternary&limit=5");
  assert(search.response.ok && search.body.includes("ternary"), "public search did not return a ternary result");
  const arena = await get("/api/benchmarks/p10");
  assert(arena.response.ok && arena.body.includes("p10"), "P10 implementation arena evidence is unavailable");
  const challenges = await get("/api/problems");
  assert(challenges.response.ok && challenges.body.includes("T001"), "practice challenge catalog is unavailable");
  checks.push("public search, learn content, P10 arena evidence, and the Trit challenge catalog are reachable");

  const discussions = await get("/api/community/v1/discussions?task_id=T001");
  assert(discussions.response.ok && discussions.body.includes("discussions"), "public practice discussions are unavailable");
  for (const retired of ["/api/intelligence/v1/benchmark", "/api/intelligence/v3/catalog", "/api/intelligence/v3.1/catalog"]) {
    const result = await get(retired);
    assert(result.response.status === 404, `${retired} is still served`);
  }
  const sources = await get("/api/public/v1/sources.json");
  assert(sources.response.ok, "public source catalog is unavailable");
  for (const removedPath of ["benchmarks/intelligence-", "treatcode/src/Intelligence", "treatcode/src/intelligence", "P14_intelligence_benchmark.md"]) {
    assert(!sources.body.includes(removedPath), `${removedPath} remains in the public source catalog`);
  }
  checks.push("practice discussions remain available and retired benchmark routes are absent");

  fs.mkdirSync(evidenceRoot, { recursive: true });
  fs.writeFileSync(path.join(evidenceRoot, "participant-journey.json"), `${JSON.stringify({ schema: "trit.treatcode_participant_journey.v1", ok: true, routes: ["/", "/stack", "/learn", "/practice", "/arena", "/account"], checks }, null, 2)}\n`);
} catch (error) {
  errors.push(String(error?.stack || error?.message || error));
  fs.mkdirSync(evidenceRoot, { recursive: true });
  fs.writeFileSync(path.join(evidenceRoot, "participant-journey.json"), `${JSON.stringify({ schema: "trit.treatcode_participant_journey.v1", ok: false, checks, errors }, null, 2)}\n`);
} finally {
  if (server && server.exitCode === null) {
    const closed = new Promise(resolve => server.once("close", resolve));
    server.kill(); await closed;
  }
  fs.rmSync(stateRoot, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
}

console.log(`Participant journey: ${errors.length ? "failed" : "passed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = errors.length ? 1 : 0;
