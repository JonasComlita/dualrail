import assert from "node:assert/strict";
import { spawn, execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const scriptDirectory = path.dirname(new URL(import.meta.url).pathname.replace(/^\/[A-Za-z]:/, (value) => value.slice(1)).replaceAll("/", path.sep));
export const treatcodeRoot = path.resolve(scriptDirectory, "..");
export const repoRoot = path.resolve(treatcodeRoot, "..");
export const evidenceRoot = path.resolve(process.env.TREATCODE_P08_EVIDENCE_DIR || path.join(repoRoot, "build", "treatcode-plan-evidence", "P08"));

function wait(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function waitForServer(child, port, output) {
  const deadline = Date.now() + 15000;
  let lastError = null;
  while (Date.now() < deadline) {
    if (child.exitCode !== null) throw new Error(`workspace server exited with ${child.exitCode}: ${output.join("\n")}`);
    try {
      const response = await fetch(`http://127.0.0.1:${port}/api/workspaces/v1/capabilities`);
      if (response.ok) return;
    } catch (error) {
      lastError = error;
    }
    await wait(100);
  }
  throw new Error(`workspace server did not become ready: ${lastError || output.join("\n")}`);
}

export async function startWorkspaceServer() {
  const temporaryRoot = fs.mkdtempSync(path.join(os.tmpdir(), "treatcode-p08-"));
  const storageRoot = path.join(temporaryRoot, "storage");
  const port = 35000 + Math.floor(Math.random() * 1000);
  const output = [];
  const child = spawn("bun", ["run", "server.ts"], {
    cwd: treatcodeRoot,
    env: {
      ...process.env,
      PORT: String(port),
      TREATCODE_REPOSITORY_ROOT: repoRoot,
      TREATCODE_WORKSPACE_ROOT: storageRoot,
      TREATCODE_AUTH_AUDIT_PATH: path.join(temporaryRoot, "auth-audit.jsonl"),
    },
    stdio: ["ignore", "pipe", "pipe"],
  });
  child.stdout.on("data", (chunk) => output.push(String(chunk)));
  child.stderr.on("data", (chunk) => output.push(String(chunk)));
  await waitForServer(child, port, output);

  const baseUrl = `http://127.0.0.1:${port}`;
  return {
    baseUrl,
    storageRoot,
    temporaryRoot,
    child,
    async close() {
      if (child.exitCode === null) child.kill();
      await wait(150);
      try { fs.rmSync(temporaryRoot, { recursive: true, force: true }); } catch { /* Windows may release handles shortly after kill. */ }
    },
  };
}

export async function api(server, route, options = {}) {
  const headers = new Headers(options.headers || {});
  headers.set("Accept", "application/json");
  if (options.body !== undefined) headers.set("Content-Type", "application/json");
  if (options.token) headers.set("Authorization", `Bearer ${options.token}`);
  const response = await fetch(`${server.baseUrl}${route}`, { ...options, headers, body: options.body === undefined ? undefined : JSON.stringify(options.body) });
  const payload = await response.json().catch(() => ({}));
  return { response, payload };
}

export async function login(server, identityId = "tc:identity:demo-human", accessKey = "local-human-key") {
  const { response, payload } = await api(server, "/api/auth/v1/login", { method: "POST", body: { identity_id: identityId, access_key: accessKey } });
  assert.equal(response.status, 200, `login failed: ${JSON.stringify(payload)}`);
  assert.ok(payload.data?.credential?.token, "login did not return a credential token");
  return payload.data.credential.token;
}

export function currentCommit() {
  return execFileSync("git", ["rev-parse", "HEAD"], { cwd: repoRoot, encoding: "utf8" }).trim();
}

export function writeEvidence(name, value) {
  fs.mkdirSync(evidenceRoot, { recursive: true });
  const output = path.join(evidenceRoot, name);
  fs.writeFileSync(output, `${JSON.stringify(value, null, 2)}\n`, "utf8");
  return output;
}

export function assertApiOk(result, message = "workspace API request failed") {
  assert.ok(result.response.ok, `${message}: ${result.response.status} ${JSON.stringify(result.payload)}`);
  assert.ok(result.payload?.schema_version === "treatcode.workspace.api.v1", `${message}: missing workspace API schema version`);
  return result.payload.data;
}

