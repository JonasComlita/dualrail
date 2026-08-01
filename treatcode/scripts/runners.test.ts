import { afterAll, describe, expect, it } from "bun:test";
import { mkdtemp, readFile, readdir, rm, writeFile, mkdir } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import {
  ArtifactStore,
  RunnerWorkerError,
  SecureExecutionQueue,
  type TritExecutionRequest,
  type WorkerOutput,
} from "../src/runner/secure-runner";

const repoRoot = path.resolve(import.meta.dir, "..", "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P09");
const tempRoots: string[] = [];
const checks: string[] = [];

function request(code = "fn main() -> t40 { return 0; }"): TritExecutionRequest {
  return {
    schema: "treatcode.execution-request.v1",
    kind: "trit.compile-and-run",
    code,
    engine: "bootstrap",
    optLevel: "-O2",
    sourceCommit: "p09-test-commit",
  };
}

function fakeOutput(context: { workspaceRoot: string }): WorkerOutput {
  return {
    success: true,
    compilerOutput: "compiler-ok",
    stdout: "worker-stdout",
    stderr: "",
    cycles: 3,
    r13: 0,
    consoleOutput: "",
    registers: { r13: 0 },
    commands: [{ executable: "fixed-worker", args: ["--safe"], cwd: context.workspaceRoot }],
    commandResults: [],
    workerPid: process.pid,
  };
}

async function tempRoot(): Promise<string> {
  const root = await mkdtemp(path.join(os.tmpdir(), "treatcode-p09-runners-"));
  tempRoots.push(root);
  return root;
}

afterAll(async () => {
  await mkdir(evidenceRoot, { recursive: true });
  await writeFile(path.join(evidenceRoot, "runner-tests.json"), `${JSON.stringify({
    schema: "treatcode.p09.runner-tests.v1",
    ok: true,
    checks,
  }, null, 2)}\n`);
  await Promise.all(tempRoots.map((root) => rm(root, { recursive: true, force: true })));
});

describe("P09 runner queue", () => {
  it("commits immutable content-addressed evidence without storing source text", async () => {
    const root = await tempRoot();
    const queue = new SecureExecutionQueue({
      artifactRoot: root,
      repositoryRoot: repoRoot,
      workerPath: path.join(repoRoot, "treatcode", "src", "runner", "worker.ts"),
      workerExecutor: async (_request, context) => fakeOutput(context),
    });
    const source = "fn main() -> t40 { return 41 + 1; }";
    const result = await queue.submit(request(source)).result;

    expect(result.state).toBe("succeeded");
    expect(result.success).toBe(true);
    expect(result.record.source_commit).toBe("p09-test-commit");
    expect(result.record.input_hashes.source).toMatch(/^sha256:[0-9a-f]{64}$/);
    expect(result.record.artifact_hashes.stdout).toMatch(/^sha256:[0-9a-f]{64}$/);
    expect(JSON.stringify(result.record)).not.toContain(source);
    const persisted = JSON.parse(await readFile(result.evidence.recordPath, "utf8"));
    expect(persisted.record_hash).toBe(result.record.record_hash);
    const artifactNames = await new ArtifactStore(root).listObjectNames();
    expect(artifactNames.length).toBeGreaterThanOrEqual(4);
    expect(await readdir(path.join(root, "workspaces"))).toHaveLength(0);
    checks.push("successful runs create immutable records and content-addressed artifacts");
  });

  it("retries a failed worker once and records both attempts", async () => {
    const root = await tempRoot();
    let calls = 0;
    const queue = new SecureExecutionQueue({
      artifactRoot: root,
      repositoryRoot: repoRoot,
      workerPath: path.join(repoRoot, "treatcode", "src", "runner", "worker.ts"),
      maxRetries: 1,
      workerExecutor: async (_request, context) => {
        calls += 1;
        if (calls === 1) throw new RunnerWorkerError("simulated worker crash");
        return fakeOutput(context);
      },
    });
    const result = await queue.submit(request()).result;

    expect(result.success).toBe(true);
    expect(result.record.attempts).toBe(2);
    checks.push("worker failure retries are bounded and recorded");
  });

  it("cancels a queued run and persists a cancelled terminal record", async () => {
    const root = await tempRoot();
    let release!: () => void;
    const blocker = new Promise<void>((resolve) => { release = resolve; });
    let calls = 0;
    const queue = new SecureExecutionQueue({
      artifactRoot: root,
      repositoryRoot: repoRoot,
      workerPath: path.join(repoRoot, "treatcode", "src", "runner", "worker.ts"),
      concurrency: 1,
      workerExecutor: async (_request, context) => {
        calls += 1;
        if (calls === 1) await blocker;
        return fakeOutput(context);
      },
    });
    const first = queue.submit(request());
    await new Promise((resolve) => setTimeout(resolve, 10));
    const second = queue.submit(request("fn main() -> t40 { return 2; }"));
    expect(second.cancel()).toBe(true);
    const cancelled = await second.result;
    release();
    await first.result;

    expect(cancelled.state).toBe("cancelled");
    expect(cancelled.record.termination_reason).toBe("cancelled_by_request");
    expect(queue.status(second.runId)?.state).toBe("cancelled");
    checks.push("queued cancellation leaves an immutable terminal record");
  });
});
