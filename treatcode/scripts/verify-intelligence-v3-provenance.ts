import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { verifyIntelligenceV3ProvenanceBundle, type IntelligenceV3ProvenanceBundle } from "../src/intelligenceV3Provenance";

function argument(name: string): string | undefined {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

function argumentsFor(name: string): string[] {
  return process.argv.flatMap((value, index) => value === name && process.argv[index + 1] ? [process.argv[index + 1]] : []);
}

const bundleArgument = argument("--bundle");
const registryArgument = argument("--trusted-keys");
const subjectFamilies = argumentsFor("--subject-model-family");
if (!bundleArgument || !registryArgument || subjectFamilies.length === 0) throw new Error("Usage: bun run scripts/verify-intelligence-v3-provenance.ts --bundle <bundle.json> --trusted-keys <registry.json> --subject-model-family <family> [--subject-model-family <family> ...] [--output <report.json>] [--require-valid]");
const bundlePath = path.resolve(bundleArgument);
const registryPath = path.resolve(registryArgument);
const outputPath = path.resolve(argument("--output") || `${bundlePath}.verification.json`);
const [bundle, registry] = await Promise.all([
  readFile(bundlePath, "utf8").then((value) => JSON.parse(value) as IntelligenceV3ProvenanceBundle),
  readFile(registryPath, "utf8").then((value) => JSON.parse(value) as { schema: string; keys: Record<string, string> }),
]);
if (registry.schema !== "treatcode.intelligence.trusted-contributors.v3" || !registry.keys || typeof registry.keys !== "object") throw new Error("Trusted contributor registry is invalid");
const report = verifyIntelligenceV3ProvenanceBundle(bundle, registry.keys, [...new Set(subjectFamilies)]);
await mkdir(path.dirname(outputPath), { recursive: true });
await writeFile(outputPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
console.log(JSON.stringify({ ...report, output: outputPath }, null, 2));
if (process.argv.includes("--require-valid") && !report.passed) process.exitCode = 2;
