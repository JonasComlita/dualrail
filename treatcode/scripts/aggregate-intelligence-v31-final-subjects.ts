import { existsSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { compareIntelligenceV31 } from "../src/intelligenceV31Comparison";
import { verifyIntelligenceV31FrozenSuite } from "../src/intelligenceV31Repository";
import type { IntelligenceV31DimensionObservation } from "../src/intelligenceV31Tracks";

const repositoryRoot = path.resolve(import.meta.dir, "..", "..");
const corpusRoot = path.join(repositoryRoot, "benchmarks", "intelligence-v3.1");
const argument = (name: string): string | undefined => process.argv.find((value) => value.startsWith(`--${name}=`))?.slice(name.length + 3);
const runId = argument("run-id") || "";
if (!/^[A-Za-z0-9._-]+$/.test(runId)) throw new Error("usage: --run-id=<safe frozen subject run id>");
const runRoot = path.join(repositoryRoot, "build", "intelligence-v31-final-subject-runs", runId);
const frozenPath = path.join(corpusRoot, "frozen-suite.v3.1.json");
const reviewPath = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v31-review-validation.json");
const calibrationPath = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v31-final-calibration.json");
const contractPath = path.join(corpusRoot, "final-corpus-contract.v3.1.json");
const registryPath = path.join(corpusRoot, "run-registry.v3.1.json");
for (const required of [runRoot, frozenPath, reviewPath, calibrationPath, contractPath, registryPath]) if (!existsSync(required)) throw new Error(`final comparison prerequisite is unavailable: ${required}`);
const frozen = JSON.parse(readFileSync(frozenPath, "utf8"));
const frozenIssues = verifyIntelligenceV31FrozenSuite(frozen);
if (frozenIssues.length) throw new Error(`frozen suite failed verification: ${frozenIssues.join("; ")}`);
const review = JSON.parse(readFileSync(reviewPath, "utf8"));
const calibration = JSON.parse(readFileSync(calibrationPath, "utf8"));
const contract = JSON.parse(readFileSync(contractPath, "utf8"));
const publicationReady = review.status === "all_candidates_twice_approved" && review.twice_approved === 100 && calibration.status === "ready_to_freeze" && calibration.accepted_tasks === 100 && calibration.infrastructure_blocked === false && contract.readiness.frozen === 100 && Object.values(contract.infrastructure_readiness).every(Boolean);
if (!publicationReady) throw new Error("final comparison publication gates are incomplete");
const executionAttestationHashes = new Set<string>();

function dimensionObservation(grade: any, taskId: string): IntelligenceV31DimensionObservation {
  const report = grade.sealed_report || {};
  const total = Number(report.behavioral_total || 0) + Number(report.adversarial_total || 0);
  const efficiency = grade.efficiency || grade.agent_execution?.efficiency || {};
  return {
    task_id: taskId,
    track: grade.track,
    correctness: Boolean(grade.passed),
    robustness: total > 0 ? (Number(report.behavioral_passed || 0) + Number(report.adversarial_passed || 0)) / total : null,
    wall_clock_ms: Number.isFinite(report.wall_clock_ms) ? report.wall_clock_ms : Number.isFinite(grade.elapsed_ms) ? grade.elapsed_ms : null,
    latency_budget_ms: Number.isFinite(grade.budget_ms) ? grade.budget_ms : 1_200_000,
    cpu_ms: Number.isFinite(report.cpu_ms) ? report.cpu_ms : null,
    cpu_budget_ms: Number.isFinite(report.cpu_budget_ms) ? report.cpu_budget_ms : null,
    memory_mb: Number.isFinite(report.memory_mb) ? report.memory_mb : null,
    memory_budget_mb: Number.isFinite(report.memory_budget_mb) ? report.memory_budget_mb : null,
    tool_calls: Number.isFinite(efficiency.tool_calls) ? efficiency.tool_calls : null,
    successful_tool_calls: Number.isFinite(efficiency.successful_tool_calls) ? efficiency.successful_tool_calls : null,
    tool_errors: Number.isFinite(efficiency.tool_errors) ? efficiency.tool_errors : null,
    discussion_quality: grade.discussion?.scored === true && Number.isFinite(grade.discussion.quality_percent) ? grade.discussion.quality_percent : null,
    telemetry_source: report.telemetry_source || (grade.efficiency ? "participant_reported" : undefined),
  };
}

function gradesFor(model: "gpt-5.6-luna" | "gpt-5.6-sol", reasoning: "max" | "high") {
  const outcomes = [];
  const observations: IntelligenceV31DimensionObservation[] = [];
  for (const taskId of frozen.task_order) {
    const gradePath = path.join(runRoot, taskId, `${model}-${reasoning}`, "sealed-grade.json");
    if (!existsSync(gradePath)) throw new Error(`${model}/${taskId}: sealed grade is missing`);
    const grade = JSON.parse(readFileSync(gradePath, "utf8"));
    const frozenTask = frozen.tasks.find((item: { task_id: string }) => item.task_id === taskId);
    if (grade.schema !== "treatcode.intelligence.final-subject-grade.v3.1" || grade.phase !== "subject" || grade.official !== true || grade.run_id !== runId || grade.task_id !== taskId || grade.track !== frozenTask?.track || grade.model !== model || grade.reasoning_effort !== reasoning || grade.suite_sha256 !== frozen.suite_sha256 || grade.agent_execution?.fresh_context !== true || grade.agent_execution?.cross_task_memory_disabled !== true || grade.agent_execution?.attempt_count !== 1 || grade.agent_execution?.model_execution_verified !== true || !/^[a-f0-9]{64}$/.test(grade.agent_execution?.execution_attestation_sha256 || "")) throw new Error(`${model}/${taskId}: sealed grade identity or execution controls are invalid`);
    if (executionAttestationHashes.has(grade.agent_execution.execution_attestation_sha256)) throw new Error(`${model}/${taskId}: provider execution attestation was reused`);
    executionAttestationHashes.add(grade.agent_execution.execution_attestation_sha256);
    outcomes.push({ task_id: taskId, passed: Boolean(grade.passed), track: frozenTask.track });
    observations.push(dimensionObservation(grade, taskId));
  }
  return { outcomes, observations };
}

const lunaGrades = gradesFor("gpt-5.6-luna", "max");
const solGrades = gradesFor("gpt-5.6-sol", "high");
const comparison = compareIntelligenceV31({
  suite_frozen: true,
  suite_hash: frozen.suite_sha256,
  frozen_task_order: frozen.task_order,
  publication_ready: publicationReady,
  luna: { model: "gpt-5.6-luna", reasoning_effort: "max", outcomes: lunaGrades.outcomes },
  sol: { model: "gpt-5.6-sol", reasoning_effort: "high", outcomes: solGrades.outcomes },
  dimension_observations: { luna: lunaGrades.observations, sol: solGrades.observations },
  bootstrap_iterations: 10_000,
});
if (!comparison.complete || !comparison.official) throw new Error(`final comparison is incomplete: ${comparison.blockers.join("; ")}`);
const evidencePath = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", `intelligence-v31-final-comparison-${runId}.json`);
writeFileSync(evidencePath, `${JSON.stringify({ ...comparison, run_id: runId, suite_sha256: frozen.suite_sha256, generated_at: new Date().toISOString(), retuned_after_unblinding: false }, null, 2)}\n`, { flag: "wx", mode: 0o444 });
const registry = JSON.parse(readFileSync(registryPath, "utf8"));
if (registry.runs.some((run: { run_id: string }) => run.run_id === runId)) throw new Error("final subject run is already registered");
registry.official = true;
registry.runs.push({ run_id: runId, phase: "official", suite_version: "3.1", task_count: 100, artifact_path: path.relative(repositoryRoot, evidencePath).replaceAll(path.sep, "/"), subject_configurations: ["gpt-5.6-luna:max", "gpt-5.6-sol:high"], official: true, holdout: true, note: comparison.target_reproduced ? "Frozen one-shot comparison met the preregistered 60-75 score band and 1-4 task Sol lead." : "Frozen one-shot comparison missed the preregistered replication target; the suite was not edited or rerun." });
writeFileSync(registryPath, `${JSON.stringify(registry, null, 2)}\n`);
contract.status = comparison.target_reproduced ? "official_replication_complete" : "official_replication_failed_without_retuning";
contract.official = true;
contract.execution_identity_readiness.provider_signed_subject_attempts = 200;
writeFileSync(contractPath, `${JSON.stringify(contract, null, 2)}\n`);
console.log(JSON.stringify({ status: contract.status, official: true, run_id: runId, luna_score: comparison.luna_score, sol_score: comparison.sol_score, sol_lead: comparison.sol_lead, target_reproduced: comparison.target_reproduced, paired_confidence_interval: comparison.paired_confidence_interval, exact_sign_test: comparison.exact_sign_test, dimensions: comparison.dimensions }, null, 2));
