import assert from "node:assert/strict";
import { generateKeyPairSync, sign } from "node:crypto";
import { describe, test } from "bun:test";
import { canonicalInfrastructurePayload, canonicalModelExecutionPayload, intelligenceV31AttemptId, verifyIntelligenceV31InfrastructureAttestation, verifyIntelligenceV31ModelExecutionAttestation, type IntelligenceV31InfrastructureAttestation, type IntelligenceV31ModelExecutionAttestation, type IntelligenceV31TrustedKeyRegistry } from "../src/intelligenceV31Infrastructure";

describe("Intelligence v3.1 infrastructure attestations", () => {
  test("accepts a current Ed25519 attestation from a pinned official provider and rejects tampering", () => {
    const { publicKey, privateKey } = generateKeyPairSync("ed25519");
    const payload = {
      schema: "treatcode.intelligence.infrastructure-attestation-payload.v3.1" as const,
      version: "3.1" as const,
      provider_id: "test-provider",
      key_id: "test-key",
      verified_at: "2026-09-05T00:00:00Z",
      expires_at: "2026-09-06T00:00:00Z",
      network_isolation: { enforcement: "verified_os_or_container" as const, provider: "test-network", evidence_sha256: "a".repeat(64) },
      resource_isolation: { enforcement: "verified_os_or_container" as const, provider: "test-job", evidence_sha256: "b".repeat(64) },
      evidence_store: { enforcement: "verified_append_only" as const, provider: "test-worm", evidence_sha256: "c".repeat(64) },
    };
    const registry: IntelligenceV31TrustedKeyRegistry = { schema: "treatcode.intelligence.trusted-infrastructure-keys.v3.1", version: "3.1", status: "configured", official: true, keys: [{ key_id: "test-key", provider_id: "test-provider", algorithm: "Ed25519", capabilities: ["infrastructure"], public_key_pem: publicKey.export({ type: "spki", format: "pem" }).toString(), not_before: "2026-09-01T00:00:00Z", not_after: "2026-10-01T00:00:00Z" }] };
    const attestation: IntelligenceV31InfrastructureAttestation = { schema: "treatcode.intelligence.infrastructure-attestation.v3.1", version: "3.1", payload, signature_base64: sign(null, Buffer.from(canonicalInfrastructurePayload(payload)), privateKey).toString("base64") };
    assert.equal(verifyIntelligenceV31InfrastructureAttestation(attestation, registry, new Date("2026-09-05T12:00:00Z")).provider_id, "test-provider");
    const tampered = { ...attestation, payload: { ...payload, network_isolation: { ...payload.network_isolation, provider: "fake-network" } } };
    assert.throws(() => verifyIntelligenceV31InfrastructureAttestation(tampered, registry, new Date("2026-09-05T12:00:00Z")), /signature verification failed/);
  });

  test("refuses an unsigned development registry", () => {
    const registry = { schema: "treatcode.intelligence.trusted-infrastructure-keys.v3.1", version: "3.1", status: "unconfigured", official: false, keys: [] } as const;
    assert.throws(() => verifyIntelligenceV31InfrastructureAttestation({ schema: "treatcode.intelligence.infrastructure-attestation.v3.1", version: "3.1", payload: {} } as never, registry as never), /payload is invalid|registry is not configured/);
  });

  test("binds signed model identity and execution controls to one released task", () => {
    const { publicKey, privateKey } = generateKeyPairSync("ed25519");
    const registry: IntelligenceV31TrustedKeyRegistry = { schema: "treatcode.intelligence.trusted-infrastructure-keys.v3.1", version: "3.1", status: "configured", official: true, keys: [{ key_id: "execution-key", provider_id: "model-provider", algorithm: "Ed25519", capabilities: ["model_execution"], public_key_pem: publicKey.export({ type: "spki", format: "pem" }).toString(), not_before: "2026-09-01T00:00:00Z", not_after: "2026-10-01T00:00:00Z" }] };
    const expected = {
      released_at: "2026-09-05T09:00:00Z",
      phase: "subject" as const,
      run_id: "v31-final-a",
      task_id: "TC-V31-FINAL-001",
      model: "gpt-5.6-sol",
      reasoning_effort: "high",
      participant_bundle_sha256: "a".repeat(64),
      suite_sha256: "b".repeat(64),
      cohort: null,
      configuration: null,
      run_number: null,
      fresh_context: true as const,
      cross_task_memory_disabled: true as const,
      attempt_count: 1 as const,
      tool_trace_sha256: "c".repeat(64),
    };
    const payload = { schema: "treatcode.intelligence.model-execution-attestation-payload.v3.1" as const, version: "3.1" as const, provider_id: "model-provider", key_id: "execution-key", issued_at: "2026-09-05T10:00:00Z", expires_at: "2026-09-06T00:00:00Z", ...expected };
    const attestation: IntelligenceV31ModelExecutionAttestation = { schema: "treatcode.intelligence.model-execution-attestation.v3.1", version: "3.1", payload, signature_base64: sign(null, Buffer.from(canonicalModelExecutionPayload(payload)), privateKey).toString("base64") };
    assert.equal(verifyIntelligenceV31ModelExecutionAttestation(attestation, registry, expected, new Date("2026-09-05T12:00:00Z")).model, "gpt-5.6-sol");
    assert.throws(() => verifyIntelligenceV31ModelExecutionAttestation(attestation, { ...registry, keys: registry.keys.map((key) => ({ ...key, capabilities: ["infrastructure"] })) }, expected, new Date("2026-09-05T12:00:00Z")), /not trusted for model_execution/);
    assert.throws(() => verifyIntelligenceV31ModelExecutionAttestation(attestation, registry, { ...expected, reasoning_effort: "max" }, new Date("2026-09-05T12:00:00Z")), /does not match/);
    const relabeled = { ...attestation, payload: { ...payload, model: "gpt-5.6-luna" } };
    assert.throws(() => verifyIntelligenceV31ModelExecutionAttestation(relabeled, registry, { ...expected, model: "gpt-5.6-luna" }, new Date("2026-09-05T12:00:00Z")), /signature verification failed/);
    const prereleased = { ...payload, released_at: "2026-09-05T11:00:00Z" };
    const prereleasedAttestation: IntelligenceV31ModelExecutionAttestation = { ...attestation, payload: prereleased, signature_base64: sign(null, Buffer.from(canonicalModelExecutionPayload(prereleased)), privateKey).toString("base64") };
    assert.throws(() => verifyIntelligenceV31ModelExecutionAttestation(prereleasedAttestation, registry, { ...expected, released_at: prereleased.released_at }, new Date("2026-09-05T12:00:00Z")), /task release/);
    const calibration = { ...payload, phase: "calibration" as const, suite_sha256: null, cohort: "weak" as const, configuration: "gpt-5.6-sol-high", run_number: 1 as const };
    assert.notEqual(intelligenceV31AttemptId(calibration), intelligenceV31AttemptId({ ...calibration, run_number: 2 }));
    assert.throws(() => intelligenceV31AttemptId({ ...calibration, phase: "subject", cohort: "weak" }), /calibration slot/);
  });

  test("derives a stable id for a disposable development attempt", () => {
    assert.equal(intelligenceV31AttemptId({ phase: "development", run_id: "dev-001", task_id: "TC-V31-FINAL-001", model: "gpt-5.6-luna", reasoning_effort: "max", cohort: null, configuration: null, run_number: null }), "dev-001-TC-V31-FINAL-001-gpt-5.6-luna-max");
  });
});
