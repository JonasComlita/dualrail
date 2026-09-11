import { createHash } from "node:crypto";
import { existsSync, readdirSync, readFileSync } from "node:fs";
import path from "node:path";
import { compareIntelligenceV31Dimensions, validateIntelligenceV31TracksManifest, type IntelligenceV31TracksManifest } from "./intelligenceV31Tracks";

interface V31Protocol {
  schema: "treatcode.intelligence.protocol.v3.1";
  version: "3.1";
  status: string;
  score_scope: string;
  legacy_diagnostic_tasks: string[];
  tracks_manifest: string;
  execution_modes: string[];
  score_dimensions: {
    headline: string;
    separate: string[];
    no_iq_composite: boolean;
    discussion_in_executable_score: boolean;
    missing_data_policy: string;
  };
  dimension_observation_schema: string;
  evaluation_goals: { primary: string[]; secondary: string[]; not_a_single_iq_score: true };
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
  official: boolean;
  task_count: 100;
  category_mix: Record<string, number>;
  track_mix: Record<string, number>;
  minimum_track_coverage: Record<string, number>;
  minimum_design_coverage: Record<string, number>;
  authoring_recovery: { shard_count: number; tasks_per_shard: number; immutable_private_checkpoint_required: boolean; canonical_task_hashes_required: boolean; checkpoint_publication: string; resumable_private_ingest: boolean };
  readiness: Record<string, number>;
  infrastructure_readiness: Record<string, boolean>;
  execution_identity_readiness: { provider_signed_calibration_observations: number; required_calibration_observations: number; provider_signed_subject_attempts: number; required_subject_attempts: number };
}

interface V31AuthorShardPlan {
  schema: "treatcode.intelligence.author-shard-plan.v3.1";
  version: "3.1";
  task_count: 100;
  shard_count: 20;
  tasks_per_shard: 5;
  track_mix: Record<string, number>;
  shards: Array<{ shard_id: string; first_task_number: number; categories: string[]; tracks: string[] }>;
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
  official: boolean;
  runs: Array<{ run_id: string; phase: string; suite_version: string; task_count: number; artifact_path: string; subject_configurations: string[]; official: boolean; holdout: boolean; note: string }>;
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
  readonly tracks: IntelligenceV31TracksManifest;
  readonly pilot: V31PilotCorpus;
  readonly finalContract: V31FinalContract;
  readonly authorShardPlan: V31AuthorShardPlan;
  readonly calibration: V31CalibrationPlan;
  readonly runs: V31RunRegistry;

  constructor(private readonly root = defaultRoot(), private readonly repositoryRoot = defaultRepositoryRoot()) {
    this.protocol = json<V31Protocol>(path.join(root, "protocol.v3.1.json"));
    const tracksManifestPath = this.protocol.tracks_manifest;
    if (!tracksManifestPath || path.isAbsolute(tracksManifestPath) || tracksManifestPath.split(/[\\/]/).includes("..")) throw new Error("invalid Intelligence v3.1 tracks manifest path");
    this.tracks = json<IntelligenceV31TracksManifest>(path.join(root, tracksManifestPath));
    const observationSchemaPath = this.protocol.dimension_observation_schema;
    if (!observationSchemaPath || path.isAbsolute(observationSchemaPath) || observationSchemaPath.split(/[\\/]/).includes("..")) throw new Error("invalid Intelligence v3.1 score observation schema path");
    const observationSchema = json<Record<string, unknown>>(path.join(root, observationSchemaPath));
    this.pilot = json<V31PilotCorpus>(path.join(root, "pilot-corpus.v3.1.json"));
    this.finalContract = json<V31FinalContract>(path.join(root, "final-corpus-contract.v3.1.json"));
    this.authorShardPlan = json<V31AuthorShardPlan>(path.join(root, "author-shard-plan.v3.1.json"));
    this.calibration = json<V31CalibrationPlan>(path.join(root, "calibration-plan.v3.1.json"));
    this.runs = json<V31RunRegistry>(path.join(root, "run-registry.v3.1.json"));
    if (this.protocol.schema !== "treatcode.intelligence.protocol.v3.1" || this.protocol.version !== "3.1") throw new Error("invalid Intelligence v3.1 protocol");
    if (observationSchema.$id !== "https://treatcode.local/schemas/intelligence/score-observation.v3.1.json" || observationSchema.properties && (observationSchema.properties as Record<string, unknown>).track === undefined) throw new Error("invalid Intelligence v3.1 score observation schema");
    if (!this.protocol.execution_modes.includes("scalar") || !this.protocol.execution_modes.includes("repository") || !this.protocol.execution_modes.includes("terminal")) throw new Error("v3.1 protocol does not expose all execution modes");
    if (this.protocol.score_dimensions.headline !== "correctness" || !this.protocol.score_dimensions.no_iq_composite || this.protocol.score_dimensions.discussion_in_executable_score) throw new Error("v3.1 protocol must keep dimensions separate and prohibit an IQ composite");
    const trackIssues = validateIntelligenceV31TracksManifest(this.tracks);
    if (trackIssues.length) throw new Error(`invalid Intelligence v3.1 tracks manifest: ${trackIssues.join("; ")}`);
    if (this.pilot.schema !== "treatcode.intelligence.pilot-corpus.v3.1" || this.pilot.task_count !== 30 || this.pilot.tasks.length !== 30 || new Set(this.pilot.tasks.map((task) => task.task_id)).size !== 30) throw new Error("invalid Intelligence v3.1 pilot corpus");
    if (Object.values(this.pilot.category_counts).reduce((sum, count) => sum + count, 0) !== 30) throw new Error("v3.1 pilot category mix does not total 30");
    if (this.finalContract.schema !== "treatcode.intelligence.final-corpus-contract.v3.1" || this.finalContract.task_count !== 100 || Object.values(this.finalContract.category_mix).reduce((sum, count) => sum + count, 0) !== 100 || Object.values(this.finalContract.track_mix).reduce((sum, count) => sum + count, 0) !== 100 || Object.entries(this.finalContract.minimum_track_coverage).some(([track, count]) => !(track in this.finalContract.track_mix) || count > this.finalContract.track_mix[track])) throw new Error("invalid Intelligence v3.1 final corpus contract");
    if (this.authorShardPlan.schema !== "treatcode.intelligence.author-shard-plan.v3.1" || this.authorShardPlan.shard_count !== 20 || this.authorShardPlan.tasks_per_shard !== 5 || this.authorShardPlan.shards.length !== 20 || JSON.stringify(this.authorShardPlan.track_mix) !== JSON.stringify(this.finalContract.track_mix) || this.authorShardPlan.shards.some((shard) => !Array.isArray(shard.tracks) || shard.tracks.length !== 5)) throw new Error("invalid Intelligence v3.1 author shard plan");
    if (this.calibration.schema !== "treatcode.intelligence.calibration-plan.v3.1" || !this.calibration.excluded_model_families.includes("gpt-5.6-luna") || !this.calibration.excluded_model_families.includes("gpt-5.6-sol")) throw new Error("v3.1 calibration does not exclude subject model families");
    if (this.runs.schema !== "treatcode.intelligence.run-registry.v3.1" || this.runs.runs.some((run) => path.isAbsolute(run.artifact_path) || run.artifact_path.split(/[\\/]/).includes(".."))) throw new Error("invalid Intelligence v3.1 run registry");
  }

  private authoringProgress() {
    const receiptRoot = path.join(this.repositoryRoot, "build", "treatcode-plan-evidence", "P14", "author-shard-checkpoints");
    const planHash = createHash("sha256").update(readFileSync(path.join(this.root, "author-shard-plan.v3.1.json"))).digest("hex");
    let checkpointed = 0;
    let invalid = 0;
    for (const shard of this.authorShardPlan.shards) {
      const receiptPath = path.join(receiptRoot, `${shard.shard_id}.receipt.json`);
      if (!existsSync(receiptPath)) continue;
      try {
        const receipt = json<Record<string, any>>(receiptPath);
        const expectedIds = Array.from({ length: 5 }, (_, index) => `TC-V31-FINAL-${String(shard.first_task_number + index).padStart(3, "0")}`);
        const valid = receipt.schema === "treatcode.intelligence.author-shard-receipt.v3.1" && receipt.shard_id === shard.shard_id && receipt.shard_plan_sha256 === planHash && JSON.stringify(receipt.task_ids) === JSON.stringify(expectedIds) && /^[a-f0-9]{64}$/.test(receipt.contribution_sha256 || "") && /^[a-f0-9]{64}$/.test(receipt.canonical_tasks_sha256 || "");
        if (valid) checkpointed += 1;
        else invalid += 1;
      } catch { invalid += 1; }
    }
    return { status: invalid ? "invalid_evidence" as const : checkpointed === 20 ? "checkpointed" as const : checkpointed ? "in_progress" as const : "awaiting_shards" as const, shard_count: checkpointed, required_shards: 20, task_count: checkpointed * 5, required: 100, tasks_per_shard: 5, missing_shards: 20 - checkpointed - invalid, invalid_shards: invalid, track_mix: this.authorShardPlan.track_mix };
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
      dimensions: null,
      discrimination: { status: "ceiling", pair_count: this.protocol.legacy_diagnostic_tasks.length, discordant_pairs: 0, discordance_rate: 0, pass_pass_fraction: 1, fail_fail_fraction: 0, note: "The diagnostic tasks were solved by both models and are not suitable for ranking." },
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
    const pilotDimensionComparison = evidence.dimensions || (() => {
      const rows = Array.isArray(evidence.task_matrix) ? evidence.task_matrix : [];
      const observationsFor = (modelKey: "luna_max" | "sol_high") => rows
        .filter((row: Record<string, any>) => row[modelKey]?.correctness_observed)
        .map((row: Record<string, any>) => ({
          task_id: row.task_id,
          track: "repository_repair" as const,
          correctness: Boolean(row[modelKey].passed),
          wall_clock_ms: row[modelKey].subject_elapsed_ms,
          latency_budget_ms: 1_200_000,
        }));
      return compareIntelligenceV31Dimensions(observationsFor("luna_max"), observationsFor("sol_high"));
    })();
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
      dimensions: pilotDimensionComparison,
      discrimination: evidence.discrimination || {
        status: (evidence.classifications?.ceiling_pass_pass / Math.max(1, evidence.paired_correctness_observed) >= 0.9 ? "ceiling" : "separation_observed"),
        pair_count: evidence.paired_correctness_observed || 0,
        discordant_pairs: (evidence.classifications?.diagnostic_sol_only || 0) + (evidence.classifications?.diagnostic_luna_only || 0),
        discordance_rate: ((evidence.classifications?.diagnostic_sol_only || 0) + (evidence.classifications?.diagnostic_luna_only || 0)) / Math.max(1, evidence.paired_correctness_observed || 0),
        pass_pass_fraction: (evidence.classifications?.ceiling_pass_pass || 0) / Math.max(1, evidence.paired_correctness_observed || 0),
        fail_fail_fraction: (evidence.classifications?.floor_fail_fail || 0) / Math.max(1, evidence.paired_correctness_observed || 0),
        note: "Pilot evidence predates the explicit discrimination summary.",
      },
      task_matrix: Array.isArray(evidence.task_matrix) ? evidence.task_matrix.map((row: Record<string, any>) => ({
        task_id: row.task_id,
        luna_passed: row.luna_max?.correctness_observed ? Boolean(row.luna_max.passed) : null,
        sol_passed: row.sol_high?.correctness_observed ? Boolean(row.sol_high.passed) : null,
        excluded: !row.luna_max?.correctness_observed || !row.sol_high?.correctness_observed,
      })) : [],
      derived_design_patterns: evidence.derived_design_patterns || [],
    };
  }

  private officialComparison() {
    const registered = this.runs.runs.find((run) => run.phase === "official" && run.official && run.holdout);
    if (!registered) return null;
    const artifactPath = path.resolve(this.repositoryRoot, registered.artifact_path);
    if (!artifactPath.startsWith(path.resolve(this.repositoryRoot, "build") + path.sep) || !existsSync(artifactPath)) return { ...registered, status: "evidence_unavailable" as const };
    const evidence = json<Record<string, any>>(artifactPath);
    if (evidence.official !== true || evidence.complete !== true || evidence.task_count !== 100 || evidence.run_id !== registered.run_id || !Array.isArray(evidence.task_matrix) || evidence.task_matrix.length !== 100) return { ...registered, status: "invalid_evidence" as const };
    return { ...registered, status: "complete" as const, result: evidence.target_reproduced ? "replicated" as const : "replication_failed" as const, scores: { luna_max: evidence.luna_score, sol_high: evidence.sol_score }, sol_lead: evidence.sol_lead, target_reproduced: evidence.target_reproduced, paired_confidence_interval: evidence.paired_confidence_interval, exact_sign_test: evidence.exact_sign_test, dimensions: evidence.dimensions || null, discrimination: evidence.discrimination || null, task_matrix: evidence.task_matrix };
  }

  private developmentComparison() {
    const evidenceRoot = path.join(this.repositoryRoot, "build", "treatcode-plan-evidence", "P14");
    if (!existsSync(evidenceRoot)) return null;
    const candidates = readdirSync(evidenceRoot)
      .filter((file) => /^intelligence-v31-development-comparison-[A-Za-z0-9._-]+\.json$/.test(file))
      .map((file) => {
        const artifactPath = path.join(evidenceRoot, file);
        try {
          const evidence = json<Record<string, any>>(artifactPath);
          if (evidence.schema !== "treatcode.intelligence.development-comparison.v3.1" || evidence.version !== "3.1" || evidence.phase !== "development" || evidence.official !== false || !/^[A-Za-z0-9._-]+$/.test(evidence.run_id || "") || evidence.complete !== true || !Number.isSafeInteger(evidence.task_count) || evidence.task_count < 1 || !Array.isArray(evidence.task_matrix) || evidence.task_matrix.length !== evidence.task_count) return null;
          return { file, artifactPath, evidence };
        } catch {
          return null;
        }
      })
      .filter((candidate): candidate is { file: string; artifactPath: string; evidence: Record<string, any> } => candidate !== null)
      .sort((left, right) => String(left.evidence.generated_at || left.file).localeCompare(String(right.evidence.generated_at || right.file)));
    const latest = candidates[candidates.length - 1];
    if (!latest) return null;
    const evidence = latest.evidence;
    return {
      run_id: evidence.run_id,
      phase: "development" as const,
      suite_version: "3.1",
      task_count: evidence.task_count,
      artifact_path: path.relative(this.repositoryRoot, latest.artifactPath).split(path.sep).join("/"),
      official: false,
      holdout: false,
      status: "complete" as const,
      result: "development" as const,
      scores: { luna_max: evidence.luna_score, sol_high: evidence.sol_score },
      sol_lead: evidence.sol_lead,
      target_reproduced: false,
      paired_confidence_interval: evidence.paired_confidence_interval,
      exact_sign_test: evidence.exact_sign_test,
      dimensions: evidence.dimensions || null,
      discrimination: evidence.discrimination || null,
      task_matrix: evidence.task_matrix,
    };
  }

  catalog() {
    const executablePilotPackages = this.pilot.tasks.filter((task) => task.package_status === "executable").length;
    const qualifiedThenDisposedPilotTasks = this.pilot.tasks.filter((task) => task.disposition === "discarded_after_ceiling_analysis").length;
    const disposedPilotPackages = this.pilot.tasks.filter((task) => task.package_status === "disposed").length;
    const completedPilotPackages = executablePilotPackages + Math.max(disposedPilotPackages, qualifiedThenDisposedPilotTasks);
    const disposedPilots = this.pilot.tasks.filter((task) => task.disposition !== "pending_package").length;
    const assignedCalibrationModels = Object.values(this.calibration.cohorts).reduce((sum, cohort) => sum + cohort.assignments.length, 0);
    const readiness = this.finalContract.readiness;
    const authoring = this.authoringProgress();
    const officialComparison = this.officialComparison();
    const officialComplete = officialComparison?.status === "complete";
    const blockers = [
      ...(completedPilotPackages < 30 ? [`${30 - completedPilotPackages} disposable pilot tasks were never packaged and qualified`] : []),
      ...(readiness.executable_packages < 100 ? [`${100 - readiness.executable_packages} final tasks lack executable packages`] : []),
      ...(readiness.independently_authored < 100 ? [`${authoring.required_shards - authoring.shard_count} five-task authoring shards lack immutable checkpoints and verified independent authorship`] : []),
      ...(readiness.twice_reviewed < 100 ? [`${100 - readiness.twice_reviewed} final tasks lack two independent reviews`] : []),
      ...(readiness.calibrated < 100 ? [`${100 - readiness.calibrated} final tasks lack non-subject calibration`] : []),
      ...(readiness.frozen < 100 ? ["the final task order, participant bundles, graders, protocol, and budgets are not frozen"] : []),
      ...(!officialComplete ? ["no valid held-out 100-task Luna/Sol comparison exists"] : []),
      ...(this.finalContract.execution_identity_readiness.provider_signed_calibration_observations < this.finalContract.execution_identity_readiness.required_calibration_observations ? [`${this.finalContract.execution_identity_readiness.required_calibration_observations - this.finalContract.execution_identity_readiness.provider_signed_calibration_observations} calibration observations lack provider-signed model execution identity`] : []),
      ...(this.finalContract.execution_identity_readiness.provider_signed_subject_attempts < this.finalContract.execution_identity_readiness.required_subject_attempts ? [`${this.finalContract.execution_identity_readiness.required_subject_attempts - this.finalContract.execution_identity_readiness.provider_signed_subject_attempts} Luna/Sol attempts lack provider-signed model execution identity`] : []),
      ...Object.entries(this.finalContract.infrastructure_readiness).filter(([, ready]) => !ready).map(([capability]) => `official infrastructure is not verified: ${capability.replace(/_/g, " ")}`),
    ];
    return {
      schema: "treatcode.intelligence.catalog.v3.1" as const,
      version: "3.1" as const,
      official: this.finalContract.official && officialComplete,
      score_scope: this.protocol.score_scope,
      replication_target: this.protocol.replication_target,
      execution_modes: this.protocol.execution_modes,
      tracks: this.tracks.tracks,
      score_dimensions: this.tracks.score_dimensions,
      dimension_observation_schema: this.protocol.dimension_observation_schema,
      evaluation_goals: this.protocol.evaluation_goals,
      phases: {
        diagnostic: { status: "complete", task_count: this.protocol.legacy_diagnostic_tasks.length, holdout_eligible: false, comparison: this.diagnosticComparison() },
        pilot: { status: this.pilot.status, task_count: this.pilot.task_count, executable_packages: executablePilotPackages, qualified_then_disposed: qualifiedThenDisposedPilotTasks, disposed: disposedPilots, holdout_eligible: false, comparison: this.pilotComparison(), category_counts: this.pilot.category_counts, design_feature_counts: this.pilot.design_feature_counts },
        authoring,
        calibration: { status: this.calibration.status, assigned_models: assignedCalibrationModels, excluded_model_families: this.calibration.excluded_model_families },
        frozen: { status: this.finalContract.status, task_count: readiness.frozen, required: 100 },
        official: { status: officialComplete ? "complete" : "blocked", task_count: officialComplete ? 100 : 0, required: 100, comparison: officialComparison },
      },
      final_contract: { task_count: this.finalContract.task_count, category_mix: this.finalContract.category_mix, track_mix: this.finalContract.track_mix, minimum_track_coverage: this.finalContract.minimum_track_coverage, minimum_design_coverage: this.finalContract.minimum_design_coverage, authoring_recovery: this.finalContract.authoring_recovery, readiness, infrastructure_readiness: this.finalContract.infrastructure_readiness, execution_identity_readiness: this.finalContract.execution_identity_readiness },
      pilot_tasks: this.pilot.tasks.map((task) => ({ task_id: task.task_id, phase: task.phase, category: task.category, title: task.title, initial_signal: task.initial_signal, package_status: task.package_status, disposition: task.disposition, holdout_eligible: false })),
      blockers,
    };
  }

  latestComparison() {
    const official = this.officialComparison();
    const completedOfficial = official?.status === "complete" ? official : null;
    const development = this.developmentComparison();
    const pilot = this.pilotComparison();
    return completedOfficial
      ? { schema: "treatcode.intelligence.latest-comparison.v3.1" as const, official: true, phase: "official" as const, comparison: completedOfficial, replication_target: this.protocol.replication_target, note: completedOfficial.target_reproduced ? "The frozen one-shot v3.1 holdout met the preregistered directional target." : "The frozen one-shot v3.1 holdout missed the preregistered target and was not retuned or rerun." }
      : development
      ? { schema: "treatcode.intelligence.latest-comparison.v3.1" as const, official: false, phase: "development" as const, comparison: development, replication_target: this.protocol.replication_target, note: "This disposable paired development run is scored from sealed grades but is not an official holdout result." }
      : pilot
      ? { schema: "treatcode.intelligence.latest-comparison.v3.1" as const, official: false, phase: "pilot" as const, comparison: pilot, replication_target: this.protocol.replication_target, note: "The disposable 30-task pilot reached a correctness ceiling, was discarded, and is not a v3.1 holdout result." }
      : { schema: "treatcode.intelligence.latest-comparison.v3.1" as const, official: false, phase: "diagnostic" as const, comparison: this.diagnosticComparison(), replication_target: this.protocol.replication_target, note: "The two-task diagnostic reached a correctness ceiling and is not a v3.1 holdout result." };
  }
}

export const intelligenceV31CatalogService = new IntelligenceV31CatalogService();
