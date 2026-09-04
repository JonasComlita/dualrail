import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { calibrateIntelligenceV3Suite, type IntelligenceV3CalibrationInput } from "../src/intelligenceV3Calibration";
import type { IntelligenceV3Protocol } from "../src/intelligenceV3";

function argument(name: string): string | undefined {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

const repoRoot = path.resolve(import.meta.dir, "..", "..");
const inputArgument = argument("--input");
if (!inputArgument) throw new Error("Usage: bun run scripts/calibrate-intelligence-v3.ts --input <observations.json> [--output <report.json>] [--require-ready]");
const inputPath = path.resolve(inputArgument);
const protocolPath = path.resolve(argument("--protocol") || path.join(repoRoot, "benchmarks", "intelligence-v3", "protocol.v3.json"));
const outputPath = path.resolve(argument("--output") || path.join(repoRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v3-calibration.json"));
const [protocol, input] = await Promise.all([
  readFile(protocolPath, "utf8").then((value) => JSON.parse(value) as IntelligenceV3Protocol),
  readFile(inputPath, "utf8").then((value) => JSON.parse(value) as IntelligenceV3CalibrationInput),
]);
const report = calibrateIntelligenceV3Suite(protocol, input);
await mkdir(path.dirname(outputPath), { recursive: true });
await writeFile(outputPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
console.log(JSON.stringify({ ...report, tasks: undefined, output: outputPath }, null, 2));
if (process.argv.includes("--require-ready") && report.status !== "ready_for_human_review") process.exitCode = 2;
