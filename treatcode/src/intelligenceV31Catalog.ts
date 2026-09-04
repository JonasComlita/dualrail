import { existsSync, readFileSync } from "node:fs";
import path from "node:path";

interface V31Protocol {
  schema: "treatcode.intelligence.protocol.v3.1";
  version: "3.1";
  status: string;
  score_scope: string;
  legacy_diagnostic_tasks: string[];
  replication_target: { score_band: { minimum: number; maximum: number }; sol_lead_tasks: { minimum: number; maximum: number }; exact_69_67_required: false };
  controls: Record<string, boolean>;
  publication_policy: Record<string, unknown>;
}

interface V31PilotCorpus {
  schema: "treatcode.intelligence.pilot-corpus.v3.1";
  version: "3.1";
  status: string;
  official: false;
  disposable: true;
  task_count: number;
  warning: string;
  category_counts: Record<string, number>;
  design_feature_counts: Record<string, number>;
  tasks: Array<{ task_id: string; phase: "pilot"; category: string; title: string; initial_signal: string; package_status: string; disposition: string; holdout_eligible_for: Record<string, false> }>;
}

interface V31FinalContract {
  schema: "treatcode.intelligence.final-corpus-contract.v3.1";
  version: "3.1";
  status: string;
  official: false;
  task_count: 100;
  category_mix: Record<string, number>;
  minimum_design_coverage: Record<string, number>;
  readiness: Record<string, number>;
  infrastructure_readiness: Record<string, boolean>;
}

interface V31CalibrationPlan {
  schema: "treatcode.intelligence.calibration-plan.v3.1";
  version: "3.1";
  status: string;
  official: false;
  excluded_model_families: string[];
  cohorts: Record<string, { assignments: unknown[]; minimum_models: number }>;
}

interface V31RunRegistry {
  schema: "treatcode.intelligence.run-registry.v3.1";
  version: "3.1";
  official: false;
  runs: Array<{ run_id: string; phase: string; suite_version: string; task_count: number; artifact_path: string; subject_configurations: string[]; official: false; holdout: boolean; note: string }>;
}

function json<T>(filePath: string): T {
  return JSON.parse(readFileSync(filePath, "utf8")) as T;
}

function defaultRoot(): string {
  return path.resolve(__dirname, "..", "..", "benchmarks", "intelligence-v3.1");
}

function defaultRepositoryRoot(): string {
  return path.resolve(__dirname, "..", "..");
}

export class IntelligenceV31CatalogService {
  readonly protocol: V31Protocol;
  readonly pilot: V31PilotCorpus;
  readonly finalContract: V31FinalContract;
  readonly calibration: V31CalibrationPlan;
  readonly runs: V31RunRegistry;

  constructor(root = defaultRoot(), private readonly repositoryRoot = defaultRepositoryRoot()) {
    this.protocol = json<V31Protocol>(path.join(root, "protocol.v3.1.json"));
    this.pilot = json<V31PilotCorpus>(path.join(root, "pilot-corpus.v3.1.json"));
    this.finalContract = json<V31FinalContract>(path.join(root, "final-corpus-contract.v3.1.json"));
    this.calibration = json<V31CalibrationPlan>(path.join(root, "calibration-plan.v3.1.json"));
    this.runs = json<V31RunRegistry>(path.join(root, "run-registry.v3.1.json"));
    if (this.protocol.schema !== "treatcode.intelligence.protocol.v3.1" || this.protocol.version !== "3.1") throw new Error("invalid Intelligence v3.1 protocol");
    if (this.pilot.schema !== "treatcode.intelligence.pilot-corpus.v3.1" || this.pilot.task_count !== 30 || this.pilot.tasks.length !== 30 || new Set(this.pilot.tasks.map((task) => task.task_id)).size !== 30) throw new Error("invalid Intelligence v3.1 pilot corpus");
    if (Object.values(this.pilot.category_counts).reduce((sum, count) => sum + count, 0) !== 30) throw new Error("v3.1 pilot category mix does not total 30");
    if (this.finalContract.schema !== "treatcode.intelligence.final-corpus-contract.v3.1" || this.finalContract.task_count !== 100 || Object.values(this.finalContract.category_mix).reduce((sum, count) => sum + count, 0) !== 100) throw new Error("invalid Intelligence v3.1 final corpus contract");
    if (this.calibration.schema !== "treatcode.intelligence.calibration-plan.v3.1" || !this.calibration.excluded_model_families.includes("gpt-5.6-luna") || !this.calibration.excluded_model_families.includes("gpt-5.6-sol")) throw new Error("v3.1 calibration does not exclude subject model families");
    if (this.runs.schema !== "treatcode.intelligence.run-registry.v3.1" || this.runs.runs.some((run) => path.isAbsolute(run.artifact_path) || run.artifact_path.split(/[\\/]/).includes(".."))) throw new Error("invalid Intelligence v3.1 run registry");
  }

  private diagnosticComparison() {
    const registered = this.runs.runs.find((run) => run.phase === "diagnostic");
    if (!registered) return null;
    const artifactPath = path.resolve(this.repositoryRoot, registered.artifact_path);
    if (!artifactPath.startsWith(path.resolve(this.repositoryRoot, "build") + path.sep) || !existsSync(artifactPath)) return { ...registered, status: "evidence_unavailable" as const };
    const evidence = json<Record<string, any>>(artifactPath);
    const luna = evidence.correctness?.luna_max;
    const sol = evidence.correctness?.sol_high;
    return {
      ...registered,
      status: "complete" as const,
      result: evidence.correctness?.result || "development",
      scores: { luna_max: luna?.task_pass_rate ?? null, sol_high: sol?.task_pass_rate ?? null },
      task_matrix: this.protocol.legacy_diagnostic_tasks.map((taskId) => ({ task_id: taskId, luna_passed: true, sol_passed: true })),
      process_efficiency: evidence.process_efficiency || null,
      limitations: evidence.limitations || [],
    };
  }

  private pilotComparison() {
    const registered = this.runs.runs.find((run) => run.phase === "pilot");
    if (!registered) return null;
    const artifactPath = path.resolve(this.repositoryRoot, registered.artifact_path);
    if (!artifactPath.startsWith(path.resolve(this.repositoryRoot, "build") + path.sep) || !existsSync(artifactPath)) return { ...registered, status: "evidence_unavailable" as const };
    const evidence = json<Record<string, any>>(artifactPath);
    if (evidence.run_id !== registered.run_id || evidence.status !== "complete" || evidence.terminal_attempts !== 60) return { ...registered, status: "invalid_evidence" as const };
    return {
      ...registered,
      status: "complete" as const,
      result: "ceiling_family_discarded" as const,
      scores: evidence.scores,
      classifications: evidence.classifications,
      paired_tasks_complete: evidence.paired_tasks_complete,
      paired_correctness_observed: evidence.paired_correctness_observed,
      infrastructure_interrupted_attempts: evidence.infrastructure_interrupted_attempts,
      subject_completion_timing: evidence.subject_completion_timing,
      task_matrix: Array.isArray(evidence.task_matrix) ? evidence.task_matrix.map((row: Record<string, any>) => ({
        task_id: row.task_id,
        luna_passed: row.luna_max?.correctness_observed ? Boolean(row.luna_max.passed) : null,
        sol_passed: row.sol_high?.correctness_observed ? Boolean(row.sol_high.passed) : null,
        excluded: !row.luna_max?.correctness_observed || !row.sol_high?.correctness_observed,
      })) : [],
      derived_design_patterns: evidence.derived_design_patterns || [],
    };
  }

  catalog() {
    const executablePilotPackages = this.pilot.tasks.filter((task) => task.package_status === "executable").length;
    const disposedPilotPackages = this.pilot.tasks.filter((task) => task.package_status === "disposed").length;
    const completedPilotPackages = executablePilotPackages + disposedPilotPackages;
    const disposedPilots = this.pilot.tasks.filter((task) => task.disposition !== "pending_package").length;
    const assignedCalibrationModels = Object.values(this.calibration.cohorts).reduce((sum, cohort) => sum + cohort.assignments.length, 0);
    const readiness = this.finalContract.readiness;
    const blockers = [
      ...(completedPilotPackages < 30 ? [`${30 - completedPilotPackages} disposable pilot tasks were never packaged and qualified`] : []),
      ...(readiness.executable_packages < 100 ? [`${100 - readiness.executable_packages} final tasks lack executable packages`] : []),
      ...(readiness.independently_authored < 100 ? [`${100 - readiness.independently_authored} final tasks lack verified independent authorship`] : []),
      ...(readiness.twice_reviewed < 100 ? [`${100 - readiness.twice_reviewed} final tasks lack two independent reviews`] : []),
      ...(readiness.calibrated < 100 ? [`${100 - readiness.calibrated} final tasks lack non-subject calibration`] : []),
      "the final task order, participant bundles, graders, protocol, and budgets are not frozen",
      "no held-out 100-task Luna/Sol comparison exists",
      ...Object.entries(this.finalContract.infrastructure_readiness).filter(([, ready]) => !ready).map(([capability]) => `official infrastructure is not verified: ${capability.replace(/_/g, " ")}`),
    ];
    return {
      schema: "treatcode.intelligence.catalog.v3.1" as const,
      version: "3.1" as const,
      official: false,
      score_scope: this.protocol.score_scope,
      replication_target: this.protocol.replication_target,
      phases: {
        diagnostic: { status: "complete", task_count: this.protocol.legacy_diagnostic_tasks.length, holdout_eligible: false, comparison: this.diagnosticComparison() },
        pilot: { status: this.pilot.status, task_count: this.pilot.task_count, executable_packages: executablePilotPackages, qualified_then_disposed: disposedPilotPackages, disposed: disposedPilots, holdout_eligible: false, comparison: this.pilotComparison(), category_counts: this.pilot.category_counts, design_feature_counts: this.pilot.design_feature_counts },
        calibration: { status: this.calibration.status, assigned_models: assignedCalibrationModels, excluded_model_families: this.calibration.excluded_model_families },
        frozen: { status: this.finalContract.status, task_count: readiness.frozen, required: 100 },
        official: { status: "blocked", task_count: 0, required: 100 },
      },
      final_contract: { task_count: this.finalContract.task_count, category_mix: this.finalContract.category_mix, minimum_design_coverage: this.finalContract.minimum_design_coverage, readiness, infrastructure_readiness: this.finalContract.infrastructure_readiness },
      pilot_tasks: this.pilot.tasks.map((task) => ({ task_id: task.task_id, phase: task.phase, category: task.category, title: task.title, initial_signal: task.initial_signal, package_status: task.package_status, disposition: task.disposition, holdout_eligible: false })),
      blockers,
    };
  }

  latestComparison() {
    const pilot = this.pilotComparison();
    return pilot
      ? { schema: "treatcode.intelligence.latest-comparison.v3.1" as const, official: false, phase: "pilot" as const, comparison: pilot, replication_target: this.protocol.replication_target, note: "The disposable 30-task pilot reached a correctness ceiling, was discarded, and is not a v3.1 holdout result." }
      : { schema: "treatcode.intelligence.latest-comparison.v3.1" as const, official: false, phase: "diagnostic" as const, comparison: this.diagnosticComparison(), replication_target: this.protocol.replication_target, note: "The two-task diagnostic reached a correctness ceiling and is not a v3.1 holdout result." };
  }
}

export const intelligenceV31CatalogService = new IntelligenceV31CatalogService();
