import { verify } from "node:crypto";

export interface IntelligenceV31InfrastructurePayload {
  schema: "treatcode.intelligence.infrastructure-attestation-payload.v3.1";
  version: "3.1";
  provider_id: string;
  key_id: string;
  verified_at: string;
  expires_at: string;
  network_isolation: { enforcement: "verified_os_or_container"; provider: string; evidence_sha256: string };
  resource_isolation: { enforcement: "verified_os_or_container"; provider: string; evidence_sha256: string };
  evidence_store: { enforcement: "verified_append_only"; provider: string; evidence_sha256: string };
}

export interface IntelligenceV31InfrastructureAttestation {
  schema: "treatcode.intelligence.infrastructure-attestation.v3.1";
  version: "3.1";
  payload: IntelligenceV31InfrastructurePayload;
  signature_base64: string;
}

export interface IntelligenceV31TrustedKeyRegistry {
  schema: "treatcode.intelligence.trusted-infrastructure-keys.v3.1";
  version: "3.1";
  status: string;
  official: boolean;
  keys: Array<{ key_id: string; provider_id: string; algorithm: "Ed25519"; capabilities: Array<"infrastructure" | "model_execution">; public_key_pem: string; not_before: string; not_after: string }>;
}

export interface IntelligenceV31ModelExecutionPayload {
  schema: "treatcode.intelligence.model-execution-attestation-payload.v3.1";
  version: "3.1";
  provider_id: string;
  key_id: string;
  issued_at: string;
  expires_at: string;
  released_at: string;
  phase: "calibration" | "subject" | "development";
  run_id: string;
  task_id: string;
  model: string;
  reasoning_effort: string;
  participant_bundle_sha256: string;
  suite_sha256: string | null;
  cohort: "weak" | "medium" | "frontier" | null;
  configuration: string | null;
  run_number: 1 | 2 | 3 | null;
  fresh_context: true;
  cross_task_memory_disabled: true;
  attempt_count: 1;
  tool_trace_sha256: string;
}

export interface IntelligenceV31ModelExecutionAttestation {
  schema: "treatcode.intelligence.model-execution-attestation.v3.1";
  version: "3.1";
  payload: IntelligenceV31ModelExecutionPayload;
  signature_base64: string;
}

const HASH = /^[a-f0-9]{64}$/;
const SAFE = /^[A-Za-z0-9._-]+$/;

function canonicalize(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(canonicalize);
  if (value && typeof value === "object") return Object.fromEntries(Object.entries(value as Record<string, unknown>).sort(([a], [b]) => a.localeCompare(b)).map(([key, item]) => [key, canonicalize(item)]));
  return value;
}

export function canonicalInfrastructurePayload(payload: IntelligenceV31InfrastructurePayload): string {
  return JSON.stringify(canonicalize(payload));
}

export function canonicalModelExecutionPayload(payload: IntelligenceV31ModelExecutionPayload): string {
  return JSON.stringify(canonicalize(payload));
}

export function intelligenceV31AttemptId(payload: Pick<IntelligenceV31ModelExecutionPayload, "phase" | "run_id" | "task_id" | "model" | "reasoning_effort" | "cohort" | "configuration" | "run_number">): string {
  const subject = payload.phase === "subject";
  const development = payload.phase === "development";
  if (subject || development ? payload.cohort !== null || payload.configuration !== null || payload.run_number !== null : !payload.cohort || !payload.configuration || ![1, 2, 3].includes(payload.run_number || 0)) throw new Error("model execution calibration slot is invalid");
  const slot = subject || development ? `${payload.model}-${payload.reasoning_effort}` : `${payload.configuration}-${payload.cohort}-${payload.run_number}`;
  const attemptId = `${payload.run_id}-${payload.task_id}-${slot}`;
  if (!SAFE.test(attemptId)) throw new Error("derived model execution attempt id is unsafe");
  return attemptId;
}

function trustedKey(registry: IntelligenceV31TrustedKeyRegistry, providerId: string, keyId: string, at: number, capability: "infrastructure" | "model_execution") {
  if (registry.schema !== "treatcode.intelligence.trusted-infrastructure-keys.v3.1" || registry.version !== "3.1" || registry.status !== "configured" || registry.official !== true) throw new Error("trusted infrastructure key registry is not configured for official use");
  const key = registry.keys.find((item) => item.key_id === keyId && item.provider_id === providerId && item.algorithm === "Ed25519");
  if (!key) throw new Error("attestation signing key is not trusted");
  if (!key.capabilities?.includes(capability)) throw new Error(`attestation signing key is not trusted for ${capability}`);
  const keyStart = Date.parse(key.not_before);
  const keyEnd = Date.parse(key.not_after);
  if (![keyStart, keyEnd].every(Number.isFinite) || at < keyStart || at > keyEnd) throw new Error("attestation signing key is outside its validity window");
  return { key, keyStart, keyEnd };
}

export function verifyIntelligenceV31InfrastructureAttestation(attestation: IntelligenceV31InfrastructureAttestation, registry: IntelligenceV31TrustedKeyRegistry, now = new Date()): IntelligenceV31InfrastructurePayload {
  if (attestation.schema !== "treatcode.intelligence.infrastructure-attestation.v3.1" || attestation.version !== "3.1") throw new Error("infrastructure attestation schema is invalid");
  const payload = attestation.payload;
  if (!payload || payload.schema !== "treatcode.intelligence.infrastructure-attestation-payload.v3.1" || payload.version !== "3.1" || !SAFE.test(payload.provider_id) || !SAFE.test(payload.key_id)) throw new Error("infrastructure attestation payload is invalid");
  const timestamp = now.getTime();
  const verifiedAt = Date.parse(payload.verified_at);
  const expiresAt = Date.parse(payload.expires_at);
  const { key, keyStart, keyEnd } = trustedKey(registry, payload.provider_id, payload.key_id, verifiedAt, "infrastructure");
  if (![verifiedAt, expiresAt, keyStart, keyEnd].every(Number.isFinite) || verifiedAt > timestamp || expiresAt <= timestamp || verifiedAt < keyStart || verifiedAt > keyEnd || expiresAt > keyEnd) throw new Error("infrastructure attestation or signing key is outside its validity window");
  for (const proof of [payload.network_isolation, payload.resource_isolation, payload.evidence_store]) {
    if (!SAFE.test(proof.provider) || !HASH.test(proof.evidence_sha256)) throw new Error("infrastructure attestation contains invalid proof metadata");
  }
  let signature: Buffer;
  try { signature = Buffer.from(attestation.signature_base64, "base64"); } catch { throw new Error("infrastructure attestation signature encoding is invalid"); }
  if (signature.length !== 64 || !verify(null, Buffer.from(canonicalInfrastructurePayload(payload)), key.public_key_pem, signature)) throw new Error("infrastructure attestation signature verification failed");
  return payload;
}


export function verifyIntelligenceV31ModelExecutionAttestation(attestation: IntelligenceV31ModelExecutionAttestation, registry: IntelligenceV31TrustedKeyRegistry, expected: Omit<IntelligenceV31ModelExecutionPayload, "schema" | "version" | "provider_id" | "key_id" | "issued_at" | "expires_at">, now = new Date()): IntelligenceV31ModelExecutionPayload {
  if (attestation.schema !== "treatcode.intelligence.model-execution-attestation.v3.1" || attestation.version !== "3.1") throw new Error("model execution attestation schema is invalid");
  const payload = attestation.payload;
  if (!payload || payload.schema !== "treatcode.intelligence.model-execution-attestation-payload.v3.1" || payload.version !== "3.1" || !SAFE.test(payload.provider_id) || !SAFE.test(payload.key_id) || !SAFE.test(payload.run_id) || !SAFE.test(payload.reasoning_effort) || !/^TC-V31-FINAL-\d{3}$/.test(payload.task_id) || !/^[a-z0-9]+(?:[.-][a-z0-9]+)*$/.test(payload.model) || !HASH.test(payload.participant_bundle_sha256) || (payload.suite_sha256 !== null && !HASH.test(payload.suite_sha256)) || !HASH.test(payload.tool_trace_sha256)) throw new Error("model execution attestation payload is invalid");
  if (payload.fresh_context !== true || payload.cross_task_memory_disabled !== true || payload.attempt_count !== 1 || !["calibration", "subject"].includes(payload.phase)) throw new Error("model execution controls are invalid");
  const expectedFields = ["released_at", "phase", "run_id", "task_id", "model", "reasoning_effort", "participant_bundle_sha256", "suite_sha256", "cohort", "configuration", "run_number", "fresh_context", "cross_task_memory_disabled", "attempt_count", "tool_trace_sha256"] as const;
  if (expectedFields.some((field) => payload[field] !== expected[field])) throw new Error("model execution attestation does not match the released attempt and tool trace");
  intelligenceV31AttemptId(payload);
  const timestamp = now.getTime();
  const issuedAt = Date.parse(payload.issued_at);
  const expiresAt = Date.parse(payload.expires_at);
  const releasedAt = Date.parse(payload.released_at);
  const { key, keyStart, keyEnd } = trustedKey(registry, payload.provider_id, payload.key_id, issuedAt, "model_execution");
  if (![issuedAt, expiresAt, releasedAt].every(Number.isFinite) || issuedAt < releasedAt || issuedAt > timestamp || expiresAt <= timestamp || issuedAt < keyStart || expiresAt > keyEnd) throw new Error("model execution attestation, task release, or signing key is outside its validity window");
  let signature: Buffer;
  try { signature = Buffer.from(attestation.signature_base64, "base64"); } catch { throw new Error("model execution attestation signature encoding is invalid"); }
  if (signature.length !== 64 || !verify(null, Buffer.from(canonicalModelExecutionPayload(payload)), key.public_key_pem, signature)) throw new Error("model execution attestation signature verification failed");
  return payload;
}
