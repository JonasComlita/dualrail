import { readFile, realpath } from "node:fs/promises";
import path from "node:path";
import {
  EXECUTION_REQUEST_SCHEMA,
  SecureExecutionQueue,
  type ExecutionResult,
} from "../src/runner/secure-runner";

interface PublicManifest {
  schema: "treatcode.intelligence.public-task.v3";
  version: 3;
  task_id: string;
  source_files: string[];
  entrypoint: string;
  arity: number;
  public_cases_path: string;
  limits: { wall_clock_ms_per_case: number };
}

interface PublicCase {
  id: string;
  args: number[];
  expected: number;
  suite: string;
}

const participantArgument = process.argv.find((value) => value.startsWith("--participant="))?.slice("--participant=".length);
if (!participantArgument) throw new Error("usage: bun run scripts/run-intelligence-v3-public.ts --participant=<participant-root>");

const repositoryRoot = path.resolve(import.meta.dir, "..", "..");
const participantRoot = await realpath(path.resolve(participantArgument));
const manifest = JSON.parse(await readFile(path.join(participantRoot, "task.public.v3.json"), "utf8")) as PublicManifest;
if (manifest.schema !== "treatcode.intelligence.public-task.v3" || manifest.version !== 3) throw new Error("invalid public task manifest");
if (!Array.isArray(manifest.source_files) || manifest.source_files.length < 2) throw new Error("public task must contain at least two source modules");
const casesPayload = JSON.parse(await readFile(path.join(participantRoot, manifest.public_cases_path), "utf8")) as { schema: string; visibility: string; task_id: string; cases: PublicCase[] };
if (casesPayload.schema !== "treatcode.intelligence.cases.v3" || casesPayload.visibility !== "public" || casesPayload.task_id !== manifest.task_id) throw new Error("invalid public cases file");
const sources = (await Promise.all(manifest.source_files.map((relative) => readFile(path.join(participantRoot, relative), "utf8")))).join("\n\n");
const queue = new SecureExecutionQueue({
  artifactRoot: path.join(path.dirname(participantRoot), "public-runner-artifacts"),
  repositoryRoot,
  workerPath: path.join(repositoryRoot, "treatcode", "src", "runner", "worker.ts"),
  concurrency: 1,
  maxRetries: 0,
  limits: {
    wallTimeMs: manifest.limits.wall_clock_ms_per_case,
    cpuMs: manifest.limits.wall_clock_ms_per_case,
    outputBytes: 262_144,
    memoryMb: 256,
    processCount: 4,
  },
});

function resultValue(result: ExecutionResult): number | null {
  if (!result.success) return null;
  if (typeof result.r13 === "number") return result.r13;
  return typeof result.registers?.r13 === "number" ? result.registers.r13 : null;
}

const results = [];
for (const test of casesPayload.cases) {
  if (!Array.isArray(test.args) || test.args.length !== manifest.arity) throw new Error(`invalid argument count for ${test.id}`);
  const code = `${sources}\n\nfn main() -> t40 {\n    return ${manifest.entrypoint}(${test.args.join(", ")});\n}\n`;
  const execution = await queue.submit({
    schema: EXECUTION_REQUEST_SCHEMA,
    kind: "trit.compile-and-run",
    code,
    engine: "bootstrap",
    optLevel: "-O2",
    sourceCommit: "intelligence-v3-public-development",
  }).result;
  const actual = resultValue(execution);
  results.push({ id: test.id, suite: test.suite, expected: test.expected, actual, passed: execution.success && actual === test.expected, error: execution.success ? undefined : execution.error || execution.compilerOutput });
}

const report = {
  schema: "treatcode.intelligence.public-report.v3",
  task_id: manifest.task_id,
  passed: results.every((item) => item.passed),
  passed_cases: results.filter((item) => item.passed).length,
  total_cases: results.length,
  cases: results,
};
console.log(JSON.stringify(report, null, 2));
if (!report.passed) process.exitCode = 1;
