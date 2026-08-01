import { strict as assert } from "node:assert";
import { mkdtempSync, readFileSync, rmSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createDefaultAuthStore, DEFAULT_PROJECT_ID, type PermissionAction } from "../src/auth";

const repoRoot = resolve(import.meta.dir, "..", "..");
const evidenceRoot = join(repoRoot, "build", "treatcode-plan-evidence", "P07");
const tempRoot = mkdtempSync(join(tmpdir(), "treatcode-p07-positive-"));
const auditPath = join(tempRoot, "audit.jsonl");
let now = Date.parse("2026-01-01T00:00:00.000Z");

const checks: string[] = [];
const report = {
  schema: "treatcode.auth.positive-report.v1",
  ok: false,
  policy_version: "treatcode.authz.policy.v1",
  checks,
  errors: [] as string[],
};

function check(condition: unknown, message: string): asserts condition {
  assert(condition, message);
}

try {
  const store = createDefaultAuthStore({ audit_path: auditPath, now: () => now, task_ttl_seconds: 30 });
  const login = store.login("tc:identity:demo-human", "local-human-key");
  check(login.ok, "demo human login must succeed");
  const humanToken = login.credential.token;

  const read = store.authorize({ token: humanToken, action: "read", project_id: DEFAULT_PROJECT_ID, require_nonce: false });
  check(read.allowed, "human read must be allowed");
  check(read.credential.project_id === "*", "human session must retain explicit wildcard project policy");

  const missingNonce = store.authorize({ token: humanToken, action: "test", project_id: DEFAULT_PROJECT_ID });
  check(!missingNonce.allowed && missingNonce.denial.code === "nonce_required", "mutating action without nonce must be denied");

  const firstTest = store.authorize({ token: humanToken, action: "test", project_id: DEFAULT_PROJECT_ID, request_nonce: "positive-test-1" });
  check(firstTest.allowed, "human test with a fresh nonce must be allowed");
  const replay = store.authorize({ token: humanToken, action: "test", project_id: DEFAULT_PROJECT_ID, request_nonce: "positive-test-1" });
  check(!replay.allowed && replay.denial.code === "replay_detected", "replayed mutation nonce must be denied");
  checks.push("independent human authorization and one-time mutation nonces");

  const issued = store.issueTaskCredential({
    requester_token: humanToken,
    target_identity_id: "tc:identity:demo-agent",
    project_id: DEFAULT_PROJECT_ID,
    task_id: "tc:task:p07-positive",
    actions: ["read", "test"],
    ttl_seconds: 30,
    request_nonce: "issue-positive-1",
  });
  check(issued.ok, "human must be able to issue a bounded agent credential");
  check(issued.credential.credential.task_id === "tc:task:p07-positive", "agent credential must bind the exact task");
  check(issued.credential.credential.actions.length === 2, "agent credential must carry only delegated actions");
  const agentToken = issued.credential.token;

  const agentTest = store.authorize({
    token: agentToken,
    action: "test",
    project_id: DEFAULT_PROJECT_ID,
    task_id: "tc:task:p07-positive",
    request_nonce: "agent-test-1",
  });
  check(agentTest.allowed, "delegated agent test must be allowed");
  const agentEdit = store.authorize({
    token: agentToken,
    action: "edit",
    project_id: DEFAULT_PROJECT_ID,
    task_id: "tc:task:p07-positive",
    request_nonce: "agent-edit-1",
  });
  check(!agentEdit.allowed && agentEdit.denial.code === "action_not_granted", "edit must remain independent from test");
  checks.push("task-scoped agent credential and least-privilege action intersection");

  const capabilities = store.capabilities({ token: agentToken, project_id: DEFAULT_PROJECT_ID, task_id: "tc:task:p07-positive" });
  check(capabilities.ok, "capability discovery must use the same authorization policy");
  check(capabilities.actions.includes("test") && !capabilities.actions.includes("edit"), "capability discovery must expose exact grants");

  const crossProject = store.authorize({ token: agentToken, action: "read", project_id: "tc:project:other", task_id: "tc:task:p07-positive", require_nonce: false });
  check(!crossProject.allowed && crossProject.denial.code === "project_scope_denied", "cross-project access must be denied");
  const crossTask = store.authorize({ token: agentToken, action: "read", project_id: DEFAULT_PROJECT_ID, task_id: "tc:task:other", require_nonce: false });
  check(!crossTask.allowed && crossTask.denial.code === "task_scope_denied", "cross-task access must be denied");
  checks.push("cross-project and cross-task isolation");

  now += 31_000;
  const expired = store.authorize({ token: agentToken, action: "read", project_id: DEFAULT_PROJECT_ID, task_id: "tc:task:p07-positive", require_nonce: false });
  check(!expired.allowed && expired.denial.code === "token_expired", "expired task credential must be denied");

  now = Date.parse("2026-01-01T00:00:00.000Z");
  const second = store.issueTaskCredential({
    requester_token: humanToken,
    target_identity_id: "tc:identity:demo-agent",
    project_id: DEFAULT_PROJECT_ID,
    task_id: "tc:task:p07-revoke",
    actions: ["read"],
    ttl_seconds: 30,
    request_nonce: "issue-revoke-1",
  });
  check(second.ok, "second task credential must be issuable for revocation coverage");
  const revoked = store.revokeCredential({
    requester_token: humanToken,
    credential_id: second.credential.credential.credential_id,
    project_id: DEFAULT_PROJECT_ID,
    task_id: "tc:task:p07-revoke",
    request_nonce: "revoke-1",
  });
  check(revoked.ok, "credential revocation must be authorized and recorded");
  const afterRevoke = store.authorize({ token: second.credential.token, action: "read", project_id: DEFAULT_PROJECT_ID, task_id: "tc:task:p07-revoke", require_nonce: false });
  check(!afterRevoke.allowed && afterRevoke.denial.code === "token_revoked", "revoked task credential must be denied");
  checks.push("expiry and revocation fail closed");

  const events = readFileSync(auditPath, "utf8");
  check(!events.includes(humanToken) && !events.includes(agentToken), "audit records must not contain raw credentials");
  const audit = store.verifyAudit();
  check(audit.ok && audit.count >= 12, "audit records must form a verified hash chain with denied actions included");
  checks.push("hash-chained audit evidence excludes raw credentials");

  report.ok = true;
} catch (error) {
  report.errors.push(String(error instanceof Error ? error.message : error));
} finally {
  mkdirSync(evidenceRoot, { recursive: true });
  writeFileSync(join(evidenceRoot, "auth-positive.json"), `${JSON.stringify(report, null, 2)}\n`);
  rmSync(tempRoot, { recursive: true, force: true });
}

console.log(`P07 positive auth: ${report.ok ? "passed" : "failed"}`);
for (const item of report.checks) console.log(`  [ok] ${item}`);
for (const error of report.errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
