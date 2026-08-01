import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P10");
const manifestPath = path.join(repoRoot, "BENCHMARK_MANIFEST.json");
const protocolPath = path.join(repoRoot, "BENCHMARK_PROTOCOL_SCHEMA.json");
const referencePath = path.join(repoRoot, "benchmarks", "reference", "p10-reference.v1.json");
const checks = [];
const errors = [];

function assert(condition, message) { if (!condition) throw new Error(message); }
function readJson(filePath) { return JSON.parse(fs.readFileSync(filePath, "utf8")); }

try {
  const manifest = readJson(manifestPath);
  const protocol = readJson(protocolPath);
  const reference = readJson(referencePath);
  assert(manifest.schema === "trit.benchmark_manifest.v1", "P10 manifest schema is incorrect");
  assert(protocol.version === 1, "P10 protocol schema is not versioned");
  assert(reference.schema === "trit.benchmark_reference.v1", "P10 reference artifact schema is incorrect");
  assert(manifest.protocol_schema === "BENCHMARK_PROTOCOL_SCHEMA.json", "manifest does not name its protocol schema");
  assert(manifest.protocol.warmups >= 0 && manifest.protocol.repetitions >= 1, "protocol does not declare warmups and repetitions");
  assert(manifest.protocol.correctness_gate === "before_performance", "correctness gate is not ordered before performance");
  assert(manifest.target_profiles.length === 5, "all five target profiles must be present");
  assert(manifest.workloads.some((item) => item.kind === "tritwise"), "tritwise pilot is missing");
  const vectorPilot = manifest.workloads.find((item) => item.kind === "vector_matrix");
  assert(vectorPilot, "vector/matrix pilot is missing");
  assert(vectorPilot.required_metrics.includes("throughput_ops_per_second"), "vector/matrix pilot does not declare numeric throughput");
  assert(reference.runs.length >= 8, "reference artifact does not cover the declared pilots");
  checks.push("manifest, protocol schema, and reference artifact are present and versioned");
  checks.push("warmups, repetitions, limits, distributions, and regression thresholds are declared");
  checks.push("correctness equivalence is an explicit prerequisite for performance comparison");
  checks.push("tritwise representation and vector/matrix pilots have reference coverage");

  const command = process.platform === "win32" ? "python.exe" : "python3";
  const result = spawnSync(command, [path.join(repoRoot, "tools", "trit_tool.py"), "website", "benchmarks", "verify-reference", "--json"], {
    cwd: repoRoot,
    encoding: "utf8",
    stdio: "pipe",
  });
  assert(result.status === 0, `reference verifier failed:\n${result.stdout}\n${result.stderr}`);
  const report = JSON.parse(result.stdout);
  assert(report.ok === true, "reference verifier returned a non-passing report");
  assert(report.correctness_before_performance === true, "reference report does not record correctness ordering");
  assert(report.repeatability?.same_reference_result === true, "reference report is not repeatable");
  checks.push("authoritative Python verifier passes the reference distribution and repeatability checks");

  fs.mkdirSync(evidenceRoot, { recursive: true });
  fs.writeFileSync(path.join(evidenceRoot, "benchmark-tests.json"), `${JSON.stringify({
    schema: "treatcode.p10_benchmark_tests.v1",
    ok: true,
    checks,
    errors,
    verifier: report.output,
  }, null, 2)}\n`);
} catch (error) {
  errors.push(String(error?.message || error));
}

const report = { schema: "treatcode.p10_benchmark_tests.v1", ok: errors.length === 0, checks, errors };
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "benchmark-tests.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(`P10 benchmark tests: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
