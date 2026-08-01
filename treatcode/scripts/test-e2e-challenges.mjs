import fs from "node:fs";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P06");
const port = 4317;
const baseUrl = `http://127.0.0.1:${port}`;
const checks = [];
const errors = [];
let serverProcess;

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

async function request(pathname, options = {}) {
  const response = await fetch(`${baseUrl}${pathname}`, options);
  const body = await response.json();
  return { response, body };
}

async function waitForServer() {
  const deadline = Date.now() + 15000;
  let lastError = "server has not started";
  while (Date.now() < deadline) {
    try {
      const result = await request("/api/problems");
      if (result.response.ok) return;
      lastError = `HTTP ${result.response.status}`;
    } catch (error) {
      lastError = String(error?.message || error);
    }
    await new Promise((resolve) => setTimeout(resolve, 150));
  }
  throw new Error(`challenge server did not start: ${lastError}`);
}

try {
  serverProcess = spawn("bun", ["run", "server.ts"], {
    cwd: appRoot,
    env: { ...process.env, PORT: String(port), TREATCODE_AUTH_AUDIT_PATH: path.join(repoRoot, "build", "treatcode-plan-evidence", "P06", "challenge-e2e-audit.jsonl") },
    stdio: ["ignore", "pipe", "pipe"],
  });
  let serverOutput = "";
  serverProcess.stdout.on("data", (chunk) => { serverOutput += chunk.toString(); });
  serverProcess.stderr.on("data", (chunk) => { serverOutput += chunk.toString(); });
  await waitForServer();

  const catalog = await request("/api/problems");
  assert(catalog.response.ok, `challenge catalog returned HTTP ${catalog.response.status}`);
  assert(Array.isArray(catalog.body) && catalog.body.length === 58, "active challenge catalog should contain 58 non-retired entries");
  assert(!catalog.body.some((challenge) => challenge.id === "T059"), "retired challenge is visible in the active API");
  assert(catalog.body.some((challenge) => challenge.id === "T057" && challenge.lifecycle === "published"), "word-parallel pilot is missing from API");
  assert(catalog.body.some((challenge) => challenge.id === "T058" && challenge.lifecycle === "published"), "vector pilot is missing from API");
  assert(catalog.body.every((challenge) => !challenge.execution.correctness), "server exposed hidden correctness fixtures in the client catalog");
  assert(catalog.body.every((challenge) => challenge.facets && Object.keys(challenge.facets).length === 6), "API challenge facets are incomplete");
  checks.push("API exposes generated active catalog with six facets and no hidden fixtures");

  const login = await request("/api/auth/v1/login", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ identity_id: "tc:identity:demo-human", access_key: "local-human-key" }),
  });
  assert(login.response.ok, `demo login returned HTTP ${login.response.status}`);
  const token = login.body?.data?.credential?.token;
  assert(typeof token === "string" && token.length > 20, "login did not issue a session token");

  const before = await request("/api/leaderboard");
  const draftSubmission = await request("/api/submit", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Authorization: `Bearer ${token}`,
      "X-Action-Nonce": `p06-draft-${Date.now()}`,
    },
    body: JSON.stringify({ problemId: "T003", code: "import ulib; fn main() -> t40 { return 0; }", engine: "native", username: "p06-e2e" }),
  });
  assert(draftSubmission.response.status === 409, `draft submission returned HTTP ${draftSubmission.response.status}, expected 409`);
  assert(draftSubmission.body.code === "challenge_not_published", "draft submission did not return structured lifecycle denial");
  const after = await request("/api/leaderboard");
  assert(JSON.stringify(before.body) === JSON.stringify(after.body), "draft submission changed leaderboard state");
  assert(!after.body.some((entry) => entry.problemId === "T003"), "draft challenge entered leaderboard");
  checks.push("draft submission is denied before execution and cannot create leaderboard evidence");

  const retiredRun = await request("/api/run", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Authorization: `Bearer ${token}`,
      "X-Action-Nonce": `p06-retired-${Date.now()}`,
    },
    body: JSON.stringify({ problemId: "T059", code: "", engine: "native" }),
  });
  assert(retiredRun.response.status === 409, `retired run returned HTTP ${retiredRun.response.status}, expected 409`);
  checks.push("retired challenge is denied at the execution boundary");
} catch (error) {
  errors.push(String(error?.message || error));
  if (serverProcess) {
    errors.push(`server output:\n${serverProcess.stdout ? "" : ""}`);
  }
} finally {
  if (serverProcess && !serverProcess.killed) serverProcess.kill();
}

const report = {
  schema: "trit.treatcode_challenge_e2e.v1",
  ok: errors.length === 0,
  checks,
  errors,
};
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "challenge-e2e.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(`P06 challenge E2E: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
