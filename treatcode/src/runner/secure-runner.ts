import { createHash, randomUUID } from "node:crypto";
import { spawn, type ChildProcess } from "node:child_process";
import { chmod, mkdir, readFile, readdir, rm, writeFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import path from "node:path";

export const RUNNER_PROTOCOL_VERSION = "treatcode.runner.v1" as const;
export const EXECUTION_REQUEST_SCHEMA = "treatcode.execution-request.v1" as const;

export type RunnerEngine = "native" | "bootstrap";
export type RunState = "queued" | "running" | "succeeded" | "failed" | "timed_out" | "cancelled";

export interface RunLimits {
  cpuMs: number;
  memoryMb: number;
  processCount: number;
  outputBytes: number;
  wallTimeMs: number;
}

export const DEFAULT_RUN_LIMITS: RunLimits = {
  cpuMs: 30_000,
  memoryMb: 256,
  processCount: 8,
  outputBytes: 1_048_576,
  wallTimeMs: 45_000,
};

const HARD_LIMITS: RunLimits = {
  cpuMs: 120_000,
  memoryMb: 512,
  processCount: 16,
  outputBytes: 4 * 1_048_576,
  wallTimeMs: 120_000,
};

export interface TritExecutionRequest {
  schema: typeof EXECUTION_REQUEST_SCHEMA;
  kind: "trit.compile-and-run";
  code: string;
  engine: RunnerEngine;
  optLevel: "-O0" | "-O1" | "-O2" | "-O3";
  sourceCommit: string;
}

export interface SafeCommand {
  executable: string;
  args: string[];
  cwd: string;
}

export interface BoundedCommandResult {
  code: number | null;
  signal: string | null;
  stdout: string;
  stderr: string;
  durationMs: number;
  timedOut: boolean;
  outputLimitExceeded: boolean;
  spawnError?: string;
  terminationReason?: "timeout" | "output-limit" | "cancelled";
}

export interface WorkerOutput {
  success: boolean;
  compileTimeCycles?: number;
  assembly?: string;
  cycles?: number | null;
  status?: string | null;
  r13?: number | null;
  consoleOutput?: string;
  registers?: Record<string, number>;
  compilerOutput: string;
  stdout: string;
  stderr: string;
  error?: string;
  commands: SafeCommand[];
  commandResults: BoundedCommandResult[];
  workerPid: number | null;
  terminationReason?: "timeout" | "output-limit" | "cancelled";
}

export interface ArtifactRef {
  hash: string;
  path: string;
  bytes: number;
  mediaType: string;
}

export interface ImmutableRunRecord {
  schema: "treatcode.run-record.v1";
  run_id: string;
  state: Exclude<RunState, "queued" | "running">;
  submitted_at: string;
  started_at: string;
  finished_at: string;
  source_commit: string;
  input_hashes: {
    source: string;
    request: string;
  };
  runner_image: string;
  toolchain: {
    runner_protocol: string;
    runtime: string;
    compiler: string;
  };
  isolation: {
    base_image: "read-only";
    workspace: "ephemeral";
    network: "disabled";
    host_secrets: "not-forwarded";
    shell: "disabled";
  };
  limits: RunLimits;
  commands: SafeCommand[];
  environment: {
    allowed_keys: string[];
    fingerprint: string;
    values_exposed: false;
  };
  attempts: number;
  worker_pid: number | null;
  exit_status: {
    worker_code: number | null;
    worker_signal: string | null;
    command_results: BoundedCommandResult[];
  };
  termination_reason: string | null;
  correctness: {
    success: boolean;
    result_fingerprint: string;
  };
  artifact_hashes: Record<string, string>;
  record_hash: string;
}

export interface ExecutionEvidence {
  recordPath: string;
  recordHash: string;
  artifacts: Record<string, ArtifactRef>;
}

export interface ExecutionResult {
  runId: string;
  state: Exclude<RunState, "queued" | "running">;
  success: boolean;
  compileTimeCycles?: number;
  assembly?: string;
  cycles?: number | null;
  status?: string | null;
  r13?: number | null;
  consoleOutput?: string;
  registers?: Record<string, number>;
  compilerOutput: string;
  error?: string;
  evidence: ExecutionEvidence;
  record: ImmutableRunRecord;
}

export interface RunHandle {
  runId: string;
  result: Promise<ExecutionResult>;
  cancel: () => boolean;
}

export interface RunStatus {
  runId: string;
  state: RunState;
  attempts: number;
}

export interface SanitizedEnvironment {
  values: NodeJS.ProcessEnv;
  metadata: ImmutableRunRecord["environment"];
}

export interface WorkerContext {
  runId: string;
  workspaceRoot: string;
  repositoryRoot: string;
  workerPath: string;
  limits: RunLimits;
  environment: SanitizedEnvironment;
  signal: AbortSignal;
}

export type WorkerExecutor = (request: TritExecutionRequest, context: WorkerContext) => Promise<WorkerOutput>;

interface QueueJob {
  runId: string;
  request: TritExecutionRequest;
  submittedAt: string;
  controller: AbortController;
  attempts: number;
  state: RunState;
  startedAt?: string;
  cancelRequested: boolean;
  resolve: (result: ExecutionResult) => void;
  result: Promise<ExecutionResult>;
}

interface QueueOptions {
  artifactRoot: string;
  repositoryRoot: string;
  workerPath: string;
  concurrency?: number;
  maxRetries?: number;
  limits?: Partial<RunLimits>;
  workerExecutor?: WorkerExecutor;
}

function canonicalize(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(canonicalize);
  if (value && typeof value === "object") {
    return Object.fromEntries(
      Object.entries(value as Record<string, unknown>)
        .sort(([left], [right]) => left.localeCompare(right))
        .map(([key, item]) => [key, canonicalize(item)]),
    );
  }
  return value;
}

export function stableJson(value: unknown): string {
  return JSON.stringify(canonicalize(value));
}

export function sha256(value: string | Uint8Array): string {
  return createHash("sha256").update(value).digest("hex");
}

function assertSafeSegment(value: string, label: string): void {
  if (!/^[A-Za-z0-9._-]+$/.test(value)) throw new Error(`${label} contains an unsafe path segment`);
}

function isWithin(parent: string, candidate: string): boolean {
  const relative = path.relative(path.resolve(parent), path.resolve(candidate));
  return relative === "" || (relative !== ".." && !relative.startsWith(`..${path.sep}`) && !path.isAbsolute(relative));
}

async function writeImmutable(filePath: string, data: string | Uint8Array): Promise<void> {
  await mkdir(path.dirname(filePath), { recursive: true });
  try {
    await writeFile(filePath, data, { flag: "wx", mode: 0o444 });
    try {
      await chmod(filePath, 0o444);
    } catch {
      // Windows has weaker chmod semantics; content-addressing still prevents overwrites.
    }
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code !== "EEXIST") throw error;
    const existing = await readFile(filePath);
    const expected = typeof data === "string" ? Buffer.from(data) : Buffer.from(data);
    if (!existing.equals(expected)) throw new Error(`immutable artifact collision at ${filePath}`);
  }
}

export class ArtifactStore {
  readonly root: string;
  private readonly objectRoot: string;
  private readonly recordRoot: string;
  private readonly workspaceRoot: string;

  constructor(root: string) {
    this.root = path.resolve(root);
    this.objectRoot = path.join(this.root, "objects", "sha256");
    this.recordRoot = path.join(this.root, "records");
    this.workspaceRoot = path.join(this.root, "workspaces");
  }

  async init(): Promise<void> {
    await Promise.all([
      mkdir(this.objectRoot, { recursive: true }),
      mkdir(this.recordRoot, { recursive: true }),
      mkdir(this.workspaceRoot, { recursive: true }),
    ]);
  }

  async createWorkspace(runId: string): Promise<string> {
    assertSafeSegment(runId, "run id");
    await this.init();
    const workspace = path.join(this.workspaceRoot, runId);
    if (!isWithin(this.workspaceRoot, workspace)) throw new Error("workspace escaped its root");
    await mkdir(workspace, { recursive: false });
    return workspace;
  }

  async removeWorkspace(runId: string): Promise<void> {
    assertSafeSegment(runId, "run id");
    const workspace = path.join(this.workspaceRoot, runId);
    if (!isWithin(this.workspaceRoot, workspace)) throw new Error("workspace escaped its root");
    await rm(workspace, { recursive: true, force: true });
  }

  async put(data: string | Uint8Array, mediaType: string): Promise<ArtifactRef> {
    await this.init();
    const bytes = typeof data === "string" ? Buffer.from(data, "utf8") : Buffer.from(data);
    const digest = sha256(bytes);
    const filePath = path.join(this.objectRoot, digest);
    if (!isWithin(this.objectRoot, filePath)) throw new Error("artifact escaped its root");
    await writeImmutable(filePath, bytes);
    return {
      hash: `sha256:${digest}`,
      path: filePath,
      bytes: bytes.byteLength,
      mediaType,
    };
  }

  async putJson(value: unknown, mediaType = "application/json"): Promise<ArtifactRef> {
    return this.put(`${stableJson(value)}\n`, mediaType);
  }

  async commitRecord(runId: string, record: Omit<ImmutableRunRecord, "record_hash">): Promise<{ record: ImmutableRunRecord; path: string }> {
    assertSafeSegment(runId, "run id");
    const payloadWithoutHash = `${stableJson(record)}\n`;
    const recordHash = `sha256:${sha256(payloadWithoutHash)}`;
    const complete: ImmutableRunRecord = { ...record, record_hash: recordHash };
    const filePath = path.join(this.recordRoot, `${runId}.json`);
    if (!isWithin(this.recordRoot, filePath)) throw new Error("record escaped its root");
    await writeImmutable(filePath, `${stableJson(complete)}\n`);
    return { record: complete, path: filePath };
  }

  async readRecord(runId: string): Promise<ImmutableRunRecord | null> {
    assertSafeSegment(runId, "run id");
    const filePath = path.join(this.recordRoot, `${runId}.json`);
    if (!isWithin(this.recordRoot, filePath)) throw new Error("record escaped its root");
    try {
      const parsed = JSON.parse(await readFile(filePath, "utf8")) as ImmutableRunRecord;
      const { record_hash: storedHash, ...withoutHash } = parsed;
      const expectedHash = `sha256:${sha256(`${stableJson(withoutHash)}\n`)}`;
      if (storedHash !== expectedHash) throw new Error(`run record integrity check failed for ${runId}`);
      return parsed;
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === "ENOENT") return null;
      throw error;
    }
  }

  async listObjectNames(): Promise<string[]> {
    await this.init();
    return readdir(this.objectRoot);
  }
}

export function normalizeRunLimits(input: Partial<RunLimits> = {}): RunLimits {
  const positive = (value: unknown, fallback: number, maximum: number): number => {
    const candidate = typeof value === "number" && Number.isFinite(value) ? Math.floor(value) : fallback;
    return Math.max(1, Math.min(maximum, candidate));
  };
  return {
    cpuMs: positive(input.cpuMs, DEFAULT_RUN_LIMITS.cpuMs, HARD_LIMITS.cpuMs),
    memoryMb: positive(input.memoryMb, DEFAULT_RUN_LIMITS.memoryMb, HARD_LIMITS.memoryMb),
    processCount: positive(input.processCount, DEFAULT_RUN_LIMITS.processCount, HARD_LIMITS.processCount),
    outputBytes: positive(input.outputBytes, DEFAULT_RUN_LIMITS.outputBytes, HARD_LIMITS.outputBytes),
    wallTimeMs: positive(input.wallTimeMs, DEFAULT_RUN_LIMITS.wallTimeMs, HARD_LIMITS.wallTimeMs),
  };
}

export function validateExecutionRequest(request: TritExecutionRequest): void {
  if (request.schema !== EXECUTION_REQUEST_SCHEMA || request.kind !== "trit.compile-and-run") {
    throw new Error("unsupported execution request schema");
  }
  if (request.engine !== "native" && request.engine !== "bootstrap") throw new Error("unsupported runner engine");
  if (!/^-[O][0-3]$/.test(request.optLevel)) throw new Error("unsupported optimization level");
  if (typeof request.code !== "string" || request.code.length === 0 || request.code.length > 512 * 1024) {
    throw new Error("source code is empty or exceeds the runner input limit");
  }
  if (typeof request.sourceCommit !== "string" || request.sourceCommit.length === 0 || request.sourceCommit.length > 128) {
    throw new Error("source commit provenance is invalid");
  }
}

export function createSanitizedEnvironment(workspaceRoot: string, additionalPath: string[] = []): SanitizedEnvironment {
  const values: NodeJS.ProcessEnv = {
    LANG: "C",
    LC_ALL: "C",
    TZ: "UTC",
    TREATCODE_NETWORK_POLICY: "disabled",
    TREATCODE_HOST_SECRETS: "unavailable",
    TREATCODE_WORKSPACE: workspaceRoot,
    TEMP: workspaceRoot,
    TMP: workspaceRoot,
    TMPDIR: workspaceRoot,
  };

  const inheritedToolchainPaths = (process.env.PATH || "")
    .split(path.delimiter)
    .filter((entry) => /(?:msys|mingw|ucrt|llvm|gcc)/i.test(entry));

  if (process.platform === "win32") {
    const systemRoot = process.env.SystemRoot || process.env.WINDIR;
    if (systemRoot) {
      values.SystemRoot = systemRoot;
      values.WINDIR = systemRoot;
      values.PATH = [path.join(systemRoot, "System32"), path.dirname(process.execPath), ...additionalPath, ...inheritedToolchainPaths].join(path.delimiter);
    }
  } else {
    values.PATH = ["/usr/bin", "/bin", path.dirname(process.execPath), ...additionalPath, ...inheritedToolchainPaths].join(path.delimiter);
  }

  const allowedKeys = Object.keys(values).sort();
  const fingerprint = `sha256:${sha256(stableJson(Object.fromEntries(allowedKeys.map((key) => [key, values[key] || ""])) ))}`;
  return {
    values,
    metadata: {
      allowed_keys: allowedKeys,
      fingerprint,
      values_exposed: false,
    },
  };
}

async function killProcessTree(child: ChildProcess): Promise<void> {
  if (!child.pid) return;
  if (process.platform === "win32") {
    const systemRoot = process.env.SystemRoot || process.env.WINDIR || "C:\\Windows";
    const taskkill = path.join(systemRoot, "System32", "taskkill.exe");
    await new Promise<void>((resolve) => {
      const killer = spawn(taskkill, ["/PID", String(child.pid), "/T", "/F"], {
        shell: false,
        windowsHide: true,
        stdio: "ignore",
      });
      killer.once("close", () => resolve());
      killer.once("error", () => resolve());
    });
    return;
  }

  try {
    process.kill(-child.pid, "SIGKILL");
  } catch {
    try {
      child.kill("SIGKILL");
    } catch {
      // The process already exited.
    }
  }
}

function appendOutput(chunks: Buffer[], incoming: Buffer, remaining: number): number {
  if (remaining <= 0) return 0;
  const accepted = incoming.subarray(0, remaining);
  if (accepted.length > 0) chunks.push(accepted);
  return accepted.length;
}

export async function spawnBounded(
  command: SafeCommand,
  options: {
    env: NodeJS.ProcessEnv;
    limits: RunLimits;
    signal?: AbortSignal;
    input?: string;
  },
): Promise<BoundedCommandResult> {
  const started = Date.now();
  const stdoutChunks: Buffer[] = [];
  const stderrChunks: Buffer[] = [];
  let outputBytes = 0;
  let timedOut = false;
  let outputLimitExceeded = false;
  let terminationReason: BoundedCommandResult["terminationReason"];
  let settled = false;
  let timer: ReturnType<typeof setTimeout> | undefined;
  let abortHandler: (() => void) | undefined;
  let child: ChildProcess;

  const finish = (result: BoundedCommandResult): BoundedCommandResult => {
    if (timer) clearTimeout(timer);
    if (abortHandler && options.signal) options.signal.removeEventListener("abort", abortHandler);
    return result;
  };

  try {
    child = spawn(command.executable, command.args, {
      cwd: command.cwd,
      env: options.env,
      shell: false,
      windowsHide: true,
      detached: process.platform !== "win32",
      stdio: ["pipe", "pipe", "pipe"],
    });
  } catch (error) {
    return {
      code: null,
      signal: null,
      stdout: "",
      stderr: "",
      durationMs: Date.now() - started,
      timedOut: false,
      outputLimitExceeded: false,
      spawnError: String(error),
    };
  }

  const kill = (reason: BoundedCommandResult["terminationReason"]): void => {
    if (settled || !child.pid) return;
    terminationReason = reason;
    void killProcessTree(child);
  };

  const result = await new Promise<BoundedCommandResult>((resolve) => {
    const maybeFinish = (code: number | null, signal: NodeJS.Signals | null, spawnError?: string) => {
      if (settled) return;
      settled = true;
      resolve(finish({
        code,
        signal,
        stdout: Buffer.concat(stdoutChunks).toString("utf8"),
        stderr: Buffer.concat(stderrChunks).toString("utf8"),
        durationMs: Date.now() - started,
        timedOut,
        outputLimitExceeded,
        spawnError,
        terminationReason,
      }));
    };

    child.stdout?.on("data", (chunk: Buffer) => {
      const buffer = Buffer.from(chunk);
      const accepted = appendOutput(stdoutChunks, buffer, options.limits.outputBytes - outputBytes);
      outputBytes += accepted;
      if (accepted < buffer.length && !outputLimitExceeded) {
        outputLimitExceeded = true;
        kill("output-limit");
      }
    });
    child.stderr?.on("data", (chunk: Buffer) => {
      const buffer = Buffer.from(chunk);
      const accepted = appendOutput(stderrChunks, buffer, options.limits.outputBytes - outputBytes);
      outputBytes += accepted;
      if (accepted < buffer.length && !outputLimitExceeded) {
        outputLimitExceeded = true;
        kill("output-limit");
      }
    });
    child.once("error", (error) => maybeFinish(null, null, String(error)));
    child.once("close", (code, signal) => maybeFinish(code, signal));

    timer = setTimeout(() => {
      if (!settled) {
        timedOut = true;
        kill("timeout");
      }
    }, options.limits.wallTimeMs);

    abortHandler = () => {
      if (!settled) kill("cancelled");
    };
    if (options.signal) {
      if (options.signal.aborted) abortHandler();
      else options.signal.addEventListener("abort", abortHandler, { once: true });
    }

    if (options.input !== undefined) {
      child.stdin?.end(options.input);
    } else {
      child.stdin?.end();
    }
  });

  return result;
}

function displayPath(value: string, workspaceRoot: string, repositoryRoot: string): string {
  const absolute = path.resolve(value);
  if (isWithin(workspaceRoot, absolute)) return `<workspace>${path.sep}${path.relative(workspaceRoot, absolute)}`;
  if (isWithin(repositoryRoot, absolute)) return `<base>${path.sep}${path.relative(repositoryRoot, absolute)}`;
  return path.basename(value);
}

export function sanitizeCommand(command: SafeCommand, workspaceRoot: string, repositoryRoot: string): SafeCommand {
  return {
    executable: path.basename(command.executable),
    args: command.args.map((arg) => (path.isAbsolute(arg) ? displayPath(arg, workspaceRoot, repositoryRoot) : arg)),
    cwd: "<workspace>",
  };
}

export class RunnerWorkerError extends Error {
  readonly retryable = true;
}

async function executeWorkerProcess(request: TritExecutionRequest, context: WorkerContext): Promise<WorkerOutput> {
  const command: SafeCommand = {
    executable: process.execPath,
    args: [context.workerPath],
    cwd: context.workspaceRoot,
  };
  const payload = `${stableJson({
    protocol: RUNNER_PROTOCOL_VERSION,
    request,
    workspaceRoot: context.workspaceRoot,
    repositoryRoot: context.repositoryRoot,
    limits: context.limits,
  })}\n`;
  const processResult = await spawnBounded(command, {
    env: context.environment.values,
    limits: context.limits,
    signal: context.signal,
    input: payload,
  });

  if (processResult.terminationReason === "cancelled") {
    return {
      success: false,
      compilerOutput: processResult.stdout,
      stdout: processResult.stdout,
      stderr: processResult.stderr,
      error: "run cancelled",
      commands: [sanitizeCommand(command, context.workspaceRoot, context.repositoryRoot)],
      commandResults: [processResult],
      workerPid: 0,
      terminationReason: "cancelled",
    };
  }
  if (processResult.terminationReason === "timeout" || processResult.terminationReason === "output-limit") {
    return {
      success: false,
      compilerOutput: processResult.stdout,
      stdout: processResult.stdout,
      stderr: processResult.stderr,
      error: processResult.terminationReason === "timeout" ? "worker wall-time limit exceeded" : "worker output limit exceeded",
      commands: [sanitizeCommand(command, context.workspaceRoot, context.repositoryRoot)],
      commandResults: [processResult],
      workerPid: 0,
      terminationReason: processResult.terminationReason,
    };
  }
  if (processResult.spawnError || processResult.code !== 0) {
    throw new RunnerWorkerError(processResult.spawnError || `worker exited with code ${processResult.code}`);
  }

  try {
    const response = JSON.parse(processResult.stdout.trim()) as WorkerOutput;
    if (!response || typeof response.success !== "boolean" || !Array.isArray(response.commands)) {
      throw new Error("worker response does not match the runner protocol");
    }
    return {
      ...response,
      stdout: `${response.stdout || ""}`.slice(0, context.limits.outputBytes),
      stderr: `${response.stderr || ""}${processResult.stderr}`.slice(0, context.limits.outputBytes),
      commands: response.commands.map((item) => sanitizeCommand(item, context.workspaceRoot, context.repositoryRoot)),
      commandResults: [...(response.commandResults || []), processResult],
    };
  } catch (error) {
    throw new RunnerWorkerError(`worker protocol failure: ${String(error)}`);
  }
}

function resultFingerprint(output: Partial<WorkerOutput>): string {
  return `sha256:${sha256(stableJson({
    success: Boolean(output.success),
    cycles: output.cycles ?? null,
    status: output.status ?? null,
    r13: output.r13 ?? null,
    consoleOutput: output.consoleOutput || "",
    error: output.error || "",
  }))}`;
}

function runtimeFingerprint(): string {
  if (process.versions.bun) return `bun/${process.versions.bun}`;
  return `node/${process.versions.node}`;
}

function makeRunId(): string {
  return `run_${Date.now().toString(36)}_${randomUUID().replace(/-/g, "")}`;
}

export class SecureExecutionQueue {
  private readonly store: ArtifactStore;
  private readonly repositoryRoot: string;
  private readonly workerPath: string;
  private readonly concurrency: number;
  private readonly maxRetries: number;
  private readonly limits: RunLimits;
  private readonly executor: WorkerExecutor;
  private readonly jobs = new Map<string, QueueJob>();
  private readonly pending: QueueJob[] = [];
  private active = 0;

  constructor(options: QueueOptions) {
    this.store = new ArtifactStore(options.artifactRoot);
    this.repositoryRoot = path.resolve(options.repositoryRoot);
    this.workerPath = path.resolve(options.workerPath);
    this.concurrency = Math.max(1, Math.min(8, Math.floor(options.concurrency || 1)));
    this.maxRetries = Math.max(0, Math.min(3, Math.floor(options.maxRetries || 0)));
    this.limits = normalizeRunLimits(options.limits);
    this.executor = options.workerExecutor || executeWorkerProcess;
  }

  submit(request: TritExecutionRequest): RunHandle {
    validateExecutionRequest(request);
    const runId = makeRunId();
    let resolveResult!: (result: ExecutionResult) => void;
    const result = new Promise<ExecutionResult>((resolve) => {
      resolveResult = resolve;
    });
    const job: QueueJob = {
      runId,
      request: { ...request },
      submittedAt: new Date().toISOString(),
      controller: new AbortController(),
      attempts: 0,
      state: "queued",
      cancelRequested: false,
      resolve: resolveResult,
      result,
    };
    this.jobs.set(runId, job);
    this.pending.push(job);
    void this.pump();
    return {
      runId,
      result,
      cancel: () => this.cancel(runId),
    };
  }

  status(runId: string): RunStatus | null {
    const job = this.jobs.get(runId);
    if (!job) return null;
    return { runId, state: job.state, attempts: job.attempts };
  }

  async getRecord(runId: string): Promise<ImmutableRunRecord | null> {
    return this.store.readRecord(runId);
  }

  private cancel(runId: string): boolean {
    const job = this.jobs.get(runId);
    if (!job || job.state === "succeeded" || job.state === "failed" || job.state === "timed_out" || job.state === "cancelled") return false;
    job.cancelRequested = true;
    job.controller.abort();
    if (job.state === "queued") {
      void this.finalize(job, undefined, "cancelled", "cancelled_by_request");
    }
    return true;
  }

  private async pump(): Promise<void> {
    await this.store.init();
    while (this.active < this.concurrency && this.pending.length > 0) {
      const job = this.pending.shift()!;
      if (job.cancelRequested) continue;
      this.active += 1;
      void this.run(job).finally(() => {
        this.active -= 1;
        void this.pump();
      });
    }
  }

  private async run(job: QueueJob): Promise<void> {
    job.state = "running";
    job.startedAt = new Date().toISOString();
    let lastError: unknown;
    let workspaceRoot: string | undefined;
    try {
      workspaceRoot = await this.store.createWorkspace(job.runId);
      for (let attempt = 0; attempt <= this.maxRetries; attempt += 1) {
        job.attempts = attempt + 1;
        if (job.cancelRequested) {
          await this.finalize(job, undefined, "cancelled", "cancelled_by_request", workspaceRoot);
          return;
        }
        const context: WorkerContext = {
          runId: job.runId,
          workspaceRoot,
          repositoryRoot: this.repositoryRoot,
          workerPath: this.workerPath,
          limits: this.limits,
          environment: createSanitizedEnvironment(workspaceRoot),
          signal: job.controller.signal,
        };
        try {
          const output = await this.executor(job.request, context);
          if (job.cancelRequested || output.terminationReason === "cancelled") {
            await this.finalize(job, output, "cancelled", "cancelled_by_request", workspaceRoot);
          } else if (output.terminationReason === "timeout") {
            await this.finalize(job, output, "timed_out", "wall_time_limit", workspaceRoot);
          } else if (output.terminationReason === "output-limit") {
            await this.finalize(job, output, "failed", "output_limit", workspaceRoot);
          } else {
            await this.finalize(job, output, output.success ? "succeeded" : "failed", null, workspaceRoot);
          }
          return;
        } catch (error) {
          lastError = error;
          if (job.cancelRequested) {
            await this.finalize(job, undefined, "cancelled", "cancelled_by_request", workspaceRoot);
            return;
          }
          const retryable = error instanceof RunnerWorkerError && error.retryable;
          if (!retryable || attempt >= this.maxRetries) break;
        }
      }
      await this.finalize(
        job,
        {
          success: false,
          compilerOutput: "",
          stdout: "",
          stderr: "",
          error: lastError ? String(lastError) : "worker failed",
          commands: [],
          commandResults: [],
          workerPid: null,
        },
        "failed",
        "worker_failure",
        workspaceRoot,
      );
    } catch (error) {
      await this.finalize(
        job,
        {
          success: false,
          compilerOutput: "",
          stdout: "",
          stderr: "",
          error: String(error),
          commands: [],
          commandResults: [],
          workerPid: null,
        },
        "failed",
        "runner_failure",
        workspaceRoot,
      );
    } finally {
      if (workspaceRoot) await this.store.removeWorkspace(job.runId);
    }
  }

  private async finalize(
    job: QueueJob,
    output: WorkerOutput | undefined,
    state: Exclude<RunState, "queued" | "running">,
    terminationReason: string | null,
    workspaceRoot?: string,
  ): Promise<void> {
    if (job.state === "succeeded" || job.state === "failed" || job.state === "timed_out" || job.state === "cancelled") return;
    const startedAt = job.startedAt || job.submittedAt;
    const finishedAt = new Date().toISOString();
    const safeOutput: WorkerOutput = output || {
      success: false,
      compilerOutput: "",
      stdout: "",
      stderr: "",
      error: terminationReason || "run failed",
      commands: [],
      commandResults: [],
      workerPid: 0,
    };
    const normalizedCommands = safeOutput.commands.map((command) => sanitizeCommand(
      command,
      workspaceRoot || this.store.root,
      this.repositoryRoot,
    ));
    const artifacts: Record<string, ArtifactRef> = {
      stdout: await this.store.put(safeOutput.stdout || "", "text/plain"),
      stderr: await this.store.put(safeOutput.stderr || "", "text/plain"),
      compiler_output: await this.store.put(safeOutput.compilerOutput || "", "text/plain"),
      assembly: await this.store.put(safeOutput.assembly || "", "text/plain"),
      trace: await this.store.putJson({
        schema: "treatcode.run-trace.v1",
        run_id: job.runId,
        commands: normalizedCommands,
        command_results: safeOutput.commandResults,
      }),
      diagnostics: await this.store.putJson({
        schema: "treatcode.run-diagnostics.v1",
        run_id: job.runId,
        state,
        termination_reason: terminationReason || safeOutput.terminationReason || null,
        limits: this.limits,
        isolation: {
          network: "disabled",
          host_secrets: "not-forwarded",
          workspace: "ephemeral",
        },
      }),
      result: await this.store.putJson({
        success: safeOutput.success,
        compileTimeCycles: safeOutput.compileTimeCycles ?? null,
        cycles: safeOutput.cycles ?? null,
        status: safeOutput.status ?? null,
        r13: safeOutput.r13 ?? null,
        consoleOutput: safeOutput.consoleOutput || "",
        registers: safeOutput.registers || {},
        error: safeOutput.error || null,
      }),
    };
    const recordBase: Omit<ImmutableRunRecord, "record_hash"> = {
      schema: "treatcode.run-record.v1",
      run_id: job.runId,
      state,
      submitted_at: job.submittedAt,
      started_at: startedAt,
      finished_at: finishedAt,
      source_commit: job.request.sourceCommit,
      input_hashes: {
        source: `sha256:${sha256(job.request.code)}`,
        request: `sha256:${sha256(stableJson({ ...job.request, code: `sha256:${sha256(job.request.code)}` }))}`,
      },
      runner_image: "treatcode-runner-local-v1",
      toolchain: {
        runner_protocol: RUNNER_PROTOCOL_VERSION,
        runtime: runtimeFingerprint(),
        compiler: "tritc-fixed-argv",
      },
      isolation: {
        base_image: "read-only",
        workspace: "ephemeral",
        network: "disabled",
        host_secrets: "not-forwarded",
        shell: "disabled",
      },
      limits: this.limits,
      commands: normalizedCommands,
      environment: createSanitizedEnvironment(workspaceRoot || this.store.root).metadata,
      attempts: job.attempts,
      worker_pid: safeOutput.workerPid || null,
      exit_status: {
        worker_code: safeOutput.commandResults.length > 0 ? safeOutput.commandResults[safeOutput.commandResults.length - 1].code : null,
        worker_signal: safeOutput.commandResults.length > 0 ? safeOutput.commandResults[safeOutput.commandResults.length - 1].signal : null,
        command_results: safeOutput.commandResults,
      },
      termination_reason: terminationReason || safeOutput.terminationReason || null,
      correctness: {
        success: state === "succeeded" && safeOutput.success,
        result_fingerprint: resultFingerprint(safeOutput),
      },
      artifact_hashes: Object.fromEntries(Object.entries(artifacts).map(([key, value]) => [key, value.hash])),
    };
    const committed = await this.store.commitRecord(job.runId, recordBase);
    job.state = state;
    job.resolve({
      runId: job.runId,
      state,
      success: state === "succeeded" && safeOutput.success,
      compileTimeCycles: safeOutput.compileTimeCycles,
      assembly: safeOutput.assembly,
      cycles: safeOutput.cycles,
      status: safeOutput.status,
      r13: safeOutput.r13,
      consoleOutput: safeOutput.consoleOutput,
      registers: safeOutput.registers || {},
      compilerOutput: safeOutput.compilerOutput || "",
      error: safeOutput.error,
      evidence: {
        recordPath: committed.path,
        recordHash: committed.record.record_hash,
        artifacts,
      },
      record: committed.record,
    });
  }
}

export function runnerWorkerPath(moduleDirectory: string): string {
  const candidate = path.join(moduleDirectory, "worker.ts");
  if (!existsSync(candidate)) throw new Error(`runner worker is missing: ${candidate}`);
  return candidate;
}
