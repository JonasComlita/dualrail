import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

export const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
export const repoRoot = path.resolve(appRoot, "..");
export const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P12");

export function assert(condition, message) {
  if (!condition) throw new Error(message);
}

export function sleep(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

export function writeEvidence(name, report) {
  fs.mkdirSync(evidenceRoot, { recursive: true });
  const output = path.join(evidenceRoot, name);
  fs.writeFileSync(output, `${JSON.stringify(report, null, 2)}\n`, "utf8");
  return output;
}

export function buildIfNeeded() {
  const operationsHtml = path.join(appRoot, "dist", "operations", "index.html");
  if (fs.existsSync(operationsHtml)) return;
  const command = process.platform === "win32" ? "npm.cmd" : "npm";
  const result = spawnSync(command, ["run", "build", "--silent"], { cwd: appRoot, encoding: "utf8", stdio: "pipe" });
  assert(result.status === 0, `TreatCode build failed:\n${result.stdout}\n${result.stderr}`);
}

export async function startServer({ build = false } = {}) {
  if (build) buildIfNeeded();
  const port = 43000 + Math.floor(Math.random() * 1000);
  const statePath = path.join(os.tmpdir(), `treatcode-operations-${process.pid}-${port}.json`);
  const bunCommand = process.platform === "win32" ? "bun.exe" : "bun";
  const child = spawn(bunCommand, ["run", "server.ts"], {
    cwd: appRoot,
    env: { ...process.env, PORT: String(port), TREATCODE_OPERATIONS_STATE: statePath },
    stdio: ["ignore", "pipe", "pipe"],
    windowsHide: true,
  });
  let output = "";
  child.stdout.on("data", (chunk) => { output += chunk.toString(); });
  child.stderr.on("data", (chunk) => { output += chunk.toString(); });
  const baseUrl = `http://127.0.0.1:${port}`;
  let ready = false;
  for (let attempt = 0; attempt < 50 && !ready; attempt += 1) {
    await sleep(100);
    try {
      const response = await fetch(`${baseUrl}/api/operations/health`);
      ready = response.status === 200 || response.status === 503;
    } catch {
      // The server is still starting.
    }
  }
  assert(ready, `operations server did not start on ${baseUrl}. Output:\n${output}`);
  return {
    baseUrl,
    statePath,
    child,
    output: () => output,
    async stop() {
      if (!child.killed) child.kill();
      await Promise.race([new Promise((resolve) => child.once("exit", resolve)), sleep(1000)]);
      try { fs.rmSync(statePath, { force: true }); } catch { /* best-effort cleanup of one named temp file */ }
    },
  };
}

export async function api(server, pathname, init = {}) {
  const response = await fetch(`${server.baseUrl}${pathname}`, {
    ...init,
    headers: { Accept: "application/json", "Content-Type": "application/json", ...(init.headers || {}) },
  });
  const body = await response.json().catch(() => ({}));
  return { response, body };
}

export function jsonBody(value) {
  return { body: JSON.stringify(value) };
}
