export const INTELLIGENCE_V31_CALIBRATION_OBSERVATION_SCHEMA = "treatcode.intelligence.calibration-observation.v3.1" as const;
export const INTELLIGENCE_V31_CALIBRATION_REPORT_SCHEMA = "treatcode.intelligence.calibration-report.v3.1" as const;

export type IntelligenceV31CalibrationCohort = "weak" | "medium" | "frontier";

export interface IntelligenceV31CalibrationObservation {
  schema: typeof INTELLIGENCE_V31_CALIBRATION_OBSERVATION_SCHEMA;
  version: "3.1";
  task_id: string;
  participant_bundle_hash: string;
  attempt_id: string;
  cohort: IntelligenceV31CalibrationCohort;
  model_family: string;
  configuration: string;
  run_number: 1 | 2 | 3;
  fresh_context: true;
  subject_family_excluded: true;
  model_execution_verified: true;
  execution_attestation_sha256: string;
  network_isolation_verified: true;
  resource_limits_verified: true;
  append_only_evidence_verified: true;
  passed: boolean;
  completion_status: "completed" | "timeout" | "invalid_submission" | "infrastructure_interruption";
  released_at: string;
  submitted_at: string | null;
  evidence_hash: string;
  official: false;
}

export interface IntelligenceV31CalibrationTaskInput {
  task_id: string;
  participant_bundle_hash: string;
  author_model_family: string;
  calibration_model_families: [string, string];
  calibration_configurations: Record<IntelligenceV31CalibrationCohort, [string, string]>;
  qualification: { reference_passed: boolean; starter_failed: boolean; mutants_caught: number; mutants_total: number };
}

export interface IntelligenceV31TaskCalibration {
  task_id: string;
  disposition: "accept" | "revise" | "insufficient_data";
  observation_count: number;
  cohort_pass_rates: Record<IntelligenceV31CalibrationCohort, number>;
  item_discrimination: number;
  cohort_order_valid: boolean;
  ceiling_effect: boolean;
  floor_effect: boolean;
  reasons: string[];
}

export interface IntelligenceV31CalibrationReport {
  schema: typeof INTELLIGENCE_V31_CALIBRATION_REPORT_SCHEMA;
  version: "3.1";
  official: false;
  status: "insufficient_data" | "needs_revision" | "ready_to_freeze";
  task_count: number;
  observation_count: number;
  accepted_tasks: number;
  revise_tasks: number;
  insufficient_tasks: number;
  ceiling_fraction: number;
  floor_fraction: number;
  data_issues: string[];
  blockers: string[];
  tasks: IntelligenceV31TaskCalibration[];
}

const HASH = /^[a-f0-9]{64}$/;
const SAFE_ID = /^[A-Za-z0-9._-]+$/;
const CANONICAL_MODEL_FAMILY = /^[a-z0-9]+(?:[.-][a-z0-9]+)*$/;
const COHORTS: IntelligenceV31CalibrationCohort[] = ["weak", "medium", "frontier"];

export function canonicalIntelligenceV31ModelFamily(value: string): string {
  return `${value || ""}`.trim().toLowerCase().split(/[/:]/).filter(Boolean).at(-1) || "";
}

export function isIntelligenceV31SubjectFamily(value: string): boolean {
  return /^gpt-5\.6-(?:luna|sol)(?:$|[-.])/.test(canonicalIntelligenceV31ModelFamily(value));
}

function round(value: number, digits = 4): number {
  const scale = 10 ** digits;
  return Math.round(value * scale) / scale;
}

function average(values: number[]): number {
  return values.length ? values.reduce((sum, value) => sum + value, 0) / values.length : 0;
}

function pearson(left: number[], right: number[]): number {
  if (left.length !== right.length || left.length < 3) return 0;
  const leftMean = average(left);
  const rightMean = average(right);
  let numerator = 0;
  let leftVariance = 0;
  let rightVariance = 0;
  for (let index = 0; index < left.length; index += 1) {
    const a = left[index] - leftMean;
    const b = right[index] - rightMean;
    numerator += a * b;
    leftVariance += a * a;
    rightVariance += b * b;
  }
  return leftVariance && rightVariance ? numerator / Math.sqrt(leftVariance * rightVariance) : 0;
}

function observationKey(observation: IntelligenceV31CalibrationObservation): string {
  return `${observation.task_id}\0${observation.cohort}\0${observation.model_family}\0${observation.configuration}\0${observation.run_number}`;
}

export function calibrateIntelligenceV31Candidates(input: {
  tasks: IntelligenceV31CalibrationTaskInput[];
  observations: IntelligenceV31CalibrationObservation[];
  minimum_item_discrimination?: number;
  maximum_ceiling_fraction?: number;
  maximum_floor_fraction?: number;
}): IntelligenceV31CalibrationReport {
  const minimumDiscrimination = input.minimum_item_discrimination ?? 0.1;
  const maximumCeilingFraction = input.maximum_ceiling_fraction ?? 0.15;
  const maximumFloorFraction = input.maximum_floor_fraction ?? 0.15;
  const issues: string[] = [];
  const taskIds = new Set(input.tasks.map((task) => task.task_id));
  if (input.tasks.length !== 100 || taskIds.size !== 100) issues.push("calibration requires exactly 100 distinct candidate tasks");
  const taskById = new Map(input.tasks.map((task) => [task.task_id, task]));
  const observationKeys = new Set<string>();
  const attemptIds = new Set<string>();
  const evidenceHashes = new Set<string>();
  const executionAttestationHashes = new Set<string>();

  for (const task of input.tasks) {
    if (!/^TC-V31-FINAL-\d{3}$/.test(task.task_id) || !HASH.test(task.participant_bundle_hash)) issues.push(`${task.task_id}: invalid task identity or participant hash`);
    if (!CANONICAL_MODEL_FAMILY.test(task.author_model_family) || task.author_model_family !== canonicalIntelligenceV31ModelFamily(task.author_model_family)) issues.push(`${task.task_id}: author model family identifier is not canonical`);
    if (isIntelligenceV31SubjectFamily(task.author_model_family)) issues.push(`${task.task_id}: subject family authored a final candidate`);
    if (!Array.isArray(task.calibration_model_families) || task.calibration_model_families.length !== 2 || new Set(task.calibration_model_families).size !== 2 || task.calibration_model_families.some((family) => !CANONICAL_MODEL_FAMILY.test(family) || isIntelligenceV31SubjectFamily(family) || canonicalIntelligenceV31ModelFamily(family) === canonicalIntelligenceV31ModelFamily(task.author_model_family))) issues.push(`${task.task_id}: deterministic non-author calibration-family assignment is invalid`);
    for (const cohort of COHORTS) {
      const configurations = task.calibration_configurations?.[cohort];
      if (!Array.isArray(configurations) || configurations.length !== 2 || configurations.some((configuration, index) => !SAFE_ID.test(configuration) || !configuration.startsWith(`${task.calibration_model_families[index]}-`))) issues.push(`${task.task_id}: ${cohort} calibration configurations are invalid`);
    }
  }
  for (const observation of input.observations) {
    const task = taskById.get(observation.task_id);
    if (observation.schema !== INTELLIGENCE_V31_CALIBRATION_OBSERVATION_SCHEMA || observation.version !== "3.1" || observation.official !== false) issues.push(`${observation.task_id}: invalid observation schema`);
    if (!task) issues.push(`${observation.task_id}: observation references an unknown task`);
    else if (observation.participant_bundle_hash !== task.participant_bundle_hash) issues.push(`${observation.task_id}: observation is bound to a different participant bundle`);
    if (!SAFE_ID.test(observation.attempt_id) || attemptIds.has(observation.attempt_id)) issues.push(`${observation.task_id}: attempt id is invalid or reused`);
    attemptIds.add(observation.attempt_id);
    if (!HASH.test(observation.evidence_hash) || evidenceHashes.has(observation.evidence_hash)) issues.push(`${observation.task_id}: evidence hash is invalid or reused`);
    evidenceHashes.add(observation.evidence_hash);
    if (!CANONICAL_MODEL_FAMILY.test(observation.model_family) || observation.model_family !== canonicalIntelligenceV31ModelFamily(observation.model_family)) issues.push(`${observation.task_id}: model family identifier is not canonical`);
    if (isIntelligenceV31SubjectFamily(observation.model_family) || observation.subject_family_excluded !== true) issues.push(`${observation.task_id}: subject family exclusion failed`);
    if (observation.model_execution_verified !== true || !HASH.test(observation.execution_attestation_sha256) || executionAttestationHashes.has(observation.execution_attestation_sha256)) issues.push(`${observation.task_id}: provider-signed model execution identity is unverified or reused`);
    executionAttestationHashes.add(observation.execution_attestation_sha256);
    if (observation.network_isolation_verified !== true || observation.resource_limits_verified !== true || observation.append_only_evidence_verified !== true) issues.push(`${observation.task_id}: calibration infrastructure isolation evidence is unverified`);
    if (task && canonicalIntelligenceV31ModelFamily(observation.model_family) === canonicalIntelligenceV31ModelFamily(task.author_model_family)) issues.push(`${observation.task_id}: author-family calibration contamination`);
    if (task && !task.calibration_model_families.includes(observation.model_family)) issues.push(`${observation.task_id}: observation model family is not assigned to this task`);
    if (task) {
      const familyIndex = task.calibration_model_families.indexOf(observation.model_family);
      if (familyIndex < 0 || observation.configuration !== task.calibration_configurations?.[observation.cohort]?.[familyIndex]) issues.push(`${observation.task_id}: observation configuration is not assigned to this cohort and family`);
    }
    if (observation.fresh_context !== true || ![1, 2, 3].includes(observation.run_number)) issues.push(`${observation.task_id}: observation is not a fresh valid run`);
    const releasedAt = Date.parse(observation.released_at);
    const submittedAt = observation.submitted_at === null ? Number.NaN : Date.parse(observation.submitted_at);
    if (!Number.isFinite(releasedAt) || (observation.submitted_at !== null && (!Number.isFinite(submittedAt) || submittedAt < releasedAt))) issues.push(`${observation.task_id}: observation timestamps are invalid or reversed`);
    if (observation.completion_status === "infrastructure_interruption") issues.push(`${observation.task_id}: infrastructure interruption cannot be used as calibration evidence`);
    if (observation.passed && observation.completion_status !== "completed") issues.push(`${observation.task_id}: non-completed observation cannot pass`);
    const key = observationKey(observation);
    if (observationKeys.has(key)) issues.push(`${observation.task_id}: duplicate configuration run`);
    observationKeys.add(key);
  }

  const taskReports: IntelligenceV31TaskCalibration[] = input.tasks.map((task) => {
    const observations = input.observations.filter((item) => item.task_id === task.task_id && item.completion_status !== "infrastructure_interruption");
    const reasons: string[] = [];
    const rates = Object.fromEntries(COHORTS.map((cohort) => [cohort, round(average(observations.filter((item) => item.cohort === cohort).map((item) => Number(item.passed))))])) as Record<IntelligenceV31CalibrationCohort, number>;
    let complete = true;
    for (const cohort of COHORTS) {
      const cohortObservations = observations.filter((item) => item.cohort === cohort);
      const families = new Set(cohortObservations.map((item) => item.model_family));
      if (families.size !== 2 || task.calibration_model_families.some((family) => !families.has(family))) complete = false;
      for (const family of families) {
        const familyRuns = cohortObservations.filter((item) => item.model_family === family);
        if (new Set(familyRuns.map((item) => item.configuration)).size !== 1 || new Set(familyRuns.map((item) => item.run_number)).size !== 3) complete = false;
      }
      if (cohortObservations.length !== 6) complete = false;
    }
    if (!complete || observations.length !== 18) reasons.push("task lacks the complete two-family, three-cohort, three-run matrix");
    if (!task.qualification.reference_passed || !task.qualification.starter_failed || task.qualification.mutants_total < 1 || task.qualification.mutants_caught !== task.qualification.mutants_total) reasons.push("repository package qualification is incomplete or failed");
    const cohortOrderValid = rates.weak <= rates.medium && rates.medium <= rates.frontier;
    if (!cohortOrderValid) reasons.push("cohort pass rates are not ordered weak-to-frontier");
    const ability = observations.map((item) => COHORTS.indexOf(item.cohort));
    const itemDiscrimination = round(pearson(observations.map((item) => Number(item.passed)), ability));
    if (itemDiscrimination < minimumDiscrimination) reasons.push("item discrimination is below the protocol minimum");
    const insufficient = !complete || observations.length !== 18;
    return {
      task_id: task.task_id,
      disposition: insufficient ? "insufficient_data" : reasons.length ? "revise" : "accept",
      observation_count: observations.length,
      cohort_pass_rates: rates,
      item_discrimination: itemDiscrimination,
      cohort_order_valid: cohortOrderValid,
      ceiling_effect: complete && rates.weak >= 0.9,
      floor_effect: complete && rates.frontier <= 0.1,
      reasons,
    };
  });
  const accepted = taskReports.filter((task) => task.disposition === "accept").length;
  const revise = taskReports.filter((task) => task.disposition === "revise").length;
  const insufficient = taskReports.filter((task) => task.disposition === "insufficient_data").length;
  const ceilingFraction = round(taskReports.filter((task) => task.ceiling_effect).length / Math.max(1, taskReports.length));
  const floorFraction = round(taskReports.filter((task) => task.floor_effect).length / Math.max(1, taskReports.length));
  const blockers = [...new Set(issues)];
  if (ceilingFraction > maximumCeilingFraction) blockers.push("aggregate ceiling fraction exceeds 15%");
  if (floorFraction > maximumFloorFraction) blockers.push("aggregate floor fraction exceeds 15%");
  if (revise) blockers.push(`${revise} tasks require revision or replacement`);
  if (insufficient) blockers.push(`${insufficient} tasks have insufficient calibration evidence`);
  return {
    schema: INTELLIGENCE_V31_CALIBRATION_REPORT_SCHEMA,
    version: "3.1",
    official: false,
    status: issues.length || insufficient ? "insufficient_data" : blockers.length ? "needs_revision" : "ready_to_freeze",
    task_count: input.tasks.length,
    observation_count: input.observations.length,
    accepted_tasks: accepted,
    revise_tasks: revise,
    insufficient_tasks: insufficient,
    ceiling_fraction: ceilingFraction,
    floor_fraction: floorFraction,
    data_issues: [...new Set(issues)],
    blockers,
    tasks: taskReports,
  };
}
