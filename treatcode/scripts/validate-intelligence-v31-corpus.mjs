import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const corpusRoot = path.join(repoRoot, "benchmarks", "intelligence-v3.1");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P14");
const expectedPilotMix = {
  algorithm_and_data_structure: 4,
  balanced_ternary_and_numeric: 3,
  compiler_and_codegen: 5,
  memory_and_pointer_safety: 3,
  concurrency_and_state: 3,
  syscall_and_abi: 3,
  multi_module_repository_repair: 4,
  build_test_and_tooling: 3,
  performance_and_resource_constraints: 2,
};
const expectedFinalMix = {
  algorithm_and_data_structure: 15,
  balanced_ternary_and_numeric: 10,
  compiler_and_codegen: 15,
  memory_and_pointer_safety: 10,
  concurrency_and_state: 10,
  syscall_and_abi: 10,
  multi_module_repository_repair: 15,
  build_test_and_tooling: 10,
  performance_and_resource_constraints: 5,
};
const minimumCoverage = {
  repository_exploration_required: 100,
  multi_module_change: 80,
  misleading_or_incomplete_tests: 40,
  conflicting_invariants: 40,
  initial_build_failure: 30,
  abi_or_integration_bug: 30,
  performance_constraint: 25,
};
const errors = [];
const checks = [];
const readJson = (name) => JSON.parse(fs.readFileSync(path.join(corpusRoot, name), "utf8"));
const assert = (condition, message) => { if (!condition) errors.push(message); else checks.push(message); };
const equalObject = (left, right) => JSON.stringify(left) === JSON.stringify(right);

const protocol = readJson("protocol.v3.1.json");
const pilot = readJson("pilot-corpus.v3.1.json");
const finalContract = readJson("final-corpus-contract.v3.1.json");
const calibration = readJson("calibration-plan.v3.1.json");
const runRegistry = readJson("run-registry.v3.1.json");
const schemas = ["repository-task.schema.v3.1.json", "repository-grader.schema.v3.1.json", "repository-patch.schema.v3.1.json", "frozen-suite.schema.v3.1.json", "final-task-record.schema.v3.1.json", "calibration-observation.schema.v3.1.json", "author-contribution.schema.v3.1.json", "review-contribution.schema.v3.1.json"].map(readJson);

assert(protocol.schema === "treatcode.intelligence.protocol.v3.1" && protocol.execution_modes.includes("scalar") && protocol.execution_modes.includes("repository"), "protocol preserves scalar mode and adds repository mode");
assert(protocol.legacy_diagnostic_tasks.join(",") === "TC-V3-001,TC-V3-002" && protocol.legacy_tasks_holdout_eligible === false, "legacy tasks are diagnostic-only and excluded from holdouts");
assert(protocol.replication_target.score_band.minimum === 60 && protocol.replication_target.score_band.maximum === 75 && protocol.replication_target.sol_lead_tasks.minimum === 1 && protocol.replication_target.sol_lead_tasks.maximum === 4 && protocol.replication_target.exact_69_67_required === false, "replication target is directional and not fitted to exactly 69/67");
assert(pilot.task_count === 30 && pilot.tasks.length === 30 && new Set(pilot.tasks.map((task) => task.task_id)).size === 30, "pilot registry has 30 distinct disposable task designs");
assert(equalObject(pilot.category_counts, expectedPilotMix), "pilot category mix matches the frozen development plan");
assert(pilot.tasks.every((task) => task.disposable && task.required_trit_change && task.holdout_eligible_for["gpt-5.6-luna"] === false && task.holdout_eligible_for["gpt-5.6-sol"] === false), "all pilot designs require Trit and are barred from Luna/Sol holdouts");
assert(new Set(pilot.tasks.map((task) => task.title)).size === 30 && new Set(pilot.tasks.map((task) => task.initial_signal)).size === 30, "pilot titles and initial signals are unique");
const computedFeatures = Object.fromEntries(Object.keys(pilot.design_feature_counts).map((feature) => [feature, pilot.tasks.filter((task) => task.design_features.includes(feature)).length]));
assert(equalObject(computedFeatures, pilot.design_feature_counts), "pilot design-feature counts are derived from the task registry");
assert(finalContract.task_count === 100 && equalObject(finalContract.category_mix, expectedFinalMix), "final contract fixes an unseen 100-task category mix");
assert(Object.entries(minimumCoverage).every(([feature, minimum]) => finalContract.minimum_design_coverage[feature] >= minimum), "final contract meets every strengthened design-coverage minimum");
assert(finalContract.official === false && Object.values(finalContract.readiness).every((value) => value === 0), "unbuilt final tasks remain explicit development evidence");
assert(Object.values(finalContract.infrastructure_readiness).every((value) => value === false), "unverified official sandbox, evidence-store, and resource-accounting infrastructure remains blocked");
assert(calibration.excluded_model_families.includes("gpt-5.6-luna") && calibration.excluded_model_families.includes("gpt-5.6-sol"), "subject model families are excluded from final calibration");
assert(runRegistry.runs.every((run) => run.official === false && run.holdout === false), "registered development runs remain non-official and non-holdout");
assert(schemas.every((schema) => schema.$schema?.includes("2020-12") && schema.additionalProperties === false), "repository, patch, grader, freeze, provenance, and calibration contracts are strict JSON schemas");

const executablePilotPackages = pilot.tasks.filter((task) => task.package_status === "executable").length;
const disposedPilotPackages = pilot.tasks.filter((task) => task.package_status === "disposed" && task.disposition === "discarded_after_ceiling_analysis" && /^[a-f0-9]{64}$/.test(task.disposed_participant_bundle_hash || "") && /^[a-f0-9]{64}$/.test(task.disposed_grader_bundle_hash || "")).length;
const completedPilotPackages = executablePilotPackages + disposedPilotPackages;
assert(completedPilotPackages === 30, "all disposable pilot packages were qualified or hash-bound at disposal");
const report = {
  schema: "trit.treatcode_intelligence_v31_validation.v1",
  ok: errors.length === 0,
  status: "development_evidence",
  counts: { pilot_designs: pilot.tasks.length, executable_pilot_packages: executablePilotPackages, disposed_pilot_packages: disposedPilotPackages, final_candidates: finalContract.readiness.candidate_tasks, frozen_final_tasks: finalContract.readiness.frozen },
  checks,
  errors,
  publication_blockers: [
    ...(completedPilotPackages < 30 ? [`${30 - completedPilotPackages} pilot packages were never qualified`] : []),
    `${100 - finalContract.readiness.executable_packages} final tasks still need executable packages`,
    "independent authorship, two reviews, non-subject calibration, and the final freeze have not occurred",
    "no held-out Luna/Sol v3.1 result exists",
  ],
};
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "intelligence-v31-corpus-validation.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(`Intelligence v3.1 corpus validation: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
console.log(`  [development] ${executablePilotPackages}/30 live and ${disposedPilotPackages}/30 disposed pilot packages; ${finalContract.readiness.frozen}/100 frozen final tasks`);
process.exitCode = report.ok ? 0 : 1;
