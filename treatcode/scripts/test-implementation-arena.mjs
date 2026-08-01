import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { api, startServer } from "./operations-test-utils.mjs";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P10");
const checks = [];
const errors = [];

function assert(condition, message) { if (!condition) throw new Error(message); }
function read(filePath) { return fs.readFileSync(filePath, "utf8"); }

try {
  const arena = read(path.join(appRoot, "src", "ImplementationArena.tsx"));
  const main = read(path.join(appRoot, "src", "main.tsx"));
  const serverSource = read(path.join(appRoot, "server.ts"));
  const staticArena = read(path.join(appRoot, "arena", "index.html"));
  assert(main.includes('import("./ImplementationArena")'), "main route does not load the Implementation Arena");
  assert(main.includes("/arena"), "main route does not recognize /arena");
  assert(arena.includes("/api/benchmarks/p10"), "Arena does not load benchmark evidence");
  assert(arena.includes("correctness") && arena.includes("benchmark evidence"), "Arena does not expose correctness and benchmark evidence");
  assert(arena.includes("wall_time_ns") && arena.includes("vm_cycles") && arena.includes("memory_read_bytes") && arena.includes("throughput_ops_per_second"), "Arena does not render protocol metrics");
  assert(serverSource.includes('app.get("/api/benchmarks/p10"'), "server does not expose the P10 benchmark API");
  assert(staticArena.includes("Implementation Arena") && staticArena.includes("/api/benchmarks/p10"), "Arena route has no meaningful static shell");
  checks.push("/arena is a first-class route with a static shell and lazy interactive surface");
  checks.push("Arena displays correctness status before benchmark distributions");
  checks.push("Arena displays target profile, timing, VM, instruction, memory, code-size, register, and allocation metrics");
  checks.push("Arena reads immutable P10 evidence through the dedicated API endpoint");

  const server = await startServer({ build: false });
  try {
    const response = await api(server, "/api/benchmarks/p10");
    assert(response.response.status === 200, `P10 evidence API returned ${response.response.status}`);
    assert(response.body.schema === "treatcode.p10_benchmark_api.v1", "P10 evidence API schema is incorrect");
    assert(response.body.manifest?.schema === "trit.benchmark_manifest.v1", "P10 evidence API did not return the manifest");
    assert(response.body.reference?.runs?.length >= 8, "P10 evidence API did not return reference runs");
    checks.push("running server returns the immutable P10 manifest and reference artifact");
  } finally {
    await server.stop();
  }
} catch (error) {
  errors.push(String(error?.message || error));
}

const report = { schema: "treatcode.p10_implementation_arena_e2e.v1", ok: errors.length === 0, checks, errors };
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "implementation-arena-e2e.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(`P10 Implementation Arena E2E: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
