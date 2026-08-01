import { afterAll, describe, expect, it } from "bun:test";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import {
  DEFAULT_RUN_LIMITS,
  createSanitizedEnvironment,
  spawnBounded,
  type SafeCommand,
} from "../src/runner/secure-runner";

const repoRoot = path.resolve(import.meta.dir, "..", "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P09");
const tempRoots: string[] = [];
const checks: string[] = [];

async function tempRoot(): Promise<string> {
  const root = await mkdtemp(path.join(os.tmpdir(), "treatcode-p09-adversarial-"));
  tempRoots.push(root);
  return root;
}

function command(root: string, args: string[]): SafeCommand {
  return { executable: process.execPath, args, cwd: root };
}

afterAll(async () => {
  await mkdir(evidenceRoot, { recursive: true });
  await writeFile(path.join(evidenceRoot, "adversarial-tests.json"), `${JSON.stringify({
    schema: "treatcode.p09.adversarial-tests.v1",
    ok: true,
    checks,
  }, null, 2)}\n`);
  await Promise.all(tempRoots.map((root) => rm(root, { recursive: true, force: true })));
});

describe("P09 worker attack surface", () => {
  it("passes arguments as argv with shell execution disabled", async () => {
    const root = await tempRoot();
    const env = createSanitizedEnvironment(root);
    const injection = "literal & whoami; $(touch SHOULD_NOT_EXIST)";
    const result = await spawnBounded(command(root, ["-e", "process.stdout.write(process.argv[1])", injection]), {
      env: env.values,
      limits: { ...DEFAULT_RUN_LIMITS, outputBytes: 1024, wallTimeMs: 2_000 },
    });
    expect(result.stdout).toBe(injection);
    checks.push("shell metacharacters remain literal argv data");
  });

  it("terminates a wall-time abuse fixture and does not leave the child running", async () => {
    const root = await tempRoot();
    const result = await spawnBounded(command(root, ["-e", "setTimeout(() => {}, 10000)" ]), {
      env: createSanitizedEnvironment(root).values,
      limits: { ...DEFAULT_RUN_LIMITS, wallTimeMs: 80, cpuMs: 80 },
    });
    expect(result.terminationReason).toBe("timeout");
    expect(result.timedOut).toBe(true);
    checks.push("wall-time abuse is killed with a terminal timeout reason");
  });

  it("terminates an output-flood fixture at the bounded output size", async () => {
    const root = await tempRoot();
    const result = await spawnBounded(command(root, ["-e", "process.stdout.write('x'.repeat(200000))" ]), {
      env: createSanitizedEnvironment(root).values,
      limits: { ...DEFAULT_RUN_LIMITS, outputBytes: 1024, wallTimeMs: 2_000 },
    });
    expect(result.outputLimitExceeded).toBe(true);
    expect(Buffer.byteLength(result.stdout)).toBeLessThanOrEqual(1024);
    checks.push("output flooding is truncated and terminated at the configured cap");
  });

  it("does not forward host secret variables to workers", async () => {
    const key = "TREATCODE_P09_TEST_SECRET";
    process.env[key] = "must-not-cross";
    try {
      const env = createSanitizedEnvironment(await tempRoot());
      expect(env.values[key]).toBeUndefined();
      expect(env.metadata.values_exposed).toBe(false);
      expect(env.metadata.allowed_keys).not.toContain(key);
      checks.push("worker environment excludes host secret variables");
    } finally {
      delete process.env[key];
    }
  });

  it("keeps compiler execution out of the public server module", async () => {
    const server = await readFile(path.join(repoRoot, "treatcode", "server.ts"), "utf8");
    expect(server).not.toMatch(/from\s+["'](?:node:)?child_process["']/);
    expect(server).not.toMatch(/\bexec\s*\(/);
    expect(server).toContain("executionQueue.submit");
    checks.push("public server has no shell execution path and only submits queue jobs");
  });
});
