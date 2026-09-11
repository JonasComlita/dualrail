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

const consensusFixes = {
  "src/validation.trit": `fn is_missing(reading: t40) -> t40 {
    if reading == -1000 { return 1; }
    return 0;
}
fn is_valid_reading(reading: t40) -> t40 {
    if reading == -1000 { return 1; }
    if reading < -729 { return 0; }
    if reading > 729 { return 0; }
    return 1;
}`,
  "src/compare.trit": `fn compare(a: t40, b: t40) -> t40 {
    if a < b { return -1; }
    if a > b { return 1; }
    return 0;
}`,
  "src/median.trit": `fn median3(a: t40, b: t40, c: t40) -> t40 {
    if a <= b {
        if b <= c { return b; }
        if a <= c { return c; }
        return a;
    }
    if a <= c { return a; }
    if b <= c { return c; }
    return b;
}`,
  "src/consensus.trit": `fn consensus(a: t40, b: t40, c: t40) -> t40 {
    if is_valid_reading(a) == 0 { return -1001; }
    if is_valid_reading(b) == 0 { return -1001; }
    if is_valid_reading(c) == 0 { return -1001; }
    var ma: t40 = is_missing(a);
    var mb: t40 = is_missing(b);
    var mc: t40 = is_missing(c);
    var missing: t40 = ma + mb + mc;
    if missing == 3 { return -1000; }
    if missing == 2 {
        if ma == 0 { return a; }
        if mb == 0 { return b; }
        return c;
    }
    if missing == 1 {
        if ma == 1 { return (b + c) / 2; }
        if mb == 1 { return (a + c) / 2; }
        return (a + b) / 2;
    }
    return median3(a, b, c);
}`,
};

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
  assert(catalog.body?.data?.task?.allowlisted_files?.length === 4, "catalog does not expose the four allowlisted files");
  assert(catalog.body?.data?.task?.difficulty === "hard" && catalog.body?.data?.task?.repository_shape?.hidden_cases === 28, "catalog does not expose diagnostic difficulty/repository-shape metadata");
  assert(catalog.body?.data?.tasks?.length === 5, "benchmark catalog does not expose the five-task suite");
  assert(catalog.body.data.tasks.map((task) => task.task.category).join(",") === "algorithmic-trit-repair,parser-serialization,memory-pointer-safety,concurrency-state,syscall-abi-integration", "suite task categories are incomplete or reordered");
  assert(catalog.body.data.tasks.every((task) => task.task.runner_ready === true), "published suite tasks are not marked runner-ready");
  assert(catalog.body.data.suite?.external_reference?.model?.score === 67, "external DeepSWE calibration reference is missing");
  assert(Array.isArray(catalog.body.data.model_suite_leaderboard), "catalog does not expose the provenance-gated model suite leaderboard");
  assert(!catalogText.includes("three-neg-mixed") && !catalogText.includes("missing-edge-average") && !catalogText.includes("hidden.server"), "hidden verifier details leaked into the public catalog");
  checks.push("benchmark catalog exposes public tests/starter files while withholding hidden cases");

  const v3Catalog = await request("/api/intelligence/v3/catalog");
  const v3CatalogText = JSON.stringify(v3Catalog.body);
  assert(v3Catalog.response.ok && v3Catalog.body?.data?.catalog?.corpus?.task_count === 100, "v3 catalog does not expose the 100-task authoring registry");
  assert(v3Catalog.body.data.catalog.official === false, "unreviewed v3 candidate corpus was presented as official");
  assert(v3Catalog.body.data.catalog.corpus.readiness?.required === 100 && v3Catalog.body.data.catalog.corpus.readiness?.authored === 0, "v3 readiness gaps are not explicit");
  assert(!v3CatalogText.includes("author_id") && !v3CatalogText.includes("grader_bundle_hash") && !v3CatalogText.includes("cohort_pass_rates"), "private v3 provenance, package, or calibration fields leaked through the public catalog");
  const v3Protocol = await request("/api/intelligence/v3/protocol");
  assert(v3Protocol.response.ok && v3Protocol.body?.data?.protocol?.official_requirements?.minimum_distinct_tasks === 100, "v3 protocol endpoint is unavailable or weakened");
  assert(v3Protocol.body.data.protocol.official_requirements.attempts_per_task === 1, "v3 protocol permits repeated attempts to masquerade as independent tasks");
  const v3DiscussionRubric = await request("/api/intelligence/v3/discussion-rubric");
  assert(v3DiscussionRubric.response.ok && v3DiscussionRubric.body?.data?.rubric?.rubric_id === "treatcode-discussion-v3", "v3 secondary discussion rubric endpoint is unavailable");
  assert(v3DiscussionRubric.body.data.rubric.included_in_executable_score === false && v3DiscussionRubric.body.data.rubric.length.maximum_words === 400, "v3 discussion rubric can inflate the executable score or is not length controlled");
  assert(v3DiscussionRubric.body.data.rubric.status === "draft_pending_human_calibration", "uncalibrated discussion rubric was presented as calibrated");
  checks.push("v3 public catalog exposes 100 non-official briefs and readiness gaps without private evaluator material");

  const v31Catalog = await request("/api/intelligence/v3.1/catalog");
  const v31CatalogText = JSON.stringify(v31Catalog.body);
  assert(v31Catalog.response.ok && v31Catalog.body?.data?.catalog?.version === "3.1", "v3.1 catalog endpoint is unavailable");
  assert(v31Catalog.body.data.catalog.official === false, "unfinished v3.1 development evidence was presented as official");
  assert(v31Catalog.body.data.catalog.phases?.pilot?.task_count === 30 && v31Catalog.body.data.catalog.phases.pilot.qualified_then_disposed === 30, "v3.1 pilot disposal status is inaccurate");
  assert(v31Catalog.body.data.catalog.final_contract?.task_count === 100 && v31Catalog.body.data.catalog.phases?.frozen?.task_count === 0, "v3.1 final holdout readiness is inaccurate");
  assert(Array.isArray(v31Catalog.body.data.catalog.tracks) && v31Catalog.body.data.catalog.tracks.some((track) => track.id === "fresh_coding") && v31Catalog.body.data.catalog.tracks.some((track) => track.id === "terminal_agent") && v31Catalog.body.data.catalog.tracks.some((track) => track.id === "frontier_math") && v31Catalog.body.data.catalog.dimension_observation_schema === "score-observation.schema.v3.1.json", "v3.1 track portfolio or independent observation contract is missing from the catalog");
  assert(v31Catalog.body.data.catalog.score_dimensions?.no_iq_composite === true && v31Catalog.body.data.catalog.score_dimensions?.discussion_in_executable_score === false, "v3.1 catalog does not keep score dimensions separate");
  assert(v31Catalog.body.data.catalog.blockers?.some((item) => item.includes("independent authorship")) && v31Catalog.body.data.catalog.blockers.some((item) => item.includes("held-out 100-task")), "v3.1 publication blockers are incomplete");
  assert(!v31CatalogText.includes("grader_bundle_hash") && !v31CatalogText.includes("stdout") && !v31CatalogText.includes("stderr"), "private v3.1 grader or command evidence leaked through the public catalog");
  const v31Protocol = await request("/api/intelligence/v3.1/protocol");
  assert(v31Protocol.response.ok && v31Protocol.body?.data?.protocol?.execution_modes?.join(",") === "scalar,repository,terminal", "v3.1 protocol does not preserve scalar, repository, and terminal modes");
  assert(v31Protocol.body.data.protocol.tracks_manifest === "tracks.v3.1.json" && v31Protocol.body.data.protocol.dimension_observation_schema === "score-observation.schema.v3.1.json" && v31Protocol.body.data.protocol.score_dimensions?.separate?.join(",") === "correctness,robustness,latency,resource_use,tool_execution,discussion_quality", "v3.1 protocol track manifest or score-dimension policy is unavailable");
  assert(v31Protocol.body.data.protocol.replication_target?.score_band?.minimum === 60 && v31Protocol.body.data.protocol.replication_target?.score_band?.maximum === 75, "v3.1 subject score band changed");
  assert(v31Protocol.body.data.protocol.replication_target?.sol_lead_tasks?.minimum === 1 && v31Protocol.body.data.protocol.replication_target?.sol_lead_tasks?.maximum === 4 && v31Protocol.body.data.protocol.replication_target?.exact_69_67_required === false, "v3.1 directional comparison target was weakened or fitted to exactly 69/67");
  const v31Comparison = await request("/api/intelligence/v3.1/comparisons/latest");
  assert(v31Comparison.response.ok && v31Comparison.body?.data?.comparison?.official === false, "disposable v3.1 evidence was mislabeled as an official holdout");
  const latestV31Phase = v31Comparison.body.data.comparison.phase;
  if (latestV31Phase === "development") {
    assert(v31Comparison.body.data.comparison.comparison?.task_count >= 1 && v31Comparison.body.data.comparison.comparison?.scores?.luna_max === 100 && v31Comparison.body.data.comparison.comparison?.scores?.sol_high === 100, "the live development comparison is not visible in the v3.1 adapter");
  } else {
    assert(latestV31Phase === "pilot" && v31Comparison.body.data.comparison.comparison?.task_count === 30 && v31Comparison.body.data.comparison.comparison?.result === "ceiling_family_discarded", "the complete disposable pilot comparison is not visible in the v3.1 adapter");
  }
  const v31Tracks = await request("/api/intelligence/v3.1/tracks");
  assert(v31Tracks.response.ok && v31Tracks.body?.data?.tracks?.length >= 6 && v31Tracks.body.data.score_dimensions?.headline === "correctness", "v3.1 tracks endpoint is unavailable or collapsed into a single score");
  checks.push("v3.1 API separates diagnostic, pilot, calibration, frozen, and official phases while withholding grader evidence");

  const suiteRoute = await request("/api/intelligence/v1/suite");
  assert(suiteRoute.response.ok && suiteRoute.body?.data?.suite?.tasks?.length === 5, "versioned suite endpoint did not expose all task descriptors");
  assert(Array.isArray(suiteRoute.body?.data?.model_suite_leaderboard), "versioned suite endpoint does not expose the model suite leaderboard");
  const parserCatalog = await request("/api/intelligence/v1/benchmark?task_id=TC-SWE-002");
  assert(parserCatalog.response.ok && parserCatalog.body?.data?.task?.id === "TC-SWE-002" && parserCatalog.body?.data?.task?.allowlisted_files?.length === 2, "task-aware benchmark catalog did not select TC-SWE-002");
  checks.push("suite and task-aware catalog adapters expose all five public task projections");

  let session = await register(handle);
  checks.push("participant can register with a pseudonymous handle and least-privilege grants");
  await stopServer();
  startServer();
  await waitForServer();
  session = await login(handle);
  checks.push("participant account and handle/password login survive a server restart");

  const serializedConsensus = Object.entries(consensusFixes).map(([file, content]) => `// FILE: ${file}\n${content}`).join("\n\n");
  const solution = await request("/api/intelligence/v1/solutions", { method: "POST", body: JSON.stringify({ task_id: "TC-SWE-001", title: "Resilient sensor consensus repair", code: serializedConsensus, language: "trit" }) }, session.token);
  assert(solution.response.status === 201, `solution save returned HTTP ${solution.response.status}`);
  assert(solution.body?.solution?.owner_handle === handle, "saved solution owner was not derived from the authenticated handle");
  const discussion = await request("/api/intelligence/v1/discussions", { method: "POST", body: JSON.stringify({ task_id: "TC-SWE-001", solution_id: solution.body.solution.id, body: "Pseudocode: reject invalid readings first; count missing sentinels; select the sole value, average the surviving pair toward zero, or return the median for three valid readings." }) }, session.token);
  assert(discussion.response.status === 201, `discussion publish returned HTTP ${discussion.response.status}`);
  assert(discussion.body?.discussion?.author_handle === handle, "discussion author was not derived from the authenticated handle");
  checks.push("participant can save a versioned solution and publish a linked plain-text explanation");

  // This script is a deterministic API/browser harness proof. It intentionally
  // does not claim that a model generated the patch; real model rollouts must
  // provide provenance-bound contexts and artifact hashes to the attestor.
  const runResult = await request("/api/intelligence/v1/runs", { method: "POST", body: JSON.stringify({ task_id: "TC-SWE-001", evaluation_kind: "harness_fixture", harness: "known-correct-fixture-replay" }) }, session.token);
  assert(runResult.response.status === 201, `intelligence run start returned HTTP ${runResult.response.status}`);
  const runId = runResult.body.run.run_id;
  const trials = runResult.body.run.trials;
  assert(trials.length === 4, "intelligence run did not create four trials");
  for (let index = 0; index < trials.length; index += 1) {
    const trialId = trials[index].id;
    for (const [file, content] of Object.entries(consensusFixes)) {
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
    assert(!JSON.stringify(hidden.body).includes("three-neg-mixed"), "hidden case identifier leaked in a trial receipt");
  }
  const aggregate = await request(`/api/intelligence/v1/runs/${runId}/aggregate`, {}, session.token);
  assert(aggregate.response.ok && aggregate.body.aggregate?.score === 100, "four-trial aggregate did not produce a sealed 100 score");
  const serviceLogin = await request("/api/auth/v1/login", { method: "POST", body: JSON.stringify({ identity_id: "tc:identity:demo-service", access_key: "local-service-key" }) });
  assert(serviceLogin.response.ok, "privileged attestor service login failed");
  const attestation = await request(`/api/intelligence/v1/runs/${runId}/attest`, { method: "POST", body: JSON.stringify({ evaluation_kind: "harness_fixture", provider: "treatcode", model: "bounded-suite-proof", reasoning_effort: "not-applicable", harness: "known-correct-fixture-replay", evidence_hashes: ["sha256:p14-api-proof"] }) }, serviceLogin.body.data.credential.token);
  assert(attestation.response.ok && attestation.body.attestation?.evaluation_kind === "harness_fixture" && attestation.body.attestation?.evidence_hash?.startsWith("sha256:"), "privileged fixture attestation was not recorded");
  const intelligenceLeaderboard = await request("/api/intelligence/v1/leaderboard");
  assert(intelligenceLeaderboard.body?.official_leaderboard?.some((entry) => entry.handle === handle && entry.score === 100 && entry.evaluation_kind === "harness_fixture"), "official fixture leaderboard did not publish the deterministic harness proof");
  assert(!intelligenceLeaderboard.body?.official_leaderboard?.some((entry) => entry.handle === handle && entry.model === "gpt-5.6-luna"), "fixture proof was mislabeled as a Luna model score");
  checks.push("one-shot public-gated four-trial harness proof is explicitly separated from model evaluation");

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
    schema: "trit.treatcode_p14_intelligence_e2e.diagnostic.v3.1",
    ok: true,
    evaluation: { kind: "harness_fixture", model_generated: false, harness: "known-correct-fixture-replay" },
    external_reference: { provider: "openai", model: "gpt-5.6-luna", reasoning_effort: "max", score: 67, confidence_interval: 4, source: "https://deepswe.datacurve.ai/" },
    participant: { handle, identity_id: session.identity.id },
    run_id: runId,
    trial_ids: trials.map((trial) => trial.id),
    aggregate: aggregate.body.aggregate,
    attestation: attestation.body.attestation,
    checks,
    artifact_hashes: [path.join(repoRoot, "benchmarks", "intelligence-diagnostic", "manifest.json"), path.join(appRoot, "src", "intelligenceService.ts")].map((file) => ({ file: path.relative(repoRoot, file).replaceAll(path.sep, "/"), sha256: fileHash(file) })),
  };
  fs.mkdirSync(evidenceRoot, { recursive: true });
  fs.writeFileSync(path.join(evidenceRoot, "intelligence-e2e.json"), `${JSON.stringify(evidence, null, 2)}\n`);
} catch (error) {
  errors.push(String(error?.stack || error?.message || error));
  fs.mkdirSync(evidenceRoot, { recursive: true });
  fs.writeFileSync(path.join(evidenceRoot, "intelligence-e2e.json"), `${JSON.stringify({ schema: "trit.treatcode_p14_intelligence_e2e.diagnostic.v3.1", ok: false, checks, errors, server_output: serverOutput }, null, 2)}\n`);
} finally {
  await stopServer();
  fs.rmSync(stateRoot, { recursive: true, force: true });
}

console.log(`P14 intelligence E2E: ${errors.length ? "failed" : "passed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = errors.length ? 1 : 0;
