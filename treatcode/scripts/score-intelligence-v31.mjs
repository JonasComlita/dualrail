import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const evidenceRoot = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14");
const argument = (name) => process.argv.find((value) => value.startsWith(`--${name}=`))?.slice(name.length + 3);
const requestedSource = argument("source") || "auto";
const sources = new Set(["auto", "official", "development", "pilot", "legacy"]);
if (!sources.has(requestedSource)) throw new Error("usage: npm run score:intelligence:v31 -- [--source=auto|official|development|pilot|legacy]");

function readJson(filePath) {
  try {
    return JSON.parse(fs.readFileSync(filePath, "utf8"));
  } catch {
    return null;
  }
}

function candidates(directory, pattern) {
  if (!fs.existsSync(directory)) return [];
  return fs.readdirSync(directory).filter((file) => pattern.test(file)).map((file) => {
    const artifactPath = path.join(directory, file);
    const evidence = readJson(artifactPath);
    return evidence ? { artifactPath, evidence } : null;
  }).filter(Boolean).sort((left, right) => String(left.evidence.generated_at || left.artifactPath).localeCompare(String(right.evidence.generated_at || right.artifactPath)));
}

function validNumber(value) {
  return typeof value === "number" && Number.isFinite(value);
}

function pilotDimensions(evidence) {
  const rows = Array.isArray(evidence.task_matrix) ? evidence.task_matrix : [];
  const score = (key) => {
    const observed = rows.filter((row) => row[key]?.correctness_observed);
    const latencyValues = observed.map((row) => row[key].subject_elapsed_ms).filter(validNumber).map((elapsed) => Math.max(0, Math.min(100, (1200000 / Math.max(1, elapsed)) * 100)));
    const roundedMean = (values) => values.length ? Math.round(values.reduce((sum, value) => sum + value, 0) / values.length * 100) / 100 : null;
    return {
      observed_tasks: observed.length,
      correctness: observed.length ? Math.round(observed.filter((row) => row[key].passed).length / observed.length * 10000) / 100 : 0,
      robustness: null,
      latency: roundedMean(latencyValues),
      resource_use: null,
      tool_execution: null,
      discussion_quality: null,
      notes: ["pilot evidence has wall-clock timing only", "robustness, resource, tool, and independently calibrated discussion signals are unavailable"],
    };
  };
  const luna = score("luna_max");
  const sol = score("sol_high");
  const difference = (left, right) => left === null || right === null ? null : Math.round((right - left) * 100) / 100;
  return { luna_max: luna, sol_high: sol, deltas: { correctness: Math.round((sol.correctness - luna.correctness) * 100) / 100, robustness: null, latency: difference(luna.latency, sol.latency), resource_use: null, tool_execution: null, discussion_quality: null }, notes: [...new Set([...luna.notes, ...sol.notes])] };
}

function developmentScore() {
  const entries = candidates(evidenceRoot, /^intelligence-v31-development-comparison-[A-Za-z0-9._-]+\.json$/);
  const entry = entries.reverse().find(({ evidence }) => evidence.schema === "treatcode.intelligence.development-comparison.v3.1" && evidence.phase === "development" && evidence.official === false && evidence.complete === true && Number.isSafeInteger(evidence.task_count) && evidence.task_count > 0 && validNumber(evidence.luna_score) && validNumber(evidence.sol_score));
  if (!entry) return null;
  const { evidence, artifactPath } = entry;
  return { phase: "development", official: false, run_id: evidence.run_id, task_count: evidence.task_count, luna_score: evidence.luna_score, sol_score: evidence.sol_score, sol_lead: evidence.sol_lead, target_reproduced: false, paired_confidence_interval: evidence.paired_confidence_interval, exact_sign_test: evidence.exact_sign_test, discrimination: evidence.discrimination || null, dimensions: evidence.dimensions || null, artifactPath, note: "Disposable paired development evidence; not an official holdout." };
}

function officialScore() {
  const entries = candidates(evidenceRoot, /^intelligence-v31-final-comparison-[A-Za-z0-9._-]+\.json$/);
  const entry = entries.reverse().find(({ evidence }) => evidence.schema === "treatcode.intelligence.comparison.v3.1" && evidence.official === true && evidence.complete === true && evidence.task_count === 100 && validNumber(evidence.luna_score) && validNumber(evidence.sol_score));
  if (!entry) return null;
  const { evidence, artifactPath } = entry;
  return { phase: "official", official: true, run_id: evidence.run_id, task_count: evidence.task_count, luna_score: evidence.luna_score, sol_score: evidence.sol_score, sol_lead: evidence.sol_lead, target_reproduced: Boolean(evidence.target_reproduced), paired_confidence_interval: evidence.paired_confidence_interval, exact_sign_test: evidence.exact_sign_test, discrimination: evidence.discrimination || null, dimensions: evidence.dimensions || null, artifactPath, note: "Official frozen holdout comparison." };
}

function pilotScore() {
  const artifactPath = path.join(evidenceRoot, "intelligence-v31-pilot-comparison.json");
  const evidence = readJson(artifactPath);
  if (!evidence || evidence.schema !== "trit.treatcode_intelligence_v31_pilot_comparison.v1" || evidence.status !== "complete" || !evidence.scores?.luna_max || !evidence.scores?.sol_high) return null;
  return { phase: "pilot", official: false, run_id: evidence.run_id, task_count: evidence.paired_tasks_complete, luna_score: evidence.scores.luna_max.percent, sol_score: evidence.scores.sol_high.percent, sol_lead: evidence.scores.paired_sol_lead_tasks, target_reproduced: false, paired_confidence_interval: null, exact_sign_test: null, discrimination: evidence.discrimination || null, dimensions: evidence.dimensions || pilotDimensions(evidence), artifactPath, note: "Disposable pilot evidence; the pilot family was classified as ceiling-prone and is not a holdout." };
}

function legacyScore() {
  const root = path.join(repositoryRoot, "build", "treatcode-model-evals");
  const entries = [];
  if (fs.existsSync(root)) for (const run of fs.readdirSync(root, { withFileTypes: true }).filter((entry) => entry.isDirectory())) {
    const artifactPath = path.join(root, run.name, "comparison.json");
    const evidence = readJson(artifactPath);
    if (evidence?.schema === "treatcode.intelligence.v3-development-comparison.v1" && validNumber(evidence.correctness?.luna_max?.task_pass_rate) && validNumber(evidence.correctness?.sol_high?.task_pass_rate)) entries.push({ artifactPath, evidence });
  }
  entries.sort((left, right) => String(left.evidence.generated_at || left.artifactPath).localeCompare(String(right.evidence.generated_at || right.artifactPath)));
  const entry = entries[entries.length - 1];
  if (!entry) return null;
  const { evidence, artifactPath } = entry;
  return { phase: "diagnostic", official: false, run_id: path.basename(path.dirname(artifactPath)), task_count: evidence.scope?.executable_tasks || 0, luna_score: evidence.correctness.luna_max.task_pass_rate, sol_score: evidence.correctness.sol_high.task_pass_rate, sol_lead: 0, target_reproduced: false, paired_confidence_interval: null, exact_sign_test: null, artifactPath, note: "Two-task diagnostic evidence; both models reached the task ceiling." };
}

const bySource = { official: officialScore, development: developmentScore, pilot: pilotScore, legacy: legacyScore };
let result = null;
if (requestedSource !== "auto") result = bySource[requestedSource]();
else result = officialScore() || developmentScore() || pilotScore() || legacyScore();
if (!result) throw new Error(requestedSource === "auto" ? "no completed Luna/Sol score artifact is available" : `no completed ${requestedSource} score artifact is available`);
const relativeArtifact = path.relative(repositoryRoot, result.artifactPath).split(path.sep).join("/");
console.log(JSON.stringify({
  schema: "treatcode.intelligence.score.v3.1",
  status: "scored",
  phase: result.phase,
  official: result.official,
  run_id: result.run_id,
  task_count: result.task_count,
  luna: { model: "gpt-5.6-luna", reasoning_effort: "max", score: result.luna_score },
  sol: { model: "gpt-5.6-sol", reasoning_effort: "high", score: result.sol_score },
  sol_lead: result.sol_lead,
  target_reproduced: result.target_reproduced,
  paired_confidence_interval: result.paired_confidence_interval,
  exact_sign_test: result.exact_sign_test,
  discrimination: result.discrimination,
  dimensions: result.dimensions,
  artifact: relativeArtifact,
  note: result.note,
}, null, 2));
