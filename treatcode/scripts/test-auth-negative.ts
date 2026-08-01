import { strict as assert } from "node:assert";
import { mkdtempSync, readFileSync, rmSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createDefaultAuthStore, DEFAULT_PROJECT_ID } from "../src/auth";

const repoRoot = resolve(import.meta.dir, "..", "..");
const evidenceRoot = join(repoRoot, "build", "treatcode-plan-evidence", "P07");
const tempRoot = mkdtempSync(join(tmpdir(), "treatcode-p07-negative-"));
const auditPath = join(tempRoot, "audit.jsonl");
let now = Date.parse("2026-01-01T00:00:00.000Z");
const checks: string[] = [];
const report = {
  schema: "treatcode.auth.negative-report.v1",
  ok: false,
  checks,
  errors: [] as string[],
};

function check(condition: unknown, message: string): asserts condition {
  assert(condition, message);
}

try {
  const store = createDefaultAuthStore({ audit_path: auditPath, now: () => now });
  const invalidLogin = store.login("tc:identity:not-real", "wrong-key");
  check(!invalidLogin.ok && invalidLogin.denial.code === "invalid_credentials", "unknown identities must not be enumerable");
  const agentLogin = store.login("tc:identity:demo-agent", "local-agent-key");
  check(!agentLogin.ok && agentLogin.denial.code === "invalid_credentials", "agents must not receive standing interactive sessions");

  const missing = store.authorize({ action: "read", project_id: DEFAULT_PROJECT_ID, require_nonce: false });
  check(!missing.allowed && missing.denial.code === "auth_required" && missing.denial.audit_event_id, "missing auth must return a structured denial with audit evidence");
  const invalid = store.authorize({ token: "tc1.invalid", action: "read", project_id: DEFAULT_PROJECT_ID, require_nonce: false });
  check(!invalid.allowed && invalid.denial.code === "invalid_token", "unknown tokens must be rejected without secret details");
  checks.push("missing, invalid, and standing-agent credentials fail safely");

  const human = store.login("tc:identity:demo-human", "local-human-key");
  check(human.ok, "human setup login must succeed");
  const escalation = store.issueTaskCredential({
    requester_token: human.credential.token,
    target_identity_id: "tc:identity:demo-agent",
    project_id: DEFAULT_PROJECT_ID,
    task_id: "tc:task:p07-negative",
    actions: ["merge"],
    ttl_seconds: 30,
    request_nonce: "negative-merge-1",
  });
  check(!escalation.ok && escalation.denial.code === "target_scope_not_granted", "agent merge authority must not be escalated through issuance");

  const bounded = store.issueTaskCredential({
    requester_token: human.credential.token,
    target_identity_id: "tc:identity:demo-agent",
    project_id: DEFAULT_PROJECT_ID,
    task_id: "tc:task:p07-negative",
    actions: ["test"],
    ttl_seconds: 30,
    request_nonce: "negative-agent-1",
  });
  check(bounded.ok, "negative test setup must create a bounded token");
  const crossProject = store.authorize({ token: bounded.credential.token, action: "test", project_id: "tc:project:other", task_id: "tc:task:p07-negative", request_nonce: "negative-cross-1" });
  check(!crossProject.allowed && crossProject.denial.code === "project_scope_denied", "cross-project execution must be denied");
  const crossTask = store.authorize({ token: bounded.credential.token, action: "test", project_id: DEFAULT_PROJECT_ID, task_id: "tc:task:other", request_nonce: "negative-cross-2" });
  check(!crossTask.allowed && crossTask.denial.code === "task_scope_denied", "cross-task execution must be denied");
  checks.push("privilege escalation and cross-scope access fail closed");

  now += 901_000;
  const expired = store.authorize({ token: human.credential.token, action: "read", project_id: DEFAULT_PROJECT_ID, require_nonce: false });
  check(!expired.allowed && expired.denial.code === "token_expired", "expired session credentials must be rejected");
  const auditText = readFileSync(auditPath, "utf8");
  check(!auditText.includes("wrong-key") && !auditText.includes(human.credential.token), "negative audit evidence must not leak secrets");
  check(store.verifyAudit().ok, "negative decisions must preserve an intact audit chain");
  checks.push("expiry, denial evidence, and secret non-disclosure");
  report.ok = true;
} catch (error) {
  report.errors.push(String(error instanceof Error ? error.message : error));
} finally {
  mkdirSync(evidenceRoot, { recursive: true });
  writeFileSync(join(evidenceRoot, "auth-negative.json"), `${JSON.stringify(report, null, 2)}\n`);
  rmSync(tempRoot, { recursive: true, force: true });
}

console.log(`P07 negative auth: ${report.ok ? "passed" : "failed"}`);
for (const item of report.checks) console.log(`  [ok] ${item}`);
for (const error of report.errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
