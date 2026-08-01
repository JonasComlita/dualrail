import { strict as assert } from "node:assert";
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

process.env.TREATCODE_NO_LISTEN = "1";
const tempRoot = mkdtempSync(join(tmpdir(), "treatcode-p07-api-"));
process.env.TREATCODE_AUTH_AUDIT_PATH = join(tempRoot, "audit.jsonl");
const repoRoot = resolve(import.meta.dir, "..", "..");
const evidenceRoot = join(repoRoot, "build", "treatcode-plan-evidence", "P07");
const checks: string[] = [];
const report = {
  schema: "treatcode.auth.api-conformance.v1",
  ok: false,
  checks,
  errors: [] as string[],
};

function check(condition: unknown, message: string): asserts condition {
  assert(condition, message);
}

async function jsonRequest(base: string, route: string, init: RequestInit = {}): Promise<{ response: Response; body: any }> {
  const response = await fetch(`${base}${route}`, {
    ...init,
    headers: { "content-type": "application/json", ...(init.headers || {}) },
  });
  return { response, body: await response.json() };
}

try {
  const serverModule = await import("../server.ts");
  const listener = serverModule.app.listen(0);
  await new Promise<void>((resolveReady) => listener.once("listening", resolveReady));
  const address = listener.address();
  const port = typeof address === "object" && address ? address.port : 0;
  const base = `http://127.0.0.1:${port}`;
  try {
    const contract = await jsonRequest(base, "/api/auth/v1/openapi.json");
    check(contract.response.status === 200 && contract.body.openapi === "3.1.0", "auth OpenAPI contract must be served by the auth API");
    for (const route of ["/api/auth/v1/login", "/api/auth/v1/capabilities", "/api/auth/v1/check", "/api/auth/v1/tasks", "/api/auth/v1/audit", "/api/run", "/api/submit"]) {
      check(route in contract.body.paths, `auth OpenAPI contract is missing ${route}`);
    }
    checks.push("versioned auth OpenAPI contract and protected action paths");

    const anonymous = await jsonRequest(base, "/api/auth/v1/capabilities");
    check(anonymous.response.status === 200 && anonymous.body.data.actions.length === 1 && anonymous.body.data.actions[0] === "read", "anonymous capability discovery must expose read only");

    const badLogin = await jsonRequest(base, "/api/auth/v1/login", { method: "POST", body: JSON.stringify({ identity_id: "tc:identity:demo-human", access_key: "wrong-key" }) });
    check(badLogin.response.status === 401 && badLogin.body.error.code === "invalid_credentials" && badLogin.body.error.audit_event_id, "login denial must be structured and audited");
    const login = await jsonRequest(base, "/api/auth/v1/login", { method: "POST", body: JSON.stringify({ identity_id: "tc:identity:demo-human", access_key: "local-human-key" }) });
    check(login.response.status === 200 && login.body.data.credential.token, "human login must issue an opaque credential");
    const humanToken = login.body.data.credential.token;
    const capabilities = await jsonRequest(base, "/api/auth/v1/capabilities", { headers: { authorization: `Bearer ${humanToken}` } });
    check(capabilities.response.status === 200 && capabilities.body.data.actions.includes("test") && !capabilities.body.data.actions.includes("merge"), "UI capability discovery must reflect the server policy");
    checks.push("anonymous, invalid-login, authenticated, and capability responses");

    const task = await jsonRequest(base, "/api/auth/v1/tasks", {
      method: "POST",
      headers: { authorization: `Bearer ${humanToken}`, "x-action-nonce": "api-issue-task-1" },
      body: JSON.stringify({ target_identity_id: "tc:identity:demo-agent", project_id: "tc:project:trit", task_id: "tc:task:p07-api", actions: ["test"], ttl_seconds: 30 }),
    });
    check(task.response.status === 201 && task.body.data?.credential?.credential?.task_id === "tc:task:p07-api", `task issuance must return exact project/task binding: ${JSON.stringify(task.body)}`);
    const agentToken = task.body.data.credential.token;
    const allowed = await jsonRequest(base, "/api/auth/v1/check", {
      method: "POST",
      headers: { authorization: `Bearer ${agentToken}`, "x-treatcode-project": "tc:project:trit", "x-treatcode-task": "tc:task:p07-api" },
      body: JSON.stringify({ action: "test" }),
    });
    check(allowed.response.status === 200 && allowed.body.data.allowed === true, "agent task token must authorize its delegated action");
    const denied = await jsonRequest(base, "/api/auth/v1/check", {
      method: "POST",
      headers: { authorization: `Bearer ${agentToken}`, "x-treatcode-project": "tc:project:trit", "x-treatcode-task": "tc:task:p07-api" },
      body: JSON.stringify({ action: "push" }),
    });
    check(denied.response.status === 403 && denied.body.error.code === "action_not_granted", "agent privilege escalation must return a structured denial");
    const unauthedRun = await jsonRequest(base, "/api/run", { method: "POST", body: JSON.stringify({ problemId: "T001", code: "", engine: "native" }) });
    check(unauthedRun.response.status === 401 && unauthedRun.body.error.code === "auth_required", "run API must use the same auth decision boundary");
    checks.push("task issuance, delegated action, privilege denial, and protected run route");

    const audit = await jsonRequest(base, "/api/auth/v1/audit", { headers: { authorization: `Bearer ${humanToken}` } });
    check(audit.response.status === 200 && Array.isArray(audit.body.data.events) && audit.body.data.events.length > 0, "authorized audit read must return immutable evidence records");
    check(JSON.stringify(audit.body).indexOf(humanToken) === -1 && JSON.stringify(audit.body).indexOf(agentToken) === -1, "auth API must not expose raw tokens in audit data");
    checks.push("audit API access and secret non-disclosure");
  } finally {
    await new Promise<void>((resolveClosed) => listener.close(() => resolveClosed()));
  }
  report.ok = true;
} catch (error) {
  report.errors.push(String(error instanceof Error ? error.stack || error.message : error));
} finally {
  mkdirSync(evidenceRoot, { recursive: true });
  writeFileSync(join(evidenceRoot, "api-conformance.json"), `${JSON.stringify(report, null, 2)}\n`);
  rmSync(tempRoot, { recursive: true, force: true });
}

console.log(`P07 API conformance: ${report.ok ? "passed" : "failed"}`);
for (const item of report.checks) console.log(`  [ok] ${item}`);
for (const error of report.errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
