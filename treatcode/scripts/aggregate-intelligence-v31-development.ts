import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import path from "node:path";
import { collectIntelligenceV31DevelopmentComparison } from "../src/intelligenceV31Development";

const repositoryRoot = path.resolve(import.meta.dir, "..", "..");
const argument = (name: string): string | undefined => process.argv.find((value) => value.startsWith(`--${name}=`))?.slice(name.length + 3);
const runId = argument("run-id") || "";
if (!/^[A-Za-z0-9._-]+$/.test(runId)) throw new Error("usage: --run-id=<safe disposable development run id> [--run-root=<build-relative path>] [--output=<path>]");
const buildRoot = path.resolve(repositoryRoot, "build");
const requestedRunRoot = argument("run-root");
const runRoot = path.resolve(repositoryRoot, requestedRunRoot || path.join(buildRoot, "intelligence-v31-development-runs", runId));
const relativeRunRoot = path.relative(buildRoot, runRoot);
if (!relativeRunRoot || relativeRunRoot === ".." || relativeRunRoot.startsWith(`..${path.sep}`) || path.isAbsolute(relativeRunRoot)) throw new Error("run root must remain inside the repository build directory");
const defaultOutput = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", `intelligence-v31-development-comparison-${runId}.json`);
const outputArgument = argument("output");
const outputPath = path.resolve(outputArgument || defaultOutput);
const comparison = collectIntelligenceV31DevelopmentComparison(runRoot, runId);
if (existsSync(outputPath)) throw new Error(`development comparison output already exists: ${outputPath}`);
mkdirSync(path.dirname(outputPath), { recursive: true });
writeFileSync(outputPath, `${JSON.stringify(comparison, null, 2)}\n`, { flag: "wx", mode: 0o444 });
console.log(JSON.stringify({
  status: "scored",
  official: comparison.official,
  phase: comparison.phase,
  run_id: comparison.run_id,
  task_count: comparison.task_count,
  luna_score: comparison.luna_score,
  sol_score: comparison.sol_score,
  sol_lead: comparison.sol_lead,
  target_reproduced: comparison.target_reproduced,
  paired_confidence_interval: comparison.paired_confidence_interval,
  exact_sign_test: comparison.exact_sign_test,
  discrimination: comparison.discrimination,
  dimensions: comparison.dimensions,
  output: outputPath,
}, null, 2));
