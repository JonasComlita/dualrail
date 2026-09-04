import { createHash, verify } from "node:crypto";

export const INTELLIGENCE_V3_PROVENANCE_SCHEMA = "treatcode.intelligence.provenance-bundle.v3" as const;

interface AuthorPayload {
  role: "author";
  task_id: string;
  identity_id: string;
  participant_bundle_hash: string;
  grader_bundle_hash: string;
  independently_created: true;
  no_subject_rollout_access: true;
  subject_model_families_excluded: string[];
  conflict_disclosure: string;
  completed_at: string;
}

interface ReviewPayload {
  role: "reviewer";
  task_id: string;
  identity_id: string;
  participant_bundle_hash: string;
  grader_bundle_hash: string;
  independent_of_author: true;
  independent_of_subject_models: true;
  conflict_disclosure: string;
  checklist: {
    symptom_is_appropriately_underspecified: true;
    reference_behavior_is_correct: true;
    hidden_grader_is_behavioral_and_adversarial: true;
    participant_bundle_has_no_grader_or_answer_leakage: true;
    budgets_are_feasible: true;
    task_requires_repository_level_reasoning: true;
  };
  decision: "approve" | "reject";
  reviewed_at: string;
}

interface SignedAttestation<T> {
  identity_id: string;
  algorithm: "ed25519";
  payload: T;
  signature_base64: string;
}

export interface IntelligenceV3ProvenanceBundle {
  schema: typeof INTELLIGENCE_V3_PROVENANCE_SCHEMA;
  version: 3;
  task_id: string;
  participant_bundle_hash: string;
  grader_bundle_hash: string;
  author: SignedAttestation<AuthorPayload>;
  reviews: Array<SignedAttestation<ReviewPayload>>;
}

const HASH_PATTERN = /^(?:sha256:)?[a-f0-9]{64}$/i;

function canonical(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(canonical);
  if (value && typeof value === "object") return Object.fromEntries(Object.entries(value as Record<string, unknown>).sort(([left], [right]) => left.localeCompare(right)).map(([key, item]) => [key, canonical(item)]));
  return value;
}

export function canonicalProvenancePayload(payload: AuthorPayload | ReviewPayload): string {
  return JSON.stringify(canonical(payload));
}

function validDate(value: string): boolean {
  return typeof value === "string" && !Number.isNaN(Date.parse(value));
}

function verifyAttestation<T extends AuthorPayload | ReviewPayload>(attestation: SignedAttestation<T>, trustedKeys: Record<string, string>, issues: string[], label: string): void {
  if (!attestation.identity_id || attestation.identity_id !== attestation.payload.identity_id) issues.push(`${label} identity does not match its signed payload`);
  if (attestation.algorithm !== "ed25519") issues.push(`${label} must use ed25519`);
  const trustedKey = trustedKeys[attestation.identity_id];
  if (!trustedKey) {
    issues.push(`${label} identity is not in the trusted contributor registry`);
    return;
  }
  let signature: Buffer;
  try {
    signature = Buffer.from(attestation.signature_base64, "base64");
    if (signature.length === 0) throw new Error("empty signature");
  } catch {
    issues.push(`${label} signature is not valid base64`);
    return;
  }
  try {
    if (!verify(null, Buffer.from(canonicalProvenancePayload(attestation.payload)), trustedKey, signature)) issues.push(`${label} signature verification failed`);
  } catch {
    issues.push(`${label} trusted key or signature is invalid`);
  }
}

export function verifyIntelligenceV3ProvenanceBundle(bundle: IntelligenceV3ProvenanceBundle, trustedKeys: Record<string, string>, expectedSubjectModelFamilies: string[], minimumReviewers = 2) {
  const issues: string[] = [];
  if (bundle.schema !== INTELLIGENCE_V3_PROVENANCE_SCHEMA || bundle.version !== 3 || !/^TC-V3-\d{3}$/.test(bundle.task_id)) issues.push("unsupported provenance schema, version, or task id");
  if (!HASH_PATTERN.test(bundle.participant_bundle_hash) || !HASH_PATTERN.test(bundle.grader_bundle_hash)) issues.push("provenance bundle lacks valid package hashes");
  const author = bundle.author.payload;
  verifyAttestation(bundle.author, trustedKeys, issues, "author attestation");
  if (author.role !== "author" || author.task_id !== bundle.task_id || author.participant_bundle_hash !== bundle.participant_bundle_hash || author.grader_bundle_hash !== bundle.grader_bundle_hash) issues.push("author attestation is not bound to the exact task packages");
  if (author.independently_created !== true || author.no_subject_rollout_access !== true || !author.conflict_disclosure || !validDate(author.completed_at)) issues.push("author independence, conflict, or completion attestation is incomplete");
  const excludedFamilies = new Set(author.subject_model_families_excluded);
  for (const family of expectedSubjectModelFamilies) if (!excludedFamilies.has(family)) issues.push(`author did not attest independence from subject model family ${family}`);

  if (!Array.isArray(bundle.reviews) || bundle.reviews.length < minimumReviewers) issues.push(`provenance requires at least ${minimumReviewers} reviews`);
  const reviewerIds = new Set<string>();
  for (const [index, review] of bundle.reviews.entries()) {
    const label = `review ${index + 1}`;
    verifyAttestation(review, trustedKeys, issues, label);
    const payload = review.payload;
    if (reviewerIds.has(review.identity_id)) issues.push("reviewer identities must be distinct");
    reviewerIds.add(review.identity_id);
    if (review.identity_id === bundle.author.identity_id) issues.push("author cannot review their own task");
    if (payload.role !== "reviewer" || payload.task_id !== bundle.task_id || payload.participant_bundle_hash !== bundle.participant_bundle_hash || payload.grader_bundle_hash !== bundle.grader_bundle_hash) issues.push(`${label} is not bound to the exact task packages`);
    if (payload.independent_of_author !== true || payload.independent_of_subject_models !== true || !payload.conflict_disclosure || !validDate(payload.reviewed_at)) issues.push(`${label} independence, conflict, or date attestation is incomplete`);
    if (payload.decision !== "approve") issues.push(`${label} did not approve the task`);
    if (!payload.checklist || Object.values(payload.checklist).some((item) => item !== true)) issues.push(`${label} checklist is incomplete`);
  }
  const bundleHash = createHash("sha256").update(JSON.stringify(canonical(bundle))).digest("hex");
  return {
    schema: "treatcode.intelligence.provenance-verification.v3" as const,
    task_id: bundle.task_id,
    passed: issues.length === 0,
    author_id: bundle.author.identity_id,
    reviewer_ids: [...reviewerIds].sort(),
    provenance_bundle_hash: bundleHash,
    issues: [...new Set(issues)],
  };
}
