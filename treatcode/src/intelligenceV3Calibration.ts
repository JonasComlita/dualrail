import type { IntelligenceV3Protocol } from "./intelligenceV3";

export const INTELLIGENCE_V3_CALIBRATION_INPUT_SCHEMA = "treatcode.intelligence.calibration-observations.v3" as const;
export const INTELLIGENCE_V3_CALIBRATION_REPORT_SCHEMA = "treatcode.intelligence.calibration-report.v3" as const;

export interface IntelligenceV3CalibrationObservation {
  task_id: string;
  cohort: string;
  model_id: string;
  configuration_id: string;
  run_index: number;
  run_id: string;
  attempt_count: 1;
  fresh_context: true;
  passed: boolean;
  task_release_hash: string;
  artifact_hash: string;
}

export interface IntelligenceV3CalibrationInput {
  schema: typeof INTELLIGENCE_V3_CALIBRATION_INPUT_SCHEMA;
  version: 3;
  candidate_manifest_hash: string;
  task_ids: string[];
  observations: IntelligenceV3CalibrationObservation[];
}

export type IntelligenceV3CalibrationDisposition = "accept" | "revise" | "insufficient_data";

export interface IntelligenceV3TaskCalibration {
  task_id: string;
  disposition: IntelligenceV3CalibrationDisposition;
  cohort_pass_rates: Record<string, number>;
  observations: number;
  item_discrimination: number;
  ceiling_effect: boolean;
  floor_effect: boolean;
  cohort_order_valid: boolean;
  reasons: string[];
}

export interface IntelligenceV3CalibrationReport {
  schema: typeof INTELLIGENCE_V3_CALIBRATION_REPORT_SCHEMA;
  version: 3;
  status: "insufficient_data" | "needs_revision" | "ready_for_human_review";
  official: false;
  candidate_manifest_hash: string;
  task_count: number;
  observation_count: number;
  run_unit_count: number;
  cohort_model_counts: Record<string, number>;
  cohort_configuration_counts: Record<string, number>;
  accepted_tasks: number;
  revise_tasks: number;
  insufficient_tasks: number;
  ceiling_fraction: number;
  floor_fraction: number;
  data_issues: string[];
  blockers: string[];
  tasks: IntelligenceV3TaskCalibration[];
}

const HASH_PATTERN = /^(?:sha256:)?[a-f0-9]{64}$/i;

function round(value: number, digits = 4): number {
  const scale = 10 ** digits;
  return Math.round(value * scale) / scale;
}

function average(values: number[]): number {
  return values.length === 0 ? 0 : values.reduce((sum, value) => sum + value, 0) / values.length;
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
  if (leftVariance === 0 || rightVariance === 0) return 0;
  return numerator / Math.sqrt(leftVariance * rightVariance);
}

function runUnitKey(observation: IntelligenceV3CalibrationObservation): string {
  return `${observation.cohort}\0${observation.model_id}\0${observation.configuration_id}\0${observation.run_index}\0${observation.run_id}`;
}

function configurationKey(observation: IntelligenceV3CalibrationObservation): string {
  return `${observation.cohort}\0${observation.model_id}\0${observation.configuration_id}`;
}

function observationKey(observation: IntelligenceV3CalibrationObservation): string {
  return `${runUnitKey(observation)}\0${observation.task_id}`;
}

export function calibrateIntelligenceV3Suite(protocol: IntelligenceV3Protocol, input: IntelligenceV3CalibrationInput): IntelligenceV3CalibrationReport {
  const dataIssues: string[] = [];
  if (input.schema !== INTELLIGENCE_V3_CALIBRATION_INPUT_SCHEMA || input.version !== 3) dataIssues.push("unsupported calibration input schema or version");
  if (!HASH_PATTERN.test(input.candidate_manifest_hash)) dataIssues.push("candidate manifest hash is missing or invalid");
  const taskIds = new Set(input.task_ids);
  if (taskIds.size !== input.task_ids.length) dataIssues.push("calibration task ids contain duplicates");
  if (taskIds.size < protocol.official_requirements.minimum_distinct_tasks) dataIssues.push(`calibration requires at least ${protocol.official_requirements.minimum_distinct_tasks} distinct tasks`);

  const requiredCohorts = new Set(protocol.calibration.required_cohorts);
  const observationKeys = new Set<string>();
  const artifactHashes = new Set<string>();
  const runIdentityOwners = new Map<string, string>();
  const modelCohorts = new Map<string, string>();
  const runUnits = new Map<string, IntelligenceV3CalibrationObservation[]>();
  const configurations = new Map<string, IntelligenceV3CalibrationObservation[]>();
  const modelsByCohort = new Map<string, Set<string>>();
  const configurationsByCohort = new Map<string, Set<string>>();
  const releaseHashByTask = new Map<string, string>();

  for (const observation of input.observations) {
    if (!taskIds.has(observation.task_id)) dataIssues.push(`observation references unknown task ${observation.task_id}`);
    if (!requiredCohorts.has(observation.cohort)) dataIssues.push(`observation uses undeclared cohort ${observation.cohort}`);
    if (!observation.model_id || !observation.configuration_id || !observation.run_id) dataIssues.push(`observation for ${observation.task_id} has incomplete model/run identity`);
    if (!Number.isSafeInteger(observation.run_index) || observation.run_index < 1) dataIssues.push(`observation for ${observation.task_id} has invalid run index`);
    if (observation.attempt_count !== 1 || observation.fresh_context !== true) dataIssues.push(`observation for ${observation.task_id} is not a fresh single attempt`);
    if (!HASH_PATTERN.test(observation.task_release_hash) || !HASH_PATTERN.test(observation.artifact_hash)) dataIssues.push(`observation for ${observation.task_id} lacks release or artifact evidence`);
    if (artifactHashes.has(observation.artifact_hash)) dataIssues.push(`artifact evidence is reused by more than one calibration attempt`);
    artifactHashes.add(observation.artifact_hash);
    const modelCohort = modelCohorts.get(observation.model_id);
    if (modelCohort && modelCohort !== observation.cohort) dataIssues.push(`model ${observation.model_id} is assigned to multiple calibration cohorts`);
    else modelCohorts.set(observation.model_id, observation.cohort);
    const priorReleaseHash = releaseHashByTask.get(observation.task_id);
    if (priorReleaseHash && priorReleaseHash !== observation.task_release_hash) dataIssues.push(`task ${observation.task_id} changed during calibration`);
    else releaseHashByTask.set(observation.task_id, observation.task_release_hash);
    const key = observationKey(observation);
    if (observationKeys.has(key)) dataIssues.push(`duplicate calibration observation for ${observation.task_id}`);
    observationKeys.add(key);
    const unit = runUnitKey(observation);
    const runOwner = `${observation.cohort}\0${observation.model_id}\0${observation.configuration_id}\0${observation.run_index}`;
    const priorRunOwner = runIdentityOwners.get(observation.run_id);
    if (priorRunOwner && priorRunOwner !== runOwner) dataIssues.push(`run id ${observation.run_id} is reused by multiple configurations`);
    else runIdentityOwners.set(observation.run_id, runOwner);
    const configuration = configurationKey(observation);
    (runUnits.get(unit) || (runUnits.set(unit, []), runUnits.get(unit)!)).push(observation);
    (configurations.get(configuration) || (configurations.set(configuration, []), configurations.get(configuration)!)).push(observation);
    const cohortModels = modelsByCohort.get(observation.cohort) || new Set<string>();
    cohortModels.add(observation.model_id);
    modelsByCohort.set(observation.cohort, cohortModels);
    const cohortConfigurations = configurationsByCohort.get(observation.cohort) || new Set<string>();
    cohortConfigurations.add(configuration);
    configurationsByCohort.set(observation.cohort, cohortConfigurations);
  }

  for (const cohort of protocol.calibration.required_cohorts) {
    const modelCount = modelsByCohort.get(cohort)?.size || 0;
    if (modelCount < protocol.calibration.minimum_models_per_cohort) dataIssues.push(`cohort ${cohort} requires ${protocol.calibration.minimum_models_per_cohort} models; found ${modelCount}`);
  }
  for (const [key, observations] of configurations) {
    const runIndexes = new Set(observations.map((item) => item.run_index));
    const runIds = new Set(observations.map((item) => item.run_id));
    if (runIndexes.size < protocol.calibration.minimum_runs_per_configuration || runIds.size < protocol.calibration.minimum_runs_per_configuration) dataIssues.push(`configuration ${key.replace(/\0/g, "/")} lacks required independent runs`);
  }
  for (const [key, observations] of runUnits) {
    const observedTasks = new Set(observations.map((item) => item.task_id));
    if (observedTasks.size !== taskIds.size || [...taskIds].some((taskId) => !observedTasks.has(taskId))) dataIssues.push(`run ${key.replace(/\0/g, "/")} did not attempt the complete identical task set`);
  }

  const runTotals = new Map<string, number>();
  for (const [key, observations] of runUnits) runTotals.set(key, observations.filter((item) => item.passed).length);
  const tasks: IntelligenceV3TaskCalibration[] = [];
  for (const taskId of input.task_ids) {
    const observations = input.observations.filter((item) => item.task_id === taskId);
    const cohortPassRates: Record<string, number> = {};
    const reasons: string[] = [];
    let complete = true;
    for (const cohort of protocol.calibration.required_cohorts) {
      const cohortObservations = observations.filter((item) => item.cohort === cohort);
      if (cohortObservations.length === 0) complete = false;
      const modelRates = [...(modelsByCohort.get(cohort) || new Set<string>())].map((modelId) => {
        const modelObservations = cohortObservations.filter((item) => item.model_id === modelId);
        return average(modelObservations.map((item) => Number(item.passed)));
      });
      cohortPassRates[cohort] = round(average(modelRates));
    }
    const itemValues = observations.map((item) => Number(item.passed));
    const restScores = observations.map((item) => (runTotals.get(runUnitKey(item))! - Number(item.passed)) / Math.max(1, taskIds.size - 1));
    const itemDiscrimination = round(pearson(itemValues, restScores));
    const weakRate = cohortPassRates[protocol.calibration.required_cohorts[0]];
    const frontierRate = cohortPassRates[protocol.calibration.required_cohorts[protocol.calibration.required_cohorts.length - 1]];
    const ceilingEffect = complete && weakRate >= protocol.statistics.ceiling_threshold;
    const floorEffect = complete && frontierRate <= protocol.statistics.floor_threshold;
    const cohortOrderValid = protocol.calibration.required_cohorts.every((cohort, index, cohorts) => index === 0 || cohortPassRates[cohorts[index - 1]] <= cohortPassRates[cohort]);
    if (!complete || observations.length !== runUnits.size) reasons.push("task lacks the complete calibration run matrix");
    if (ceilingEffect) reasons.push("weak cohort pass rate meets the ceiling threshold");
    if (floorEffect) reasons.push("frontier cohort pass rate meets the floor threshold");
    if (itemDiscrimination < protocol.statistics.minimum_item_discrimination) reasons.push("item discrimination is below the protocol minimum");
    if (!cohortOrderValid) reasons.push("cohort pass rates do not preserve weak-to-frontier ordering");
    const disposition: IntelligenceV3CalibrationDisposition = !complete || observations.length !== runUnits.size
      ? "insufficient_data"
      : reasons.length > 0 ? "revise" : "accept";
    tasks.push({ task_id: taskId, disposition, cohort_pass_rates: cohortPassRates, observations: observations.length, item_discrimination: itemDiscrimination, ceiling_effect: ceilingEffect, floor_effect: floorEffect, cohort_order_valid: cohortOrderValid, reasons });
  }

  const acceptedTasks = tasks.filter((task) => task.disposition === "accept").length;
  const reviseTasks = tasks.filter((task) => task.disposition === "revise").length;
  const insufficientTasks = tasks.filter((task) => task.disposition === "insufficient_data").length;
  const ceilingFraction = round(tasks.filter((task) => task.ceiling_effect).length / Math.max(1, tasks.length));
  const floorFraction = round(tasks.filter((task) => task.floor_effect).length / Math.max(1, tasks.length));
  const blockers = [...new Set(dataIssues)];
  if (ceilingFraction > protocol.statistics.maximum_ceiling_fraction) blockers.push("suite ceiling fraction exceeds the protocol maximum");
  if (floorFraction > protocol.statistics.maximum_floor_fraction) blockers.push("suite floor fraction exceeds the protocol maximum");
  if (reviseTasks > 0) blockers.push(`${reviseTasks} tasks require revision or removal`);
  if (insufficientTasks > 0) blockers.push(`${insufficientTasks} tasks have insufficient calibration data`);
  const status: IntelligenceV3CalibrationReport["status"] = dataIssues.length > 0 || insufficientTasks > 0
    ? "insufficient_data"
    : blockers.length > 0 ? "needs_revision" : "ready_for_human_review";
  return {
    schema: INTELLIGENCE_V3_CALIBRATION_REPORT_SCHEMA,
    version: 3,
    status,
    official: false,
    candidate_manifest_hash: input.candidate_manifest_hash,
    task_count: tasks.length,
    observation_count: input.observations.length,
    run_unit_count: runUnits.size,
    cohort_model_counts: Object.fromEntries(protocol.calibration.required_cohorts.map((cohort) => [cohort, modelsByCohort.get(cohort)?.size || 0])),
    cohort_configuration_counts: Object.fromEntries(protocol.calibration.required_cohorts.map((cohort) => [cohort, configurationsByCohort.get(cohort)?.size || 0])),
    accepted_tasks: acceptedTasks,
    revise_tasks: reviseTasks,
    insufficient_tasks: insufficientTasks,
    ceiling_fraction: ceilingFraction,
    floor_fraction: floorFraction,
    data_issues: [...new Set(dataIssues)],
    blockers: [...new Set(blockers)],
    tasks,
  };
}
