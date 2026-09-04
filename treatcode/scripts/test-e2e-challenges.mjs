import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P06");
const port = 4317;
const baseUrl = `http://127.0.0.1:${port}`;
const communityStateRoot = fs.mkdtempSync(path.join(os.tmpdir(), "treatcode-p06-community-"));
const communityStatePath = path.join(communityStateRoot, "state.json");
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
    env: { ...process.env, PORT: String(port), TREATCODE_AUTH_AUDIT_PATH: path.join(repoRoot, "build", "treatcode-plan-evidence", "P06", "challenge-e2e-audit.jsonl"), TREATCODE_COMMUNITY_STATE_PATH: communityStatePath },
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

  const postedCode = `import ulib;

fn sign_test(x: t40) -> t40 {
    match x {
        neg => { return -1; }
        zero => { return 0; }
        pos => { return 1; }
    }
}`;
  const prematurePost = await request("/api/community/v1/solutions", {
    method: "POST",
    headers: { "Content-Type": "application/json", Authorization: `Bearer ${token}`, "X-Action-Nonce": `p06-premature-${Date.now()}` },
    body: JSON.stringify({ challenge_id: "T001", title: "Premature post", code: postedCode, language: "trit", visibility: "public", explanation: "This must be verified first." }),
  });
  assert(prematurePost.response.status === 409 && prematurePost.body?.error?.code === "solution_not_verified", "public solution posting was not gated by accepted verification");
  const solved = await request("/api/submit", {
    method: "POST",
    headers: { "Content-Type": "application/json", Authorization: `Bearer ${token}` },
    body: JSON.stringify({ problemId: "T001", code: postedCode, engine: "native", optLevel: "-O2" }),
  });
  assert(solved.response.ok && solved.body.success === true, `posted solution verification returned HTTP ${solved.response.status}: ${JSON.stringify(solved.body)}`);
  assert(solved.body.submission?.metrics?.engine === "native", "accepted submission did not exercise the native engine");
  assert(typeof solved.body.submission?.metrics?.runtime_ms === "number", "accepted submission did not persist measured runtime");
  assert(typeof solved.body.submission?.metrics?.memory_kib === "number", "accepted submission did not persist measured memory");
  checks.push("native compiler driver verifies all five sign-test cases and persists native execution metrics");

  const posted = await request("/api/community/v1/solutions", {
    method: "POST",
    headers: { "Content-Type": "application/json", Authorization: `Bearer ${token}`, "X-Action-Nonce": `p06-post-${Date.now()}` },
    body: JSON.stringify({ challenge_id: "T001", title: "A public sign solution", code: postedCode, language: "trit", visibility: "public", explanation: "Match the balanced ternary sign and return the corresponding answer.", pseudocode: "match input sign, then return its mapped value" }),
  });
  assert(posted.response.status === 201, `public solution post returned HTTP ${posted.response.status}`);
  const postedSolutionId = posted.body?.data?.solution?.id || posted.body?.solution?.id;
  assert(typeof postedSolutionId === "string" && postedSolutionId.length > 0, "public solution post did not return a solution id");
  const privateDraft = await request("/api/intelligence/v1/solutions", {
    method: "POST",
    headers: { "Content-Type": "application/json", Authorization: `Bearer ${token}`, "X-Action-Nonce": `p06-private-${Date.now()}` },
    body: JSON.stringify({ task_id: "T001", title: "A private draft", code: "private benchmark draft", language: "trit" }),
  });
  assert(privateDraft.response.status === 201, `private solution draft returned HTTP ${privateDraft.response.status}`);
  const publicFeedAfterSubmit = await request("/api/community/v1/solutions?challenge_id=T001");
  const publicSolutionsAfterSubmit = publicFeedAfterSubmit.body?.data?.solutions || publicFeedAfterSubmit.body?.solutions || [];
  assert(publicFeedAfterSubmit.response.ok && publicSolutionsAfterSubmit.length === 1, "public solution feed must exclude private drafts and stay problem-scoped");
  assert(publicSolutionsAfterSubmit[0].code === postedCode && publicSolutionsAfterSubmit[0].solved === true, "accepted submission did not mark the exact posted solution as solved");
  assert(typeof publicSolutionsAfterSubmit[0].metrics?.runtime_ms === "number" && typeof publicSolutionsAfterSubmit[0].metrics?.memory_kib === "number", "public solution feed did not include measured code metrics");
  assert(publicSolutionsAfterSubmit[0].discussion.includes("Plain-English explanation"), "public solution feed did not include the linked explanation");
  assert(!JSON.stringify(publicSolutionsAfterSubmit).includes("owner_identity_id"), "public solution feed leaked an internal owner id");
  checks.push("public solution posting is verification-gated; verified posts combine public code, explanation, metrics, and solved state while private drafts remain hidden");

  const vote = await request(`/api/community/v1/solutions/${encodeURIComponent(postedSolutionId)}/vote`, {
    method: "POST",
    headers: { "Content-Type": "application/json", Authorization: `Bearer ${token}`, "X-Action-Nonce": `p06-vote-${Date.now()}` },
    body: JSON.stringify({}),
  });
  assert(vote.response.ok && vote.body.upvoted === true && vote.body.upvotes === 1, "authenticated solution vote was not persisted");
  const rankedFeed = await request("/api/community/v1/solutions?challenge_id=T001");
  const rankedSolutions = rankedFeed.body?.data?.solutions || rankedFeed.body?.solutions || [];
  assert(rankedSolutions[0]?.upvotes === 1 && rankedSolutions[0]?.viewer_has_upvoted === false, "public solution feed did not expose the durable vote count without leaking viewer state");
  const authenticatedFeed = await request("/api/community/v1/solutions?challenge_id=T001", { headers: { Authorization: `Bearer ${token}` } });
  const authenticatedSolutions = authenticatedFeed.body?.data?.solutions || authenticatedFeed.body?.solutions || [];
  assert(authenticatedSolutions[0]?.viewer_has_upvoted === true, "authenticated public feed did not restore the viewer vote state");
  checks.push("community solution votes persist and feed ranking exposes approval counts");

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
  fs.rmSync(communityStateRoot, { recursive: true, force: true });
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
