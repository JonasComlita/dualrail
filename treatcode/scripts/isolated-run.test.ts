import { afterAll, describe, expect, it } from "bun:test";
import { spawn, type ChildProcess } from "node:child_process";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { SecureExecutionQueue, type TritExecutionRequest } from "../src/runner/secure-runner";

const repoRoot = path.resolve(import.meta.dir, "..", "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P09");
const tempRoots: string[] = [];

function wait(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

const request: TritExecutionRequest = {
  schema: "treatcode.execution-request.v1",
  kind: "trit.compile-and-run",
  code: "import ulib;\nfn main() -> t40 { return 0; }",
  engine: "bootstrap",
  optLevel: "-O2",
  sourceCommit: "p09-isolated-e2e",
};

async function makeQueue(root: string): Promise<SecureExecutionQueue> {
  return new SecureExecutionQueue({
    artifactRoot: root,
    repositoryRoot: repoRoot,
    workerPath: path.join(repoRoot, "treatcode", "src", "runner", "worker.ts"),
    concurrency: 1,
    maxRetries: 0,
    limits: { wallTimeMs: 120_000, cpuMs: 120_000 },
  });
}

afterAll(async () => {
  await mkdir(evidenceRoot, { recursive: true });
  await Promise.all(tempRoots.map((root) => rm(root, { recursive: true, force: true })));
});

describe("P09 isolated execution e2e", () => {
  it("executes through a separate worker and reproduces the correctness fingerprint", async () => {
    const firstRoot = await mkdtemp(path.join(os.tmpdir(), "treatcode-p09-e2e-a-"));
    const secondRoot = await mkdtemp(path.join(os.tmpdir(), "treatcode-p09-e2e-b-"));
    tempRoots.push(firstRoot, secondRoot);
    const first = await (await makeQueue(firstRoot)).submit(request).result;
    const second = await (await makeQueue(secondRoot)).submit(request).result;

    expect(first.success).toBe(true);
    expect(second.success).toBe(true);
    expect(first.record.worker_pid).not.toBe(process.pid);
    expect(first.record.isolation.network).toBe("disabled");
    expect(first.record.isolation.workspace).toBe("ephemeral");
    expect(first.record.correctness.result_fingerprint).toBe(second.record.correctness.result_fingerprint);
    expect(first.record.input_hashes.source).toBe(second.record.input_hashes.source);
    expect(first.evidence.recordHash).toMatch(/^sha256:[0-9a-f]{64}$/);
    expect(JSON.parse(await readFile(first.evidence.recordPath, "utf8")).record_hash).toBe(first.evidence.recordHash);

    await mkdir(evidenceRoot, { recursive: true });
    await writeFile(path.join(evidenceRoot, "deterministic-rerun.json"), `${JSON.stringify({
      schema: "treatcode.p09.deterministic-rerun.v1",
      ok: true,
      source_hash: first.record.input_hashes.source,
      correctness_fingerprint: first.record.correctness.result_fingerprint,
      first_record_hash: first.evidence.recordHash,
      second_record_hash: second.evidence.recordHash,
    }, null, 2)}\n`);
  });

  it("routes the public execution endpoint through a separate worker process", async () => {
    const artifactRoot = await mkdtemp(path.join(os.tmpdir(), "treatcode-p09-api-"));
    tempRoots.push(artifactRoot);
    const port = 41000 + (process.pid % 500);
    const appRoot = path.join(repoRoot, "treatcode");
    const server: ChildProcess = spawn(process.execPath, [path.join(appRoot, "server.ts")], {
      cwd: appRoot,
      env: {
        ...process.env,
        PORT: String(port),
        TREATCODE_NO_LISTEN: "0",
        TREATCODE_RUNNER_ARTIFACT_ROOT: artifactRoot,
        TREATCODE_AUTH_AUDIT_PATH: path.join(artifactRoot, "audit.jsonl"),
      },
      shell: false,
      windowsHide: true,
      stdio: "ignore",
    });
    try {
      let ready = false;
      for (let attempt = 0; attempt < 80; attempt += 1) {
        try {
          const response = await fetch(`http://127.0.0.1:${port}/api/auth/v1`);
          if (response.ok) {
            ready = true;
            break;
          }
        } catch {
          // The server is still starting.
        }
        await wait(25);
      }
      expect(ready).toBe(true);

      const loginResponse = await fetch(`http://127.0.0.1:${port}/api/auth/v1/login`, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ identity_id: "tc:identity:demo-human", access_key: "local-human-key" }),
      });
      expect(loginResponse.ok).toBe(true);
      const login = await loginResponse.json() as { data: { credential: { token: string } } };

      const runResponse = await fetch(`http://127.0.0.1:${port}/api/run`, {
        method: "POST",
        headers: {
          "content-type": "application/json",
          authorization: `Bearer ${login.data.credential.token}`,
          "x-treatcode-project": "tc:project:trit",
          "x-action-nonce": "p09-isolated-run-1",
        },
        body: JSON.stringify({
          problemId: "T001",
          code: "fn sign_test(x: t40) -> t40 { return x; }",
          engine: "bootstrap",
          optLevel: "-O2",
        }),
      });
      if (!runResponse.ok) throw new Error(`public run failed: ${runResponse.status} ${await runResponse.text()}`);
      const result = await runResponse.json() as { record: { worker_pid: number | null; state: string }; runId: string; error?: string; compilerOutput?: string };
      expect(result.runId).toMatch(/^run_/);
      expect(result.record.worker_pid).not.toBe(server.pid);
      expect(["succeeded", "failed", "timed_out", "cancelled"]).toContain(result.record.state);
      await mkdir(evidenceRoot, { recursive: true });
      await writeFile(path.join(evidenceRoot, "public-endpoint-isolation.json"), `${JSON.stringify({
        schema: "treatcode.p09.public-endpoint-isolation.v1",
        ok: true,
        run_id: result.runId,
        public_pid: server.pid,
        worker_pid: result.record.worker_pid,
        state: result.record.state,
      }, null, 2)}\n`);
    } finally {
      if (server.pid && !server.killed) server.kill();
    }
  });
});
