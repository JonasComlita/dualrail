export const INTELLIGENCE_V3_DISCUSSION_RUBRIC_SCHEMA = "treatcode.intelligence.discussion-rubric.v3" as const;
export const INTELLIGENCE_V3_DISCUSSION_CALIBRATION_SCHEMA = "treatcode.intelligence.discussion-calibration.v3" as const;

export interface IntelligenceV3DiscussionRubric {
  schema: typeof INTELLIGENCE_V3_DISCUSSION_RUBRIC_SCHEMA;
  version: 3;
  rubric_id: string;
  status: "draft_pending_human_calibration" | "calibrated" | "frozen";
  included_in_executable_score: false;
  length: { minimum_words: number; maximum_words: number };
  rating: {
    minimum_raters: number;
    model_identity_blinded: true;
    executable_score_blinded: true;
    integer_scale: { minimum: number; maximum: number };
    dimensions: Array<{ id: string; label: string }>;
  };
  calibration: {
    minimum_examples: number;
    minimum_examples_per_score_band: number;
    score_bands_percent: { low_max: number; middle_max: number };
    agreement_metric: "quadratic_weighted_kappa";
    minimum_dimension_agreement: number;
    minimum_mean_agreement: number;
    adjudicate_disagreements_at_or_above_points: number;
    requires_human_approval: true;
  };
}

export interface IntelligenceV3DiscussionRating {
  rater_id: string;
  human_attested: true;
  model_identity_blinded: true;
  executable_score_blinded: true;
  scores: Record<string, number>;
}

export interface IntelligenceV3DiscussionCalibrationExample {
  artifact_hash: string;
  word_count: number;
  ratings: [IntelligenceV3DiscussionRating, IntelligenceV3DiscussionRating];
}

export interface IntelligenceV3DiscussionCalibrationInput {
  schema: typeof INTELLIGENCE_V3_DISCUSSION_CALIBRATION_SCHEMA;
  version: 3;
  rubric_id: string;
  examples: IntelligenceV3DiscussionCalibrationExample[];
}

const HASH_PATTERN = /^(?:sha256:)?[a-f0-9]{64}$/i;

function round(value: number, digits = 4): number {
  const factor = 10 ** digits;
  return Math.round(value * factor) / factor;
}

function mean(values: number[]): number {
  return values.length === 0 ? 0 : values.reduce((sum, value) => sum + value, 0) / values.length;
}

function validateRating(rubric: IntelligenceV3DiscussionRubric, rating: IntelligenceV3DiscussionRating, label: string, issues: string[]): void {
  if (!rating.rater_id || rating.human_attested !== true || rating.model_identity_blinded !== true || rating.executable_score_blinded !== true) issues.push(`${label} lacks blinded human-rater attestation`);
  const dimensions = new Set(rubric.rating.dimensions.map((dimension) => dimension.id));
  if (Object.keys(rating.scores).length !== dimensions.size || Object.keys(rating.scores).some((id) => !dimensions.has(id))) issues.push(`${label} does not score exactly the frozen rubric dimensions`);
  for (const id of dimensions) {
    const score = rating.scores[id];
    if (!Number.isInteger(score) || score < rubric.rating.integer_scale.minimum || score > rubric.rating.integer_scale.maximum) issues.push(`${label} has an invalid ${id} score`);
  }
}

function weightedKappa(left: number[], right: number[], minimum: number, maximum: number): number {
  if (left.length !== right.length || left.length === 0) return 0;
  const categories = maximum - minimum + 1;
  const leftCounts = Array<number>(categories).fill(0);
  const rightCounts = Array<number>(categories).fill(0);
  let observedDisagreement = 0;
  const maxDistanceSquared = Math.max(1, (maximum - minimum) ** 2);
  for (let index = 0; index < left.length; index += 1) {
    leftCounts[left[index] - minimum] += 1;
    rightCounts[right[index] - minimum] += 1;
    observedDisagreement += ((left[index] - right[index]) ** 2) / maxDistanceSquared;
  }
  observedDisagreement /= left.length;
  let expectedDisagreement = 0;
  for (let leftIndex = 0; leftIndex < categories; leftIndex += 1) {
    for (let rightIndex = 0; rightIndex < categories; rightIndex += 1) {
      const weight = ((leftIndex - rightIndex) ** 2) / maxDistanceSquared;
      expectedDisagreement += weight * (leftCounts[leftIndex] / left.length) * (rightCounts[rightIndex] / right.length);
    }
  }
  if (expectedDisagreement === 0) return observedDisagreement === 0 ? 1 : 0;
  return 1 - observedDisagreement / expectedDisagreement;
}

function ratingPercent(rubric: IntelligenceV3DiscussionRubric, rating: IntelligenceV3DiscussionRating): number {
  const maximum = rubric.rating.dimensions.length * rubric.rating.integer_scale.maximum;
  const minimum = rubric.rating.dimensions.length * rubric.rating.integer_scale.minimum;
  const total = rubric.rating.dimensions.reduce((sum, dimension) => sum + rating.scores[dimension.id], 0);
  return ((total - minimum) / Math.max(1, maximum - minimum)) * 100;
}

export function calibrateIntelligenceV3DiscussionRubric(rubric: IntelligenceV3DiscussionRubric, input: IntelligenceV3DiscussionCalibrationInput) {
  const issues: string[] = [];
  if (rubric.schema !== INTELLIGENCE_V3_DISCUSSION_RUBRIC_SCHEMA || rubric.version !== 3 || rubric.included_in_executable_score !== false) issues.push("invalid discussion rubric");
  if (input.schema !== INTELLIGENCE_V3_DISCUSSION_CALIBRATION_SCHEMA || input.version !== 3 || input.rubric_id !== rubric.rubric_id) issues.push("calibration input does not bind to this rubric");
  if (input.examples.length < rubric.calibration.minimum_examples) issues.push(`requires at least ${rubric.calibration.minimum_examples} calibration examples`);
  const artifactHashes = new Set<string>();
  const scoreBands = { low: 0, middle: 0, high: 0 };
  for (const [index, example] of input.examples.entries()) {
    if (!HASH_PATTERN.test(example.artifact_hash) || artifactHashes.has(example.artifact_hash)) issues.push(`example ${index + 1} has missing or repeated artifact evidence`);
    artifactHashes.add(example.artifact_hash);
    if (!Number.isInteger(example.word_count) || example.word_count < rubric.length.minimum_words || example.word_count > rubric.length.maximum_words) issues.push(`example ${index + 1} violates discussion length control`);
    if (!Array.isArray(example.ratings) || example.ratings.length !== 2 || example.ratings[0].rater_id === example.ratings[1].rater_id) issues.push(`example ${index + 1} requires two distinct raters`);
    for (const [ratingIndex, rating] of example.ratings.entries()) validateRating(rubric, rating, `example ${index + 1} rating ${ratingIndex + 1}`, issues);
    const percent = mean(example.ratings.map((rating) => ratingPercent(rubric, rating)));
    if (percent <= rubric.calibration.score_bands_percent.low_max) scoreBands.low += 1;
    else if (percent <= rubric.calibration.score_bands_percent.middle_max) scoreBands.middle += 1;
    else scoreBands.high += 1;
  }
  for (const [band, count] of Object.entries(scoreBands)) if (count < rubric.calibration.minimum_examples_per_score_band) issues.push(`${band} score band lacks calibration anchors`);
  const dimensionAgreement: Record<string, number> = {};
  for (const dimension of rubric.rating.dimensions) {
    const agreement = weightedKappa(input.examples.map((example) => example.ratings[0].scores[dimension.id]), input.examples.map((example) => example.ratings[1].scores[dimension.id]), rubric.rating.integer_scale.minimum, rubric.rating.integer_scale.maximum);
    dimensionAgreement[dimension.id] = round(agreement);
    if (agreement < rubric.calibration.minimum_dimension_agreement) issues.push(`${dimension.id} agreement is below the calibration threshold`);
  }
  const meanAgreement = round(mean(Object.values(dimensionAgreement)));
  if (meanAgreement < rubric.calibration.minimum_mean_agreement) issues.push("mean rubric agreement is below the calibration threshold");
  return {
    schema: "treatcode.intelligence.discussion-calibration-report.v3" as const,
    rubric_id: rubric.rubric_id,
    status: issues.length === 0 ? "ready_for_human_approval" as const : "needs_calibration" as const,
    official: false as const,
    example_count: input.examples.length,
    score_bands: scoreBands,
    dimension_agreement: dimensionAgreement,
    mean_agreement: meanAgreement,
    issues: [...new Set(issues)],
  };
}

export function scoreIntelligenceV3Discussion(rubric: IntelligenceV3DiscussionRubric, text: string, ratings: IntelligenceV3DiscussionRating[], calibrationEvidenceHash: string) {
  const issues: string[] = [];
  const wordCount = text.trim() === "" ? 0 : text.trim().split(/\s+/).length;
  if (wordCount < rubric.length.minimum_words || wordCount > rubric.length.maximum_words) issues.push(`discussion must contain ${rubric.length.minimum_words}-${rubric.length.maximum_words} words`);
  if (!HASH_PATTERN.test(calibrationEvidenceHash) || rubric.status === "draft_pending_human_calibration") issues.push("rubric lacks approved human-calibration evidence");
  if (ratings.length < rubric.rating.minimum_raters || new Set(ratings.map((rating) => rating.rater_id)).size < rubric.rating.minimum_raters) issues.push(`discussion requires ${rubric.rating.minimum_raters} distinct blinded human raters`);
  ratings.forEach((rating, index) => validateRating(rubric, rating, `rating ${index + 1}`, issues));
  const maximum = rubric.rating.dimensions.length * rubric.rating.integer_scale.maximum;
  const totals = ratings.map((rating) => rubric.rating.dimensions.reduce((sum, dimension) => sum + rating.scores[dimension.id], 0));
  const disagreements = rubric.rating.dimensions.filter((dimension) => Math.max(...ratings.map((rating) => rating.scores[dimension.id])) - Math.min(...ratings.map((rating) => rating.scores[dimension.id])) >= rubric.calibration.adjudicate_disagreements_at_or_above_points).map((dimension) => dimension.id);
  if (disagreements.length > 0) issues.push(`adjudication required for: ${disagreements.join(", ")}`);
  return {
    schema: "treatcode.intelligence.discussion-score.v3" as const,
    rubric_id: rubric.rubric_id,
    valid: issues.length === 0,
    included_in_executable_score: false as const,
    word_count: wordCount,
    rater_count: new Set(ratings.map((rating) => rating.rater_id)).size,
    human_score: ratings.length === 0 ? null : round(mean(totals), 2),
    maximum_score: maximum,
    percent: ratings.length === 0 ? null : round((mean(totals) / maximum) * 100, 2),
    calibration_evidence_hash: calibrationEvidenceHash,
    adjudication_dimensions: disagreements,
    issues: [...new Set(issues)],
  };
}
