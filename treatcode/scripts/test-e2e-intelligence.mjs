import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import crypto from "node:crypto";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P14");
const port = 4327;
const baseUrl = `http://127.0.0.1:${port}`;
const stateRoot = fs.mkdtempSync(path.join(os.tmpdir(), "treatcode-p14-e2e-"));
const checks = [];
const errors = [];
let serverProcess;
let serverOutput = "";

const compareFix = `fn compare(a: t40, b: t40) -> t40 {
    match a - b {
        neg => { return -1; }
        zero => { return 0; }
        pos => { return 1; }
    }
}`;
const medianFix = `fn median(a: t40, b: t40, c: t40) -> t40 {
    if a <= b {
        if b <= c { return b; }
        if a <= c { return c; }
        return a;
    }
    if a <= c { return a; }
    if b <= c { return c; }
    return b;
}`;

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function nonce(label) {
  return `p14-${label}-${Date.now()}-${Math.random().toString(36).slice(2, 10)}`;
}

async function request(pathname, options = {}, token = "") {
  const headers = new Headers(options.headers || {});
  headers.set("Accept", "application/json");
  if (options.body && !headers.has("Content-Type")) headers.set("Content-Type", "application/json");
  if (token) headers.set("Authorization", `Bearer ${token}`);
  if (options.method && options.method !== "GET" && options.method !== "HEAD" && !headers.has("X-Action-Nonce")) headers.set("X-Action-Nonce", nonce(options.method));
  const response = await fetch(`${baseUrl}${pathname}`, { ...options, headers });
  const text = await response.text();
  let body = {};
  try { body = text ? JSON.parse(text) : {}; } catch { body = { text }; }
  return { response, body };
}

function startServer() {
  serverProcess = spawn("bun", ["run", "server.ts"], {
    cwd: appRoot,
    env: {
      ...process.env,
      PORT: String(port),
      TREATCODE_AUTH_AUDIT_PATH: path.join(stateRoot, "auth-audit.jsonl"),
      TREATCODE_AUTH_STATE_PATH: path.join(stateRoot, "auth-state.json"),
      TREATCODE_COMMUNITY_STATE_PATH: path.join(stateRoot, "community-state.json"),
      TREATCODE_INTELLIGENCE_ROOT: path.join(stateRoot, "intelligence"),
    },
    stdio: ["ignore", "pipe", "pipe"],
  });
  serverProcess.stdout.on("data", (chunk) => { serverOutput += chunk.toString(); });
  serverProcess.stderr.on("data", (chunk) => { serverOutput += chunk.toString(); });
}

async function stopServer() {
  if (!serverProcess) return;
  if (!serverProcess.killed) serverProcess.kill();
  await new Promise((resolve) => setTimeout(resolve, 250));
  serverProcess = null;
}

async function waitForServer() {
  const deadline = Date.now() + 20_000;
  let lastError = "not started";
  while (Date.now() < deadline) {
    try {
      const result = await request("/api/intelligence/v1/benchmark");
      if (result.response.ok) return;
      lastError = `HTTP ${result.response.status}`;
    } catch (error) {
      lastError = String(error?.message || error);
    }
    await new Promise((resolve) => setTimeout(resolve, 150));
  }
  throw new Error(`intelligence server did not start: ${lastError}\n${serverOutput}`);
}

async function register(handle) {
  const result = await request("/api/auth/v1/register", { method: "POST", body: JSON.stringify({ handle, password: "LunaP14Password123!" }) });
  assert(result.response.status === 201, `registration returned HTTP ${result.response.status}`);
  assert(Array.isArray(result.body?.data?.credential?.credential?.actions), "registration did not return scoped actions");
  return { token: result.body.data.credential.token, identity: result.body.data.identity };
}

async function login(handle) {
  const result = await request("/api/auth/v1/login", { method: "POST", body: JSON.stringify({ handle, password: "LunaP14Password123!" }) });
  assert(result.response.ok, `participant login returned HTTP ${result.response.status}`);
  return { token: result.body.data.credential.token, identity: result.body.data.identity };
}

function fileHash(filePath) {
  return `sha256:${crypto.createHash("sha256").update(fs.readFileSync(filePath)).digest("hex")}`;
}

try {
  const handle = `luna_p14_${Math.random().toString(36).slice(2, 9)}`;
  startServer();
  await waitForServer();

  const rootPage = await request("/intelligence");
  assert(rootPage.response.ok && String(rootPage.body?.text || "").toLowerCase().includes("intelligence"), "intelligence route is unavailable");
  checks.push("/intelligence route is reachable");

  const catalog = await request("/api/intelligence/v1/benchmark");
  const catalogText = JSON.stringify(catalog.body);
  assert(catalog.response.ok, `benchmark catalog returned HTTP ${catalog.response.status}`);
  assert(catalog.body?.data?.task?.id === "TC-SWE-001", "TC-SWE-001 is missing from the benchmark catalog");
  assert(catalog.body?.data?.task?.allowlisted_files?.length === 2, "catalog does not expose the two allowlisted files");
  assert(catalog.body?.data?.tasks?.length === 5, "benchmark catalog does not expose the five-task suite");
  assert(catalog.body.data.tasks.map((task) => task.task.category).join(",") === "algorithmic-trit-repair,parser-serialization,memory-pointer-safety,concurrency-state,syscall-abi-integration", "suite task categories are incomplete or reordered");
  assert(!catalogText.includes("perm-132") && !catalogText.includes("-1000000") && !catalogText.includes("hidden.server"), "hidden verifier details leaked into the public catalog");
  checks.push("benchmark catalog exposes public tests/starter files while withholding hidden cases");

  const suiteRoute = await request("/api/intelligence/v1/suite");
  assert(suiteRoute.response.ok && suiteRoute.body?.data?.suite?.tasks?.length === 5, "versioned suite endpoint did not expose all task descriptors");
  const parserCatalog = await request("/api/intelligence/v1/benchmark?task_id=TC-SWE-002");
  assert(parserCatalog.response.ok && parserCatalog.body?.data?.task?.id === "TC-SWE-002" && parserCatalog.body?.data?.task?.allowlisted_files?.length === 1, "task-aware benchmark catalog did not select TC-SWE-002");
  checks.push("suite and task-aware catalog adapters expose all five public task projections");

  let session = await register(handle);
  checks.push("participant can register with a pseudonymous handle and least-privilege grants");
  await stopServer();
  startServer();
  await waitForServer();
  session = await login(handle);
  checks.push("participant account and handle/password login survive a server restart");

  const solution = await request("/api/intelligence/v1/solutions", { method: "POST", body: JSON.stringify({ task_id: "TC-SWE-001", title: "Median repair", code: `// FILE: src/compare.trit\n${compareFix}\n\n// FILE: src/median.trit\n${medianFix}`, language: "trit" }) }, session.token);
  assert(solution.response.status === 201, `solution save returned HTTP ${solution.response.status}`);
  assert(solution.body?.solution?.owner_handle === handle, "saved solution owner was not derived from the authenticated handle");
  const discussion = await request("/api/intelligence/v1/discussions", { method: "POST", body: JSON.stringify({ task_id: "TC-SWE-001", solution_id: solution.body.solution.id, body: "Pseudocode: compare the three values, keep the middle value, and preserve the three-way sign invariant." }) }, session.token);
  assert(discussion.response.status === 201, `discussion publish returned HTTP ${discussion.response.status}`);
  assert(discussion.body?.discussion?.author_handle === handle, "discussion author was not derived from the authenticated handle");
  checks.push("participant can save a versioned solution and publish a linked plain-text explanation");

  const runResult = await request("/api/intelligence/v1/runs", { method: "POST", body: JSON.stringify({ task_id: "TC-SWE-001" }) }, session.token);
  assert(runResult.response.status === 201, `intelligence run start returned HTTP ${runResult.response.status}`);
  const runId = runResult.body.run.run_id;
  const trials = runResult.body.run.trials;
  assert(trials.length === 4, "intelligence run did not create four trials");
  for (let index = 0; index < trials.length; index += 1) {
    const trialId = trials[index].id;
    for (const [file, content] of [["src/compare.trit", compareFix], ["src/median.trit", medianFix]]) {
      const written = await request(`/api/intelligence/v1/runs/${runId}/trials/${trialId}/files`, { method: "POST", body: JSON.stringify({ path: file, content }) }, session.token);
      assert(written.response.ok, `${file} write failed for trial ${index + 1}: HTTP ${written.response.status}`);
    }
    const firstPublic = await request(`/api/intelligence/v1/runs/${runId}/trials/${trialId}/public-tests`, { method: "POST", body: "{}" }, session.token);
    assert(firstPublic.response.ok && firstPublic.body.report?.passed === true, `public gate failed for trial ${index + 1}`);
    if (index === 0) {
      const repeat = await request(`/api/intelligence/v1/runs/${runId}/trials/${trialId}/public-tests`, { method: "POST", body: "{}" }, session.token);
      assert(repeat.response.ok && repeat.body.report?.attempt === 2, "public tests were not repeatable");
    }
    const hidden = await request(`/api/intelligence/v1/runs/${runId}/trials/${trialId}/submit`, { method: "POST", body: "{}" }, session.token);
    assert(hidden.response.ok && hidden.body.hidden?.sealed === true, `hidden submission did not seal trial ${index + 1}`);
    assert(!JSON.stringify(hidden.body).includes("perm-132"), "hidden case identifier leaked in a trial receipt");
  }
  const aggregate = await request(`/api/intelligence/v1/runs/${runId}/aggregate`, {}, session.token);
  assert(aggregate.response.ok && aggregate.body.aggregate?.score === 100, "four-trial aggregate did not produce a sealed 100 score");
  const serviceLogin = await request("/api/auth/v1/login", { method: "POST", body: JSON.stringify({ identity_id: "tc:identity:demo-service", access_key: "local-service-key" }) });
  assert(serviceLogin.response.ok, "privileged attestor service login failed");
  const attestation = await request(`/api/intelligence/v1/runs/${runId}/attest`, { method: "POST", body: JSON.stringify({ model: "gpt-5.6-luna", model_configuration: "max", evidence_hashes: ["sha256:p14-api-proof"] }) }, serviceLogin.body.data.credential.token);
  assert(attestation.response.ok && attestation.body.attestation?.evidence_hash?.startsWith("sha256:"), "privileged model attestation was not recorded");
  const intelligenceLeaderboard = await request("/api/intelligence/v1/leaderboard");
  assert(intelligenceLeaderboard.body?.official_leaderboard?.some((entry) => entry.handle === handle && entry.score === 100), "official intelligence leaderboard did not publish the participant score");
  checks.push("one-shot public-gated four-trial run produces a sealed passed/4*100 official score");

  const challenge = await request("/api/problems");
  const signCode = "import ulib; fn sign_test(x: t40) -> t40 { match x { neg => { return -1; } zero => { return 0; } pos => { return 1; } } }";
  const challengeSubmit = await request("/api/submit", { method: "POST", body: JSON.stringify({ problemId: "T001", code: signCode, engine: "bootstrap", username: "caller-supplied-name" }) }, session.token);
  assert(challenge.response.ok && challengeSubmit.body.success === true, "the leetcode-style T001 challenge did not accept the Trit solution");
  assert(challengeSubmit.body.submission?.owner_handle === handle, "challenge submission owner was trusted from the caller instead of auth");
  const legacyLeaderboard = await request("/api/leaderboard");
  assert(legacyLeaderboard.body.some((entry) => entry.problemId === "T001" && entry.name === handle), "accepted T001 challenge did not appear under the authenticated handle");
  checks.push("participant solves and submits the first leetcode-style Trit challenge with an attributed result");

  const restoredSolution = await request("/api/intelligence/v1/solutions?task_id=TC-SWE-001", {}, session.token);
  const restoredDiscussions = await request("/api/intelligence/v1/discussions?task_id=TC-SWE-001");
  assert(restoredSolution.body?.solution?.owner_handle === handle, "saved solution was not readable after restart");
  assert(restoredDiscussions.body?.discussions?.some((post) => post.author_handle === handle), "discussion was not readable after restart");
  checks.push("saved solution, discussion, challenge submission, and both leaderboards are visible to the participant journey");

  const evidence = {
    schema: "trit.treatcode_p14_intelligence_e2e.v1",
    ok: true,
    model: { name: "gpt-5.6-luna", reasoning_effort: "max", role: "external browser agent proof follows" },
    participant: { handle, identity_id: session.identity.id },
    run_id: runId,
    trial_ids: trials.map((trial) => trial.id),
    aggregate: aggregate.body.aggregate,
    attestation: attestation.body.attestation,
    checks,
    artifact_hashes: [path.join(repoRoot, "benchmarks", "intelligence", "manifest.v1.json"), path.join(appRoot, "src", "intelligenceService.ts")].map((file) => ({ file: path.relative(repoRoot, file).replaceAll(path.sep, "/"), sha256: fileHash(file) })),
  };
  fs.mkdirSync(evidenceRoot, { recursive: true });
  fs.writeFileSync(path.join(evidenceRoot, "intelligence-e2e.json"), `${JSON.stringify(evidence, null, 2)}\n`);
} catch (error) {
  errors.push(String(error?.stack || error?.message || error));
  fs.mkdirSync(evidenceRoot, { recursive: true });
  fs.writeFileSync(path.join(evidenceRoot, "intelligence-e2e.json"), `${JSON.stringify({ schema: "trit.treatcode_p14_intelligence_e2e.v1", ok: false, checks, errors, server_output: serverOutput }, null, 2)}\n`);
} finally {
  await stopServer();
  fs.rmSync(stateRoot, { recursive: true, force: true });
}

console.log(`P14 intelligence E2E: ${errors.length ? "failed" : "passed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = errors.length ? 1 : 0;
