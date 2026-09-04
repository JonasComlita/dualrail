import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P06");
const manifest = JSON.parse(fs.readFileSync(path.join(repoRoot, "CHALLENGE_MANIFEST.json"), "utf8"));
const serverData = JSON.parse(fs.readFileSync(path.join(appRoot, "src", "generated", "challenges.server.json"), "utf8"));
const clientData = JSON.parse(fs.readFileSync(path.join(appRoot, "src", "generated", "challenges.client.json"), "utf8"));
const serverSource = fs.readFileSync(path.join(appRoot, "server.ts"), "utf8");
const appSource = fs.readFileSync(path.join(appRoot, "src", "App.tsx"), "utf8");
const solutionGuides = fs.readFileSync(path.join(appRoot, "src", "solutionGuides.ts"), "utf8");
const errors = [];
const checks = [];

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

try {
  const compilerDriverPath = path.join(appRoot, "compiler_driver.txe");
  const compilerDriver = fs.readFileSync(compilerDriverPath);
  assert(compilerDriver.length >= 72, "native compiler driver is too small to contain a TXE4 header");
  assert(compilerDriver.toString("ascii", 0, 4) === "TXE4", "native compiler driver is stale or not a TXE4 image");
  assert(compilerDriver.readUInt32LE(4) === 2, "native compiler driver does not use TXE version 2");
  assert(compilerDriver.readUInt32LE(8) === 0x12345678, "native compiler driver has an invalid TXE4 endianness marker");
  assert(compilerDriver.readUInt32LE(12) === 72, "native compiler driver has an invalid TXE4 header size");
  assert(compilerDriver.readUInt32LE(16) === 2, "native compiler driver does not use ISA version 2");
  assert((compilerDriver.readBigUInt64LE(24) & 1n) !== 0n, "native compiler driver does not require the base ISA v2 feature");
  assert(compilerDriver.readUInt32LE(48) === 2, "native compiler driver does not use ABI version 2");
  assert(compilerDriver.readUInt32LE(52) === 2, "native compiler driver does not use syscall ABI version 2");
  assert(compilerDriver.readUInt32LE(56) === 40, "native compiler driver has an invalid scalar word width");
  assert(compilerDriver.readUInt32LE(60) === 729, "native compiler driver has an invalid base page size");
  checks.push("checked-in native compiler driver is TXE4/ISA v2");

  assert(manifest.schema === "treatcode.challenge_manifest.v1", "manifest schema version is incorrect");
  assert(serverData.source_of_truth === "CHALLENGE_MANIFEST.json", "server generated data is not manifest-backed");
  assert(clientData.source_of_truth === "CHALLENGE_MANIFEST.json", "client generated data is not manifest-backed");
  assert(serverData.challenges.length === manifest.challenges.length, "server generated challenge count differs from manifest");
  assert(clientData.challenges.length === manifest.challenges.filter((challenge) => challenge.lifecycle !== "retired").length, "client data must exclude retired challenges");
  checks.push("manifest, server data, and client data share one source-of-truth marker");

  const ids = new Set();
  const lifecycleCounts = { published: 0, draft: 0, retired: 0 };
  for (const challenge of manifest.challenges) {
    assert(!ids.has(challenge.id), `duplicate challenge id ${challenge.id}`);
    ids.add(challenge.id);
    lifecycleCounts[challenge.lifecycle] += 1;
    for (const dimension of ["domain", "technique", "data_model", "stack_layer", "target", "optimization_objective"]) {
      assert(Array.isArray(challenge.facets[dimension]) && challenge.facets[dimension].length > 0, `${challenge.id} has no ${dimension} facet`);
      for (const value of challenge.facets[dimension]) assert(manifest.facet_dimensions[dimension].includes(value), `${challenge.id} uses undeclared ${dimension} facet ${value}`);
    }
    assert(challenge.execution?.limits?.time_ms > 0 && challenge.execution?.limits?.max_cycles > 0, `${challenge.id} has no positive execution limits`);
    if (challenge.lifecycle === "published") {
      assert(challenge.execution.mode === "verified", `${challenge.id} is published without verified mode`);
      assert(challenge.execution.correctness?.kind === "deterministic-vm-contract", `${challenge.id} is missing a deterministic VM contract`);
      assert(challenge.execution.correctness.test_cases.length > 0, `${challenge.id} has no correctness fixtures`);
      assert(challenge.execution.correctness.wrapper.template.includes("${code}"), `${challenge.id} wrapper does not insert user code`);
      for (const testCase of challenge.execution.correctness.test_cases) {
        assert(Object.hasOwn(testCase, "expected_output") || Object.hasOwn(testCase, "expected_r13"), `${challenge.id} has an expected-value-free test case`);
      }
    } else {
      assert(challenge.execution.mode === "compile-only", `${challenge.id} is not compile-only`);
      assert(!challenge.execution.correctness, `${challenge.id} exposes correctness fixtures before publication`);
    }
  }
  assert(lifecycleCounts.published > 0 && lifecycleCounts.draft > 0 && lifecycleCounts.retired > 0, "all challenge lifecycle states must be represented");
  checks.push("all challenge lifecycles have limits; published entries have deterministic fixtures");

  const clientIds = new Set(clientData.challenges.map((challenge) => challenge.id));
  assert(!clientIds.has("T059"), "retired challenge leaked into client data");
  assert(clientData.challenges.every((challenge) => !challenge.execution.correctness), "correctness fixtures leaked into client data");
  checks.push("client generation excludes retired entries and hidden correctness fixtures");

  const tritwise = manifest.challenges.find((challenge) => challenge.id === "T057");
  const vector = manifest.challenges.find((challenge) => challenge.id === "T058");
  assert(tritwise?.lifecycle === "published" && tritwise.facets.technique.includes("tritwise") && tritwise.facets.data_model.includes("word-parallel-trits"), "word-parallel tritwise pilot is incomplete");
  assert(vector?.lifecycle === "published" && vector.facets.technique.includes("vector-dot-product") && vector.facets.data_model.includes("vectors"), "vector dot-product pilot is incomplete");
  checks.push("word-parallel tritwise and vector dot-product pilots are published");

  assert(!serverSource.includes("const problems"), "server retains an independent challenge catalog");
  assert(!appSource.includes("const PROBLEMS"), "frontend retains an independent challenge catalog");
  assert(serverSource.includes("challenges.server.json") && appSource.includes("challenges.client.json"), "client/server do not consume generated challenge data");
  assert(appSource.includes("function openProblem") && appSource.includes('aria-label="Search challenges"'), "challenge browse/editor journey is not wired");
  assert(appSource.includes("PRACTICE_SOLUTION_GUIDES") && appSource.includes('data-testid="practice-solution-guide"') && appSource.includes("Discussion · why it works"), "practice solution learning notes are not visible from the problem view");
  assert(appSource.includes('General information') && appSource.includes('Discussions') && appSource.includes('data-testid="practice-discussions-tab"'), "practice problem view does not expose the agreed two-tab information/discussion experience");
  assert(appSource.includes("postedSolutions") && appSource.includes('data-testid="practice-public-solutions"') && appSource.includes('data-testid="practice-discussion-post"') && appSource.includes("Plain-English discussion"), "practice discussions do not render unified community solution posts");
  assert(appSource.includes('visibility: "public"') && appSource.includes("solution_id: savedSolution?.id") && appSource.includes("solutionId: savedSolution?.id") && appSource.includes("currentCodeIsVerified"), "practice posts are not public, linked to accepted submissions, or verification-gated");
  assert(appSource.includes("practice-post-explanation") && !appSource.includes("practice-publish-discussion") && !appSource.includes("publish discussion"), "practice still exposes a separate discussion publishing action");
  assert(appSource.includes("runtime_ms") && appSource.includes("memory_kib") && appSource.includes("practice-solution-vote"), "practice discussion cards omit measured metrics or voting");
  assert(serverSource.includes("listPublicSolutions") && serverSource.includes('app.get("/api/community/v1/solutions"') && serverSource.includes("toggleSolutionVote") && serverSource.includes("publishSolution"), "server does not expose the public verified solution discussion contract");
  for (const challengeId of ["T001", "T002", "T005", "T056", "T057", "T058"]) {
    assert(solutionGuides.includes(`${challengeId}:`), `${challengeId} is missing a published practice solution guide`);
  }
  assert(appSource.includes('fetch("/api/run"') && appSource.includes('fetch("/api/submit"'), "challenge execution actions are not wired");
  checks.push("frontend and server contain no independent catalog and preserve browse/editor/run/submit wiring");
} catch (error) {
  errors.push(String(error?.message || error));
}

const report = {
  schema: "trit.treatcode_challenge_correctness.v1",
  ok: errors.length === 0,
  manifest: "CHALLENGE_MANIFEST.json",
  generated_server: "treatcode/src/generated/challenges.server.json",
  generated_client: "treatcode/src/generated/challenges.client.json",
  checks,
  errors,
};
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "challenge-correctness.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(`P06 challenge correctness: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
