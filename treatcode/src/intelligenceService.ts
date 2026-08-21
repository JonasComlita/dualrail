import { createHash, randomUUID } from "node:crypto";
import { existsSync, lstatSync, mkdirSync, readFileSync, renameSync, rmSync, writeFileSync } from "node:fs";
import { lstat, mkdir, readdir, readFile, rm, stat, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import {
  EXECUTION_REQUEST_SCHEMA,
  SecureExecutionQueue,
  type ExecutionResult,
  type RunnerEngine,
} from "./runner/secure-runner";

/**
 * The intelligence benchmark is deliberately kept separate from the legacy
 * challenge catalogue.  This module owns the server-side state machine and
 * exposes plain data contracts for the HTTP layer to adapt.
 */

export const INTELLIGENCE_MANIFEST_SCHEMA = "treatcode.intelligence.benchmark-manifest.v1" as const;
export const INTELLIGENCE_PROTOCOL_SCHEMA = "treatcode.intelligence.protocol.v1" as const;
export const INTELLIGENCE_PUBLIC_TEST_SCHEMA = "treatcode.intelligence.public-tests.v1" as const;
export const INTELLIGENCE_HIDDEN_TEST_SCHEMA = "treatcode.intelligence.hidden-tests.v1" as const;
export const INTELLIGENCE_TASK_ID = "TC-SWE-001" as const;
export const INTELLIGENCE_SUITE_SCHEMA = "treatcode.intelligence.suite.v1" as const;
export const INTELLIGENCE_TRIAL_COUNT = 4 as const;
export const INTELLIGENCE_MAX_FILE_BYTES = 65_536 as const;
export const INTELLIGENCE_SAFE_INPUT_BOUND = 1_000_000 as const;

const ALLOWLISTED_FILES = ["src/compare.trit", "src/median.trit"] as const;
type AllowlistedFile = string;

export type IntelligenceTrialStatus =
  | "open"
  | "public_failed"
  | "public_passed"
  | "hidden_submitted"
  | "tampered";

export type IntelligenceRunStatus = "open" | "complete" | "tampered";
export type IntelligenceLeaderboardView = "official" | "self-reported";

export type IntelligenceErrorCode =
  | "fixture_unavailable"
  | "invalid_manifest"
  | "invalid_request"
  | "run_not_found"
  | "trial_not_found"
  | "invalid_trial_state"
  | "path_not_allowed"
  | "path_traversal"
  | "symlink_not_allowed"
  | "file_too_large"
  | "invalid_file_content"
  | "public_gate_required"
  | "hidden_submission_already_recorded"
  | "hidden_submission_sealed"
  | "trial_tampered"
  | "attestation_required"
  | "attestation_rejected"
  | "aggregate_incomplete"
  | "leaderboard_invalid";

export class IntelligenceServiceError extends Error {
  readonly code: IntelligenceErrorCode;
  readonly status: number;
  readonly details?: Record<string, unknown>;

  constructor(code: IntelligenceErrorCode, message: string, status = 400, details?: Record<string, unknown>) {
    super(message);
    this.name = "IntelligenceServiceError";
    this.code = code;
    this.status = status;
    this.details = details;
  }
}

interface BenchmarkCase {
  id: string;
  name?: string;
  suite?: string;
  a: number;
  b: number;
  c: number;
  expected: number;
}

interface BenchmarkManifest {
  schema: typeof INTELLIGENCE_MANIFEST_SCHEMA;
  version: number;
  task: {
    id: string;
    title: string;
    kind: string;
    description: string;
    category?: string;
    summary?: string;
    source_root: string;
    allowlisted_files: string[];
    max_file_bytes: number;
    trial_count: number;
    score: {
      formula: string;
      unit: string;
      minimum_trial_count: number;
    };
    runner?: {
      kind: "trit-scalar-return";
      entrypoint: string;
      arity: number;
    };
  };
  public_tests: {
    schema: typeof INTELLIGENCE_PUBLIC_TEST_SCHEMA;
    metadata_path: string;
    cases: BenchmarkCase[];
  };
  hidden_tests: {
    schema: typeof INTELLIGENCE_HIDDEN_TEST_SCHEMA;
    server_only: true;
    path: string;
    case_count: number;
    suites: Array<{ id: string; case_count: number }>;
  };
  protocol: {
    schema: typeof INTELLIGENCE_PROTOCOL_SCHEMA;
    trial_count: number;
    hidden_submissions_per_trial: number;
    public_tests_repeatable: boolean;
    hidden_results: string;
    fresh_fixture_per_trial: boolean;
  };
}

export interface IntelligencePublicCase {
  id: string;
  name: string;
  a: number;
  b: number;
  c: number;
  expected: number;
}

export interface IntelligenceTaskCatalog {
  schema: typeof INTELLIGENCE_MANIFEST_SCHEMA;
  protocol_schema: typeof INTELLIGENCE_PROTOCOL_SCHEMA;
  version: number;
  task: {
    id: string;
    title: string;
    kind: string;
    description: string;
    category?: string;
    summary?: string;
    allowlisted_files: Array<{ path: string; max_bytes: number; starter: string }>;
    trial_count: typeof INTELLIGENCE_TRIAL_COUNT;
    score_formula: "passed / 4 * 100";
  };
  public_tests: {
    schema: typeof INTELLIGENCE_PUBLIC_TEST_SCHEMA;
    cases: IntelligencePublicCase[];
  };
  hidden_tests: {
    server_only: true;
    sealed: true;
    case_count: number;
    suites: Array<{ id: string; case_count: number }>;
  };
}

export interface IntelligenceSuiteCatalog {
  schema: typeof INTELLIGENCE_SUITE_SCHEMA;
  version: number;
  tasks: IntelligenceTaskCatalog[];
}

export interface IntelligenceSuiteTaskDescriptor {
  id: string;
  category: string;
  fixture_path: string;
  summary: string;
}

interface IntelligenceSuiteManifest {
  schema: typeof INTELLIGENCE_SUITE_SCHEMA;
  version: number;
  title: string;
  description: string;
  task_count: number;
  trial_count: number;
  tasks: IntelligenceSuiteTaskDescriptor[];
}

export interface IntelligenceFileView {
  path: string;
  bytes: number;
  sha256: string;
  content: string;
}

export interface IntelligenceTrialView {
  id: string;
  status: IntelligenceTrialStatus;
  public_runs: number;
  public_passed: boolean | null;
  hidden_submitted: boolean;
  hidden_sealed: boolean;
  files: IntelligenceFileView[];
}

export interface IntelligenceRunView {
  schema: "treatcode.intelligence.run.v1";
  run_id: string;
  task_id: string;
  status: IntelligenceRunStatus;
  participant_id: string | null;
  source_commit: string;
  created_at: string;
  trials: IntelligenceTrialView[];
  completed_trials: number;
  hidden_results_sealed: boolean;
  aggregate: IntelligenceAggregate | null;
}

export interface IntelligenceAggregate {
  schema: "treatcode.intelligence.aggregate.v1";
  run_id: string;
  task_id: string;
  trial_count: typeof INTELLIGENCE_TRIAL_COUNT;
  completed_trials: typeof INTELLIGENCE_TRIAL_COUNT;
  passed_trials: number;
  score: number;
  formula: "passed / 4 * 100";
  sealed: true;
  official_eligible: boolean;
  published: boolean;
  trial_ids: string[];
  tested_commit: string;
  completed_at: string;
}

export interface IntelligencePublicCaseResult {
  id: string;
  passed: boolean;
  actual: number | null;
  expected: number;
  error?: string;
}

export interface IntelligencePublicTestReport {
  schema: "treatcode.intelligence.public-test-report.v1";
  run_id: string;
  trial_id: string;
  attempt: number;
  passed: boolean;
  repeatable: true;
  cases: IntelligencePublicCaseResult[];
}

export interface IntelligenceHiddenSubmissionReceipt {
  schema: "treatcode.intelligence.hidden-submission-receipt.v1";
  run_id: string;
  trial_id: string;
  accepted: true;
  sealed: true;
  remaining_trials: number;
  aggregate: IntelligenceAggregate | null;
}

export interface IntelligenceLeaderboardRecord {
  schema: "treatcode.intelligence.leaderboard-record.v1";
  id: string;
  view: "official";
  task_id: string;
  run_id: string;
  participant_id: string | null;
  score: number;
  passed_trials: number;
  trial_count: typeof INTELLIGENCE_TRIAL_COUNT;
  tested_commit: string;
  published_at: string;
  attested: boolean;
  attestation_id: string | null;
}

export interface IntelligenceSelfReportedRecord {
  schema: "treatcode.intelligence.self-reported-leaderboard.v1";
  id: string;
  view: "self-reported";
  task_id: string;
  participant_id: string;
  score: number;
  note: string | null;
  reported_at: string;
}

export interface IntelligenceAttestationRequest {
  run_id: string;
  principal: string;
  model: string;
  model_configuration?: string;
  tested_commit?: string;
  evidence_hashes?: string[];
}

export interface IntelligenceAttestationRecord {
  schema: "treatcode.intelligence.attestation.v1";
  id: string;
  run_id: string;
  principal: string;
  model: string;
  model_configuration: string | null;
  tested_commit: string;
  evidence_hash: string;
  attested_at: string;
}

export interface IntelligenceExecutionInput {
  run_id: string;
  trial_id: string;
  visibility: "public" | "hidden";
  test: {
    id: string;
    a: number;
    b: number;
    c: number;
    expected: number;
  };
  files: Readonly<Record<string, string>>;
}

export interface IntelligenceExecutionOutput {
  /** `success` means the program compiled and completed; correctness is checked by the service. */
  success: boolean;
  value?: number | null;
  r13?: number | null;
  error?: string;
  state?: "succeeded" | "failed" | "timed_out" | "cancelled";
  timed_out?: boolean;
}

export type IntelligenceExecutor = (input: IntelligenceExecutionInput) => Promise<IntelligenceExecutionOutput>;

export interface IntelligenceAttestationContext {
  run_id: string;
  aggregate: IntelligenceAggregate;
  request: IntelligenceAttestationRequest;
}

export type PrivilegedAttestationHook = (context: IntelligenceAttestationContext) => Promise<boolean> | boolean;

export interface IntelligenceServiceOptions {
  fixtureRoot?: string;
  taskId?: string;
  storageRoot?: string;
  statePath?: string;
  artifactRoot?: string;
  repositoryRoot?: string;
  runnerEngine?: RunnerEngine;
  optLevel?: "-O0" | "-O1" | "-O2" | "-O3";
  sourceCommit?: string;
  executor?: IntelligenceExecutor;
  privilegedAttestor?: PrivilegedAttestationHook;
}

export interface IntelligenceRunInput {
  task_id?: string;
  taskId?: string;
  participant_id?: string;
  participantId?: string;
  source_commit?: string;
  sourceCommit?: string;
}

interface InternalTrial {
  id: string;
  workspaceRoot: string;
  files: Map<string, string>;
  status: IntelligenceTrialStatus;
  publicRuns: number;
  publicPassed: boolean | null;
  publicTimedOut: boolean;
  hiddenSubmitted: boolean;
  hiddenPasses: number | null;
  hiddenClean: boolean;
  hiddenSubmittedAt?: string;
}

interface InternalRun {
  id: string;
  taskId: string;
  participantId: string | null;
  sourceCommit: string;
  createdAt: string;
  root: string;
  trials: Map<string, InternalTrial>;
  status: IntelligenceRunStatus;
  lock: Promise<void>;
  aggregate: IntelligenceAggregate | null;
  attestation: IntelligenceAttestationRecord | null;
}

interface LoadedFixture {
  manifest: BenchmarkManifest;
  taskId: string;
  allowlistedFiles: string[];
  files: Map<string, string>;
  publicCases: IntelligencePublicCase[];
  hiddenCases: BenchmarkCase[];
}

interface PersistedIntelligenceState {
  schema: "treatcode.intelligence.state.v1";
  official_records: IntelligenceLeaderboardRecord[];
  self_reported_records: IntelligenceSelfReportedRecord[];
  attestations: IntelligenceAttestationRecord[];
}

function stableJson(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(stableJson).join(",")}]`;
  if (value && typeof value === "object") {
    return `{${Object.entries(value as Record<string, unknown>)
      .sort(([left], [right]) => left.localeCompare(right))
      .map(([key, item]) => `${JSON.stringify(key)}:${stableJson(item)}`)
      .join(",")}}`;
  }
  return JSON.stringify(value);
}

function sha256(value: string | Uint8Array): string {
  return createHash("sha256").update(value).digest("hex");
}

function safeId(value: string, label: string): string {
  if (!/^[A-Za-z0-9._:-]{1,160}$/.test(value) || value.includes("..")) {
    throw new IntelligenceServiceError("invalid_request", `${label} contains an unsafe identifier`, 400);
  }
  return value;
}

function isWithin(root: string, candidate: string): boolean {
  const relative = path.relative(path.resolve(root), path.resolve(candidate));
  return relative === "" || (relative !== ".." && !relative.startsWith(`..${path.sep}`) && !path.isAbsolute(relative));
}

function canonicalRelative(value: unknown): string {
  if (typeof value !== "string" || value.length === 0 || value.includes("\0")) {
    throw new IntelligenceServiceError("path_traversal", "A relative .trit path is required", 400);
  }
  if (value.includes("\\") || value.startsWith("/") || /^[A-Za-z]:/.test(value)) {
    throw new IntelligenceServiceError("path_traversal", "Workspace paths must use safe relative POSIX syntax", 400);
  }
  const parts = value.split("/");
  if (parts.some((part) => part.length === 0 || part === "." || part === ".." || part === ".git")) {
    throw new IntelligenceServiceError("path_traversal", "Workspace paths cannot traverse or address Git metadata", 400);
  }
  return parts.join("/");
}

function parseJson<T>(filePath: string): T {
  try {
    return JSON.parse(readFileSync(filePath, "utf8")) as T;
  } catch (error) {
    throw new IntelligenceServiceError("fixture_unavailable", `Unable to load benchmark fixture ${filePath}: ${String(error)}`, 500);
  }
}

function validateCase(value: unknown, location: string): BenchmarkCase {
  if (!value || typeof value !== "object") throw new IntelligenceServiceError("invalid_manifest", `${location} is not an object`, 500);
  const item = value as Record<string, unknown>;
  const id = typeof item.id === "string" ? item.id : "";
  const values = [item.a, item.b, item.c, item.expected];
  if (!id || values.some((candidate) => typeof candidate !== "number" || !Number.isSafeInteger(candidate) || Math.abs(candidate) > INTELLIGENCE_SAFE_INPUT_BOUND)) {
    throw new IntelligenceServiceError("invalid_manifest", `${location} has an invalid safe-range integer case`, 500);
  }
  return {
    id,
    name: typeof item.name === "string" ? item.name : undefined,
    suite: typeof item.suite === "string" ? item.suite : undefined,
    a: item.a as number,
    b: item.b as number,
    c: item.c as number,
    expected: item.expected as number,
  };
}

function validateFixture(fixtureRoot: string, expectedTaskId?: string): LoadedFixture {
  const manifestPath = path.join(fixtureRoot, "manifest.v1.json");
  const manifest = parseJson<BenchmarkManifest>(manifestPath);
  if (manifest.schema !== INTELLIGENCE_MANIFEST_SCHEMA || manifest.version !== 1) {
    throw new IntelligenceServiceError("invalid_manifest", "The intelligence benchmark manifest is not version 1", 500);
  }
  if (!manifest.task.id || (expectedTaskId && manifest.task.id !== expectedTaskId) || manifest.task.trial_count !== INTELLIGENCE_TRIAL_COUNT || manifest.protocol.trial_count !== INTELLIGENCE_TRIAL_COUNT) {
    throw new IntelligenceServiceError("invalid_manifest", "Intelligence tasks must declare exactly four trials and a matching task id", 500);
  }
  if (manifest.task.max_file_bytes <= 0 || manifest.task.max_file_bytes > INTELLIGENCE_MAX_FILE_BYTES) {
    throw new IntelligenceServiceError("invalid_manifest", "The fixture file limit is outside the safe service bound", 500);
  }
  const allowlistedFiles = [...new Set(manifest.task.allowlisted_files)];
  if (allowlistedFiles.length === 0 || allowlistedFiles.some((item) => {
    try {
      return canonicalRelative(item).length === 0 || !item.endsWith(".trit");
    } catch {
      return true;
    }
  })) {
    throw new IntelligenceServiceError("invalid_manifest", "The fixture allowlist must contain safe .trit paths", 500);
  }
  if (manifest.task.id === INTELLIGENCE_TASK_ID && (allowlistedFiles.length !== ALLOWLISTED_FILES.length || allowlistedFiles.some((item) => !ALLOWLISTED_FILES.includes(item as (typeof ALLOWLISTED_FILES)[number])))) {
    throw new IntelligenceServiceError("invalid_manifest", "The TC-SWE-001 fixture allowlist changed", 500);
  }
  if (manifest.protocol.hidden_submissions_per_trial !== 1 || !manifest.protocol.fresh_fixture_per_trial || manifest.protocol.hidden_results !== "sealed_until_all_trials_complete") {
    throw new IntelligenceServiceError("invalid_manifest", "The hidden-submission protocol is not sealed and one-shot", 500);
  }

  const files = new Map<string, string>();
  for (const relative of allowlistedFiles) {
    const filePath = path.join(fixtureRoot, relative);
    if (!existsSync(filePath) || lstatSync(filePath).isSymbolicLink()) {
      throw new IntelligenceServiceError("fixture_unavailable", `Required fixture file is missing or symlinked: ${relative}`, 500);
    }
    const content = readFileSync(filePath, "utf8");
    if (Buffer.byteLength(content, "utf8") > manifest.task.max_file_bytes) {
      throw new IntelligenceServiceError("invalid_manifest", `Fixture file exceeds the declared limit: ${relative}`, 500);
    }
    files.set(relative, content);
  }

  const publicFixturePath = path.join(fixtureRoot, manifest.public_tests.metadata_path);
  const publicPayload = parseJson<{ schema: string; version: number; visibility: string; cases: unknown[] }>(publicFixturePath);
  if (publicPayload.schema !== INTELLIGENCE_PUBLIC_TEST_SCHEMA || publicPayload.visibility !== "public") {
    throw new IntelligenceServiceError("invalid_manifest", "The public test metadata is not versioned/public", 500);
  }
  const publicCases = publicPayload.cases.map((item, index) => validateCase(item, `public case ${index}`)).map((item) => ({
    id: item.id,
    name: item.name || item.id,
    a: item.a,
    b: item.b,
    c: item.c,
    expected: item.expected,
  }));
  if (publicCases.length === 0) throw new IntelligenceServiceError("invalid_manifest", "The public test set is empty", 500);

  const hiddenFixturePath = path.join(fixtureRoot, manifest.hidden_tests.path);
  const hiddenPayload = parseJson<{ schema: string; version: number; visibility: string; cases: unknown[] }>(hiddenFixturePath);
  if (hiddenPayload.schema !== INTELLIGENCE_HIDDEN_TEST_SCHEMA || hiddenPayload.visibility !== "server-only") {
    throw new IntelligenceServiceError("invalid_manifest", "The hidden verifier fixture is not server-only", 500);
  }
  const hiddenCases = hiddenPayload.cases.map((item, index) => validateCase(item, `hidden case ${index}`));
  if (hiddenCases.length !== manifest.hidden_tests.case_count || hiddenCases.length === 0) {
    throw new IntelligenceServiceError("invalid_manifest", "The hidden case count does not match its manifest", 500);
  }
  return { manifest, taskId: manifest.task.id, allowlistedFiles, files, publicCases, hiddenCases };
}

function defaultFixtureRoot(): string {
  return path.resolve(__dirname, "..", "..", "benchmarks", "intelligence");
}

function defaultRepositoryRoot(): string {
  return path.resolve(__dirname, "..", "..");
}

function defaultStorageRoot(): string {
  return process.env.TREATCODE_INTELLIGENCE_ROOT || path.join(os.tmpdir(), "treatcode-intelligence");
}

/**
 * TC-SWE-001 originally persisted its state directly at <root>/state.v1.json.
 * The versioned suite stores each task under <root>/<task-id>/.  Preserve a
 * pilot score when a server upgrades to the suite without deleting or
 * rewriting the legacy file; the task service will validate the copied state
 * with its normal schema and task-id checks.
 */
function migrateLegacyTaskState(storageRoot: string): void {
  const legacyPath = path.join(storageRoot, "state.v1.json");
  const taskStorageRoot = path.join(storageRoot, INTELLIGENCE_TASK_ID);
  const taskPath = path.join(taskStorageRoot, "state.v1.json");
  if (!existsSync(legacyPath) || existsSync(taskPath)) return;

  let raw: string;
  let parsed: unknown;
  try {
    raw = readFileSync(legacyPath, "utf8");
    parsed = JSON.parse(raw) as unknown;
  } catch {
    // Leave malformed legacy state for the normal task-service error path.
    return;
  }
  if (!parsed || typeof parsed !== "object") return;
  const state = parsed as Record<string, unknown>;
  if (state.schema !== "treatcode.intelligence.state.v1" || !Array.isArray(state.official_records) || !Array.isArray(state.self_reported_records) || !Array.isArray(state.attestations)) return;

  // Never reinterpret a state file that belongs to another task.  Empty
  // legacy state is safe to migrate and keeps the layout deterministic.
  const records = [...state.official_records, ...state.self_reported_records];
  if (records.some((item) => !item || typeof item !== "object" || (item as Record<string, unknown>).task_id !== INTELLIGENCE_TASK_ID)) return;

  mkdirSync(taskStorageRoot, { recursive: true });
  const temporaryPath = `${taskPath}.${process.pid}.${randomUUID().replace(/-/g, "")}.migration.tmp`;
  try {
    writeFileSync(temporaryPath, raw, { encoding: "utf8", mode: 0o600, flag: "wx" });
    try {
      renameSync(temporaryPath, taskPath);
    } catch (error) {
      // Another suite instance may have migrated first; retain its valid
      // destination and avoid treating that race as a server-start failure.
      if ((error as NodeJS.ErrnoException).code !== "EEXIST" && (error as NodeJS.ErrnoException).code !== "EPERM" && (error as NodeJS.ErrnoException).code !== "ENOTEMPTY") throw error;
    }
  } finally {
    if (existsSync(temporaryPath)) rmSync(temporaryPath, { force: true });
  }
}

function makeRunId(): string {
  return `intel_${Date.now().toString(36)}_${randomUUID().replace(/-/g, "")}`;
}

function makeTrialId(runId: string, index: number): string {
  return `${runId}_trial_${index + 1}`;
}

function copyFiles(files: Map<string, string>): Map<string, string> {
  return new Map([...files.entries()].map(([relative, content]) => [relative, content]));
}

function publicCaseCopy(item: IntelligencePublicCase): IntelligencePublicCase {
  return { ...item };
}

function parseExecutionValue(result: ExecutionResult): IntelligenceExecutionOutput {
  return {
    success: result.success && result.state === "succeeded",
    value: result.r13 ?? null,
    r13: result.r13 ?? null,
    error: result.error,
    state: result.state,
    timed_out: result.state === "timed_out",
  };
}

export class IntelligenceBenchmarkService {
  readonly fixtureRoot: string;
  readonly taskId: string;
  readonly storageRoot: string;
  readonly statePath: string;
  private readonly fixture: LoadedFixture;
  private readonly maxFileBytes: number;
  private readonly sourceCommit: string;
  private readonly runnerEngine: RunnerEngine;
  private readonly optLevel: "-O0" | "-O1" | "-O2" | "-O3";
  private readonly repositoryRoot: string;
  private readonly artifactRoot: string;
  private readonly customExecutor?: IntelligenceExecutor;
  private readonly privilegedAttestor?: PrivilegedAttestationHook;
  private readonly queue: SecureExecutionQueue | null;
  private readonly runs = new Map<string, InternalRun>();
  private readonly officialRecords: IntelligenceLeaderboardRecord[] = [];
  private readonly selfReportedRecords: IntelligenceSelfReportedRecord[] = [];
  private readonly persistedAttestations = new Map<string, IntelligenceAttestationRecord>();

  constructor(options: IntelligenceServiceOptions = {}) {
    this.fixtureRoot = path.resolve(options.fixtureRoot || defaultFixtureRoot());
    this.storageRoot = path.resolve(options.storageRoot || defaultStorageRoot());
    this.statePath = path.resolve(options.statePath || path.join(this.storageRoot, "state.v1.json"));
    this.repositoryRoot = path.resolve(options.repositoryRoot || defaultRepositoryRoot());
    this.artifactRoot = path.resolve(options.artifactRoot || path.join(this.storageRoot, "runner-evidence"));
    this.runnerEngine = options.runnerEngine || "bootstrap";
    this.optLevel = options.optLevel || "-O2";
    this.sourceCommit = options.sourceCommit || "tc-swe-001-v1";
    this.customExecutor = options.executor;
    // The HTTP adapter still performs normal bearer authorization.  The
    // built-in hook only accepts the pre-provisioned service identity; tests
    // and embedders may replace it with a stronger external attestor.
    this.privilegedAttestor = options.privilegedAttestor || ((context) => {
      const principal = context.request.principal;
      return principal === "tc:identity:demo-service" || principal.startsWith("tc:identity:service-");
    });
    this.fixture = validateFixture(this.fixtureRoot, options.taskId);
    this.taskId = this.fixture.taskId;
    this.maxFileBytes = Math.min(this.fixture.manifest.task.max_file_bytes, INTELLIGENCE_MAX_FILE_BYTES);
    this.loadPersistedState();
    this.queue = this.customExecutor ? null : new SecureExecutionQueue({
      artifactRoot: this.artifactRoot,
      repositoryRoot: this.repositoryRoot,
      workerPath: path.join(__dirname, "runner", "worker.ts"),
      concurrency: 1,
      maxRetries: 0,
      limits: {
        wallTimeMs: 15_000,
        cpuMs: 15_000,
        outputBytes: 256 * 1024,
        memoryMb: 256,
        processCount: 4,
      },
    });
  }

  /** Public catalog projection. Hidden test values and paths never enter this object. */
  catalog(): IntelligenceTaskCatalog {
    const manifest = this.fixture.manifest;
    return {
      schema: INTELLIGENCE_MANIFEST_SCHEMA,
      protocol_schema: INTELLIGENCE_PROTOCOL_SCHEMA,
      version: manifest.version,
      task: {
        id: this.taskId,
        title: manifest.task.title,
        kind: manifest.task.kind,
        description: manifest.task.description,
        allowlisted_files: this.fixture.allowlistedFiles.map((relative) => ({ path: relative, max_bytes: this.maxFileBytes, starter: this.fixture.files.get(relative) || "" })),
        trial_count: INTELLIGENCE_TRIAL_COUNT,
        score_formula: "passed / 4 * 100",
      },
      public_tests: {
        schema: INTELLIGENCE_PUBLIC_TEST_SCHEMA,
        cases: this.fixture.publicCases.map(publicCaseCopy),
      },
      hidden_tests: {
        server_only: true,
        sealed: true,
        case_count: this.fixture.hiddenCases.length,
        suites: manifest.hidden_tests.suites.map((suite) => ({ ...suite })),
      },
    };
  }

  getTaskCatalog(): IntelligenceTaskCatalog {
    return this.catalog();
  }

  publicTests(): IntelligencePublicCase[] {
    return this.fixture.publicCases.map(publicCaseCopy);
  }

  async startRun(input: IntelligenceRunInput | string = {}): Promise<IntelligenceRunView> {
    const normalizedInput: IntelligenceRunInput = typeof input === "string" ? { participant_id: input } : input;
    const requestedTask = normalizedInput.task_id ?? normalizedInput.taskId;
    if (requestedTask !== undefined && requestedTask !== this.taskId) {
      throw new IntelligenceServiceError("invalid_request", `This service is bound to ${this.taskId}`, 400, { task_id: this.taskId });
    }
    const participantValue = normalizedInput.participant_id ?? normalizedInput.participantId;
    const commitValue = normalizedInput.source_commit ?? normalizedInput.sourceCommit;
    const participantId = participantValue === undefined ? null : safeId(participantValue, "participant id");
    const sourceCommit = commitValue === undefined ? this.sourceCommit : safeId(commitValue, "source commit");
    await mkdir(this.storageRoot, { recursive: true });
    const runId = makeRunId();
    const runRoot = path.join(this.storageRoot, runId);
    await mkdir(runRoot, { recursive: false });
    const trials = new Map<string, InternalTrial>();
    try {
      for (let index = 0; index < INTELLIGENCE_TRIAL_COUNT; index += 1) {
        const trialId = makeTrialId(runId, index);
        const workspaceRoot = path.join(runRoot, trialId);
        const files = copyFiles(this.fixture.files);
        for (const [relative, content] of files) {
          await mkdir(path.dirname(path.join(workspaceRoot, relative)), { recursive: true });
          await writeFile(path.join(workspaceRoot, relative), content, { encoding: "utf8", mode: 0o600, flag: "wx" });
        }
        trials.set(trialId, {
          id: trialId,
          workspaceRoot,
          files,
          status: "open",
          publicRuns: 0,
          publicPassed: null,
          publicTimedOut: false,
          hiddenSubmitted: false,
          hiddenPasses: null,
          hiddenClean: true,
        });
      }
    } catch (error) {
      await rm(runRoot, { recursive: true, force: true });
      throw new IntelligenceServiceError("fixture_unavailable", `Unable to materialize a clean intelligence run: ${String(error)}`, 500);
    }
    const run: InternalRun = {
      id: runId,
      taskId: this.taskId,
      participantId,
      sourceCommit,
      createdAt: new Date().toISOString(),
      root: runRoot,
      trials,
      status: "open",
      lock: Promise.resolve(),
      aggregate: null,
      attestation: null,
    };
    this.runs.set(runId, run);
    return this.viewRun(run);
  }

  async getRun(runId: string): Promise<IntelligenceRunView> {
    return this.viewRun(this.requireRun(runId));
  }

  async getTrial(runId: string, trialId: string): Promise<IntelligenceTrialView> {
    const run = this.requireRun(runId);
    return this.viewTrial(run, this.requireTrial(run, trialId));
  }

  async trialFiles(runId: string, trialId: string): Promise<IntelligenceFileView[]> {
    const run = this.requireRun(runId);
    const trial = this.requireTrial(run, trialId);
    await this.verifyTrial(run, trial);
    return this.fileViews(trial);
  }

  async readFiles(runId: string, trialId: string): Promise<IntelligenceFileView[]> {
    return this.trialFiles(runId, trialId);
  }

  async writeFile(runId: string, trialId: string, relativePath: string, content: string): Promise<{ path: AllowlistedFile; bytes: number; sha256: string }> {
    return this.withRunLock(runId, async (run) => {
      const trial = this.requireTrial(run, trialId);
      if (trial.hiddenSubmitted) throw new IntelligenceServiceError("hidden_submission_sealed", "Writes are disabled after hidden submission", 409);
      if (trial.status === "tampered" || run.status === "tampered") throw new IntelligenceServiceError("trial_tampered", "Tampered trial workspaces cannot be edited", 409);
      const canonical = canonicalRelative(relativePath);
      if (!this.fixture.allowlistedFiles.includes(canonical)) {
        throw new IntelligenceServiceError("path_not_allowed", "Only the task's allowlisted Trit files may be edited", 403, { allowlisted_files: [...this.fixture.allowlistedFiles] });
      }
      if (typeof content !== "string") throw new IntelligenceServiceError("invalid_file_content", "File content must be text", 400);
      const bytes = Buffer.byteLength(content, "utf8");
      if (bytes > this.maxFileBytes) throw new IntelligenceServiceError("file_too_large", `File exceeds the ${this.maxFileBytes}-byte limit`, 413);
      const target = path.resolve(trial.workspaceRoot, canonical);
      if (!isWithin(trial.workspaceRoot, target)) throw new IntelligenceServiceError("path_traversal", "Workspace path escaped its trial root", 400);
      await this.assertNoSymlinkAncestors(trial.workspaceRoot, target);
      await this.verifyTrial(run, trial);
      try {
        const existing = await stat(target);
        if (!existing.isFile()) throw new IntelligenceServiceError("path_not_allowed", "Only regular files may be edited", 400);
      } catch (error) {
        if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
      }
      await writeFile(target, content, { encoding: "utf8", mode: 0o600 });
      trial.files.set(canonical, content);
      // Keep the returned hash useful for evidence without returning source to the caller.
      return { path: canonical, bytes, sha256: sha256(content) };
    });
  }

  async updateFile(runId: string, trialId: string, relativePath: string, content: string): Promise<{ path: AllowlistedFile; bytes: number; sha256: string }> {
    return this.writeFile(runId, trialId, relativePath, content);
  }

  async runPublicTests(runId: string, trialId: string): Promise<IntelligencePublicTestReport> {
    return this.withRunLock(runId, async (run) => {
      const trial = this.requireTrial(run, trialId);
      if (trial.hiddenSubmitted) throw new IntelligenceServiceError("hidden_submission_sealed", "Public tests cannot run after hidden submission", 409);
      await this.verifyTrial(run, trial);
      const files = await this.fileContents(trial);
      const cases: IntelligencePublicCaseResult[] = [];
      let timedOut = false;
      for (const test of this.fixture.publicCases) {
        const outcome = await this.execute({ run_id: run.id, trial_id: trial.id, visibility: "public", test, files });
        timedOut ||= Boolean(outcome.timed_out || outcome.state === "timed_out");
        const actual = outcome.value ?? outcome.r13 ?? null;
        cases.push({ id: test.id, passed: Boolean(outcome.success) && actual === test.expected, actual, expected: test.expected, ...(outcome.error ? { error: outcome.error } : {}) });
      }
      trial.publicRuns += 1;
      trial.publicTimedOut ||= timedOut;
      trial.publicPassed = cases.every((item) => item.passed);
      trial.status = trial.publicPassed ? "public_passed" : "public_failed";
      return {
        schema: "treatcode.intelligence.public-test-report.v1",
        run_id: run.id,
        trial_id: trial.id,
        attempt: trial.publicRuns,
        passed: trial.publicPassed,
        repeatable: true,
        cases,
      };
    });
  }

  async runPublic(runId: string, trialId: string): Promise<IntelligencePublicTestReport> {
    return this.runPublicTests(runId, trialId);
  }

  /** Convenience adapter for a one-button HTTP route: public gate, then one-shot hidden submit. */
  async submitTrial(runId: string, trialId: string): Promise<{ public: IntelligencePublicTestReport; hidden: IntelligenceHiddenSubmissionReceipt }> {
    const publicReport = await this.runPublicTests(runId, trialId);
    if (!publicReport.passed) throw new IntelligenceServiceError("public_gate_required", "The public test gate did not pass", 422, { report: publicReport });
    const hidden = await this.submitHidden(runId, trialId);
    return { public: publicReport, hidden };
  }

  async submitHidden(runId: string, trialId: string): Promise<IntelligenceHiddenSubmissionReceipt> {
    return this.withRunLock(runId, async (run) => {
      const trial = this.requireTrial(run, trialId);
      if (trial.hiddenSubmitted) throw new IntelligenceServiceError("hidden_submission_already_recorded", "Exactly one hidden submission is permitted per trial", 409);
      if (trial.status === "tampered" || run.status === "tampered") throw new IntelligenceServiceError("trial_tampered", "Tampered trial workspaces cannot be submitted", 409);
      if (trial.publicRuns === 0 || trial.publicPassed !== true) throw new IntelligenceServiceError("public_gate_required", "A passing public test run is required before hidden submission", 409);
      await this.verifyTrial(run, trial);

      // Seal before executing. A thrown compiler/adapter error still consumes the
      // one-shot submission and can never be retried as a second hidden attempt.
      trial.hiddenSubmitted = true;
      trial.status = "hidden_submitted";
      trial.hiddenSubmittedAt = new Date().toISOString();
      let passed = 0;
      let clean = !trial.publicTimedOut;
      const files = await this.fileContents(trial);
      for (const test of this.fixture.hiddenCases) {
        let outcome: IntelligenceExecutionOutput;
        try {
          outcome = await this.execute({ run_id: run.id, trial_id: trial.id, visibility: "hidden", test, files });
        } catch {
          outcome = { success: false, state: "failed", error: "hidden verifier execution failed" };
        }
        if (outcome.state === "timed_out" || outcome.timed_out) clean = false;
        if (!outcome.success) clean = false;
        if (outcome.state && outcome.state !== "succeeded") clean = false;
        const actual = outcome.value ?? outcome.r13 ?? null;
        if (outcome.success && actual === test.expected) passed += 1;
      }
      trial.hiddenPasses = passed;
      trial.hiddenClean = clean;
      trial.status = "hidden_submitted";
      await this.finishAggregateIfReady(run);
      const remaining = [...run.trials.values()].filter((item) => !item.hiddenSubmitted).length;
      return {
        schema: "treatcode.intelligence.hidden-submission-receipt.v1",
        run_id: run.id,
        trial_id: trial.id,
        accepted: true,
        sealed: true,
        remaining_trials: remaining,
        aggregate: run.aggregate ? { ...run.aggregate, trial_ids: [...run.aggregate.trial_ids] } : null,
      };
    });
  }

  async submitHiddenTests(runId: string, trialId: string): Promise<IntelligenceHiddenSubmissionReceipt> {
    return this.submitHidden(runId, trialId);
  }

  async getAggregate(runId: string): Promise<IntelligenceAggregate | { run_id: string; task_id: string; complete: false; completed_trials: number; trial_count: typeof INTELLIGENCE_TRIAL_COUNT; score: null; sealed: false; published: false }> {
    const run = this.requireRun(runId);
    if (!run.aggregate) {
      return {
        run_id: run.id,
        task_id: run.taskId,
        complete: false,
        completed_trials: [...run.trials.values()].filter((trial) => trial.hiddenSubmitted).length,
        trial_count: INTELLIGENCE_TRIAL_COUNT,
        score: null,
        sealed: false,
        published: false,
      };
    }
    return { ...run.aggregate, trial_ids: [...run.aggregate.trial_ids] };
  }

  async aggregate(runId: string): Promise<IntelligenceAggregate | { run_id: string; task_id: string; complete: false; completed_trials: number; trial_count: typeof INTELLIGENCE_TRIAL_COUNT; score: null; sealed: false; published: false }> {
    return this.getAggregate(runId);
  }

  recordSelfReported(input: { participant_id: string; score: number; note?: string }): IntelligenceSelfReportedRecord {
    const participantId = safeId(input.participant_id, "participant id");
    if (typeof input.score !== "number" || !Number.isFinite(input.score) || input.score < 0 || input.score > 100) {
      throw new IntelligenceServiceError("invalid_request", "Self-reported score must be between 0 and 100", 400);
    }
    const note = input.note === undefined ? null : String(input.note).slice(0, 2_000);
    const record: IntelligenceSelfReportedRecord = {
      schema: "treatcode.intelligence.self-reported-leaderboard.v1",
      id: `self_${Date.now().toString(36)}_${randomUUID().replace(/-/g, "")}`,
      view: "self-reported",
      task_id: this.taskId,
      participant_id: participantId,
      score: Math.round(input.score * 100) / 100,
      note,
      reported_at: new Date().toISOString(),
    };
    this.selfReportedRecords.push(record);
    this.persistState();
    return { ...record };
  }

  recordSelfReportedLeaderboard(input: { participant_id: string; score: number; note?: string }): IntelligenceSelfReportedRecord {
    return this.recordSelfReported(input);
  }

  leaderboard(view: IntelligenceLeaderboardView = "official"): Array<IntelligenceLeaderboardRecord | IntelligenceSelfReportedRecord> {
    if (view === "self-reported") return this.selfReportedRecords.map((record) => ({ ...record }));
    return this.officialRecords.map((record) => ({ ...record }));
  }

  getOfficialLeaderboard(): IntelligenceLeaderboardRecord[] {
    return this.officialRecords.map((record) => ({ ...record }));
  }

  getSelfReportedLeaderboard(): IntelligenceSelfReportedRecord[] {
    return this.selfReportedRecords.map((record) => ({ ...record }));
  }

  async attest(request: IntelligenceAttestationRequest): Promise<IntelligenceAttestationRecord> {
    const run = this.requireRun(request.run_id);
    const aggregate = run.aggregate;
    if (!aggregate || !aggregate.official_eligible) throw new IntelligenceServiceError("aggregate_incomplete", "Only a complete clean aggregate can be attested", 409);
    if (run.attestation) return { ...run.attestation };
    if (!this.privilegedAttestor) throw new IntelligenceServiceError("attestation_required", "A privileged attestation hook is not configured", 403);
    if (typeof request.principal !== "string" || request.principal.length === 0 || typeof request.model !== "string" || request.model.length === 0) {
      throw new IntelligenceServiceError("invalid_request", "Attestation principal and model are required", 400);
    }
    const accepted = await this.privilegedAttestor({ run_id: run.id, aggregate, request: { ...request } });
    if (!accepted) throw new IntelligenceServiceError("attestation_rejected", "The privileged attestation hook rejected this run", 403);
    const evidenceHash = `sha256:${sha256(stableJson({ aggregate, principal: request.principal, model: request.model, model_configuration: request.model_configuration || null, evidence_hashes: [...(request.evidence_hashes || [])].sort() }))}`;
    const record: IntelligenceAttestationRecord = {
      schema: "treatcode.intelligence.attestation.v1",
      id: `att_${Date.now().toString(36)}_${randomUUID().replace(/-/g, "")}`,
      run_id: run.id,
      principal: request.principal.slice(0, 160),
      model: request.model.slice(0, 160),
      model_configuration: request.model_configuration ? request.model_configuration.slice(0, 500) : null,
      tested_commit: request.tested_commit || aggregate.tested_commit,
      evidence_hash: evidenceHash,
      attested_at: new Date().toISOString(),
    };
    run.attestation = record;
    aggregate.published = true;
    this.persistedAttestations.set(run.id, { ...record });
    const official = this.officialRecords.find((item) => item.run_id === run.id);
    if (official) {
      official.attested = true;
      official.attestation_id = record.id;
    } else {
      this.officialRecords.push({
        schema: "treatcode.intelligence.leaderboard-record.v1",
        id: `official_${Date.now().toString(36)}_${randomUUID().replace(/-/g, "")}`,
        view: "official",
        task_id: run.taskId,
        run_id: run.id,
        participant_id: run.participantId,
        score: aggregate.score,
        passed_trials: aggregate.passed_trials,
        trial_count: INTELLIGENCE_TRIAL_COUNT,
        tested_commit: aggregate.tested_commit,
        published_at: aggregate.completed_at,
        attested: true,
        attestation_id: record.id,
      });
    }
    this.officialRecords.sort((left, right) => right.score - left.score || left.published_at.localeCompare(right.published_at));
    this.persistState();
    return { ...record };
  }

  attestOfficial(request: IntelligenceAttestationRequest): Promise<IntelligenceAttestationRecord> {
    return this.attest(request);
  }

  private loadPersistedState(): void {
    let parsed: unknown;
    try {
      parsed = JSON.parse(readFileSync(this.statePath, "utf8")) as unknown;
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === "ENOENT") return;
      throw new IntelligenceServiceError("fixture_unavailable", `Unable to load intelligence state: ${String(error)}`, 500);
    }
    if (!parsed || typeof parsed !== "object" || (parsed as Record<string, unknown>).schema !== "treatcode.intelligence.state.v1") {
      throw new IntelligenceServiceError("fixture_unavailable", "The persisted intelligence state has an unsupported schema", 500);
    }
    const state = parsed as PersistedIntelligenceState;
    if (!Array.isArray(state.official_records) || !Array.isArray(state.self_reported_records) || !Array.isArray(state.attestations)) {
      throw new IntelligenceServiceError("fixture_unavailable", "The persisted intelligence state is incomplete", 500);
    }
    for (const record of state.official_records) {
      if (!record || record.schema !== "treatcode.intelligence.leaderboard-record.v1" || record.view !== "official" || record.task_id !== this.taskId || typeof record.run_id !== "string" || typeof record.score !== "number") {
        throw new IntelligenceServiceError("fixture_unavailable", "The persisted official leaderboard contains an invalid record", 500);
      }
      if (record.attested !== true || typeof record.attestation_id !== "string" || record.attestation_id.length === 0) {
        // Before attestation-gated publication was introduced, clean pilot
        // aggregates were persisted as official rows. Preserve their score,
        // but do not let an unbound historical record violate the current
        // official leaderboard contract.
        this.selfReportedRecords.push({
          schema: "treatcode.intelligence.self-reported-leaderboard.v1",
          id: `legacy_${record.id}`,
          view: "self-reported",
          task_id: record.task_id,
          participant_id: record.participant_id || "legacy-participant",
          score: record.score,
          note: "Legacy score retained before attestation-gated official publication",
          reported_at: record.published_at,
        });
        continue;
      }
      this.officialRecords.push({ ...record });
    }
    for (const record of state.self_reported_records) {
      if (!record || record.schema !== "treatcode.intelligence.self-reported-leaderboard.v1" || record.view !== "self-reported" || record.task_id !== this.taskId || typeof record.participant_id !== "string" || typeof record.score !== "number") {
        throw new IntelligenceServiceError("fixture_unavailable", "The persisted self-reported leaderboard contains an invalid record", 500);
      }
      this.selfReportedRecords.push({ ...record });
    }
    for (const record of state.attestations) {
      if (!record || record.schema !== "treatcode.intelligence.attestation.v1" || typeof record.run_id !== "string" || typeof record.evidence_hash !== "string") {
        throw new IntelligenceServiceError("fixture_unavailable", "The persisted attestation metadata contains an invalid record", 500);
      }
      this.persistedAttestations.set(record.run_id, { ...record });
    }
    this.officialRecords.sort((left, right) => right.score - left.score || left.published_at.localeCompare(right.published_at));
  }

  private persistState(): void {
    const state: PersistedIntelligenceState = {
      schema: "treatcode.intelligence.state.v1",
      official_records: this.officialRecords.map((record) => ({ ...record })),
      self_reported_records: this.selfReportedRecords.map((record) => ({ ...record })),
      attestations: [...this.persistedAttestations.values()].map((record) => ({ ...record })),
    };
    const directory = path.dirname(this.statePath);
    mkdirSync(directory, { recursive: true });
    const temporaryPath = `${this.statePath}.${process.pid}.${randomUUID().replace(/-/g, "")}.tmp`;
    const backupPath = `${this.statePath}.${process.pid}.previous`;
    try {
      writeFileSync(temporaryPath, `${JSON.stringify(state, null, 2)}\n`, { encoding: "utf8", mode: 0o600, flag: "wx" });
      try {
        // POSIX rename replaces atomically. Windows rejects replacement, so the
        // fallback briefly moves only this service-owned state file aside and
        // restores it if the final rename fails.
        renameSync(temporaryPath, this.statePath);
      } catch (error) {
        if ((error as NodeJS.ErrnoException).code !== "EEXIST" && (error as NodeJS.ErrnoException).code !== "EPERM" && (error as NodeJS.ErrnoException).code !== "ENOTEMPTY") throw error;
        if (existsSync(this.statePath)) renameSync(this.statePath, backupPath);
        try {
          renameSync(temporaryPath, this.statePath);
          if (existsSync(backupPath)) rmSync(backupPath, { force: true });
        } catch (replaceError) {
          if (!existsSync(this.statePath) && existsSync(backupPath)) renameSync(backupPath, this.statePath);
          throw replaceError;
        }
      }
    } finally {
      if (existsSync(temporaryPath)) rmSync(temporaryPath, { force: true });
      if (existsSync(backupPath)) rmSync(backupPath, { force: true });
    }
  }

  private requireRun(runId: string): InternalRun {
    const safe = safeId(runId, "run id");
    const run = this.runs.get(safe);
    if (!run) throw new IntelligenceServiceError("run_not_found", "Intelligence run not found", 404);
    return run;
  }

  private requireTrial(run: InternalRun, trialId: string): InternalTrial {
    const safe = safeId(trialId, "trial id");
    const trial = run.trials.get(safe);
    if (!trial) throw new IntelligenceServiceError("trial_not_found", "Intelligence trial not found", 404);
    return trial;
  }

  private async withRunLock<T>(runId: string, operation: (run: InternalRun) => Promise<T>): Promise<T> {
    const run = this.requireRun(runId);
    const previous = run.lock;
    let release!: () => void;
    run.lock = new Promise<void>((resolve) => { release = resolve; });
    await previous;
    try {
      return await operation(run);
    } finally {
      release();
    }
  }

  private async fileContents(trial: InternalTrial): Promise<Readonly<Record<string, string>>> {
    const files = {} as Record<string, string>;
    for (const relative of this.fixture.allowlistedFiles) files[relative] = await readFile(path.join(trial.workspaceRoot, relative), "utf8");
    return files;
  }

  private fileViews(trial: InternalTrial): IntelligenceFileView[] {
    return this.fixture.allowlistedFiles.map((relative) => {
      const content = trial.files.get(relative) || "";
      return { path: relative, bytes: Buffer.byteLength(content, "utf8"), sha256: `sha256:${sha256(content)}`, content };
    });
  }

  private viewTrial(run: InternalRun, trial: InternalTrial): IntelligenceTrialView {
    return {
      id: trial.id,
      status: trial.status,
      public_runs: trial.publicRuns,
      public_passed: trial.publicPassed,
      hidden_submitted: trial.hiddenSubmitted,
      hidden_sealed: trial.hiddenSubmitted,
      files: this.fileViews(trial),
    };
  }

  private viewRun(run: InternalRun): IntelligenceRunView {
    const completedTrials = [...run.trials.values()].filter((trial) => trial.hiddenSubmitted).length;
    return {
      schema: "treatcode.intelligence.run.v1",
      run_id: run.id,
      task_id: run.taskId,
      status: run.status,
      participant_id: run.participantId,
      source_commit: run.sourceCommit,
      created_at: run.createdAt,
      trials: [...run.trials.values()].map((trial) => this.viewTrial(run, trial)),
      completed_trials: completedTrials,
      hidden_results_sealed: Boolean(run.aggregate),
      aggregate: run.aggregate ? { ...run.aggregate, trial_ids: [...run.aggregate.trial_ids] } : null,
    };
  }

  private async assertNoSymlinkAncestors(root: string, target: string): Promise<void> {
    const resolvedRoot = path.resolve(root);
    let cursor = path.resolve(target);
    if (!isWithin(resolvedRoot, cursor)) throw new IntelligenceServiceError("path_traversal", "Workspace path escaped its trial root", 400);
    while (true) {
      try {
        const item = await stat(cursor);
        // stat follows links; lstat is required to identify the link itself.
        const raw = await lstat(cursor);
        if (raw.isSymbolicLink()) throw new IntelligenceServiceError("symlink_not_allowed", "Symlinked workspace paths are not accepted", 400);
        if (cursor !== resolvedRoot && !item.isDirectory() && cursor !== path.resolve(target)) throw new IntelligenceServiceError("path_not_allowed", "Workspace ancestors must be directories", 400);
      } catch (error) {
        if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
      }
      if (cursor === resolvedRoot) break;
      const parent = path.dirname(cursor);
      if (parent === cursor || !isWithin(resolvedRoot, parent)) throw new IntelligenceServiceError("path_traversal", "Workspace path escaped its trial root", 400);
      cursor = parent;
    }
  }

  private async verifyTrial(run: InternalRun, trial: InternalTrial): Promise<void> {
    if (trial.status === "tampered" || run.status === "tampered") throw new IntelligenceServiceError("trial_tampered", "The trial workspace failed integrity checks", 409);
    try {
      await this.assertNoSymlinkAncestors(trial.workspaceRoot, path.join(trial.workspaceRoot, "src"));
      const expected = new Set<string>(this.fixture.allowlistedFiles);
      const seen = new Set<string>();
      const walk = async (directory: string): Promise<void> => {
        for (const entry of await readdir(directory, { withFileTypes: true })) {
          const full = path.join(directory, entry.name);
          const relative = path.relative(trial.workspaceRoot, full).split(path.sep).join("/");
          if (entry.isSymbolicLink()) throw new IntelligenceServiceError("symlink_not_allowed", "Trial workspaces cannot contain symlinks", 400);
          if (entry.isDirectory()) await walk(full);
          else if (entry.isFile()) seen.add(relative);
          else throw new IntelligenceServiceError("trial_tampered", "Trial workspace contains a special file", 409);
        }
      };
      await walk(trial.workspaceRoot);
      if (seen.size !== expected.size || [...expected].some((relative) => !seen.has(relative))) {
        throw new IntelligenceServiceError("trial_tampered", "Trial workspace contains unexpected or missing files", 409);
      }
      for (const relative of this.fixture.allowlistedFiles) {
        const content = await readFile(path.join(trial.workspaceRoot, relative), "utf8");
        if (Buffer.byteLength(content, "utf8") > this.maxFileBytes) throw new IntelligenceServiceError("file_too_large", "Trial file exceeds its declared limit", 413);
        trial.files.set(relative, content);
      }
    } catch (error) {
      if (error instanceof IntelligenceServiceError && ["symlink_not_allowed", "trial_tampered", "file_too_large"].includes(error.code)) {
        trial.status = "tampered";
        run.status = "tampered";
        throw new IntelligenceServiceError("trial_tampered", error.message, 409, { trial_id: trial.id, cause: error.code });
      }
      throw error;
    }
  }

  private async execute(input: IntelligenceExecutionInput): Promise<IntelligenceExecutionOutput> {
    if (this.customExecutor) return this.customExecutor(input);
    if (!this.queue) throw new IntelligenceServiceError("fixture_unavailable", "No intelligence execution adapter is configured", 500);
    const runner = this.fixture.manifest.task.runner || { kind: "trit-scalar-return" as const, entrypoint: "median", arity: 3 };
    const args = [input.test.a, input.test.b, input.test.c].slice(0, Math.max(0, Math.min(3, Math.floor(runner.arity))));
    const source = this.fixture.allowlistedFiles.map((relative) => input.files[relative] || "").join("\n\n");
    const code = `${source}\n\nfn main() -> t40 {\n    return ${runner.entrypoint}(${args.join(", ")});\n}\n`;
    const result = await this.queue.submit({
      schema: EXECUTION_REQUEST_SCHEMA,
      kind: "trit.compile-and-run",
      code,
      engine: this.runnerEngine,
      optLevel: this.optLevel,
      sourceCommit: this.sourceCommit,
    }).result;
    return parseExecutionValue(result);
  }

  private async finishAggregateIfReady(run: InternalRun): Promise<void> {
    if ([...run.trials.values()].some((trial) => !trial.hiddenSubmitted)) return;
    for (const trial of run.trials.values()) {
      try {
        await this.verifyTrial(run, trial);
      } catch {
        run.status = "tampered";
      }
    }
    const trials = [...run.trials.values()];
    const passedTrials = trials.filter((trial) => trial.hiddenPasses === this.fixture.hiddenCases.length).length;
    const officialEligible = run.status !== "tampered" && trials.every((trial) => trial.hiddenSubmitted && trial.hiddenClean);
    run.aggregate = {
      schema: "treatcode.intelligence.aggregate.v1",
      run_id: run.id,
      task_id: run.taskId,
      trial_count: INTELLIGENCE_TRIAL_COUNT,
      completed_trials: INTELLIGENCE_TRIAL_COUNT,
      passed_trials: passedTrials,
      score: (passedTrials / INTELLIGENCE_TRIAL_COUNT) * 100,
      formula: "passed / 4 * 100",
      sealed: true,
      official_eligible: officialEligible,
      // A clean aggregate is eligible for official publication, but the
      // leaderboard record is withheld until a privileged attestation binds
      // the model/configuration and evidence hashes to this run.
      published: false,
      trial_ids: trials.map((trial) => trial.id),
      tested_commit: run.sourceCommit,
      completed_at: new Date().toISOString(),
    };
    run.status = run.status === "tampered" ? "tampered" : "complete";
    // The clean result remains available through the sealed run aggregate;
    // only attest() publishes an official leaderboard record.
  }
}

export interface IntelligenceSuiteServiceOptions extends IntelligenceServiceOptions {
  suiteRoot?: string;
}

/**
 * Dispatches the generic single-task engine across the versioned suite. The
 * legacy TC-SWE-001 calls intentionally remain valid when task_id is omitted.
 */
export class IntelligenceSuiteService {
  readonly suiteRoot: string;
  readonly storageRoot: string;
  private readonly manifest: IntelligenceSuiteManifest;
  private readonly descriptors = new Map<string, IntelligenceSuiteTaskDescriptor>();
  private readonly services = new Map<string, IntelligenceBenchmarkService>();
  private readonly runOwners = new Map<string, IntelligenceBenchmarkService>();

  constructor(options: IntelligenceSuiteServiceOptions = {}) {
    this.suiteRoot = path.resolve(options.suiteRoot || options.fixtureRoot || defaultFixtureRoot());
    this.storageRoot = path.resolve(options.storageRoot || defaultStorageRoot());
    this.manifest = parseJson<IntelligenceSuiteManifest>(path.join(this.suiteRoot, "suite.v1.json"));
    if (this.manifest.schema !== INTELLIGENCE_SUITE_SCHEMA || this.manifest.version !== 1 || this.manifest.trial_count !== INTELLIGENCE_TRIAL_COUNT || this.manifest.task_count !== this.manifest.tasks.length || this.manifest.tasks.length < 5) {
      throw new IntelligenceServiceError("invalid_manifest", "The intelligence suite must be versioned and contain at least five four-trial tasks", 500);
    }
    for (const descriptor of this.manifest.tasks) {
      if (!/^[A-Za-z0-9._-]+$/.test(descriptor.id) || this.descriptors.has(descriptor.id) || !descriptor.category || !descriptor.fixture_path) {
        throw new IntelligenceServiceError("invalid_manifest", "The intelligence suite contains an invalid or duplicate task descriptor", 500);
      }
      const fixtureRoot = path.resolve(this.suiteRoot, descriptor.fixture_path);
      if (!isWithin(this.suiteRoot, fixtureRoot)) throw new IntelligenceServiceError("invalid_manifest", `Task fixture escaped the suite root: ${descriptor.id}`, 500);
      if (descriptor.id === INTELLIGENCE_TASK_ID) migrateLegacyTaskState(this.storageRoot);
      const taskStorageRoot = path.join(this.storageRoot, descriptor.id);
      const taskService = new IntelligenceBenchmarkService({
        ...options,
        fixtureRoot,
        taskId: descriptor.id,
        storageRoot: taskStorageRoot,
        statePath: path.join(taskStorageRoot, "state.v1.json"),
        sourceCommit: options.sourceCommit || `${descriptor.id.toLowerCase()}-v1`,
      });
      this.descriptors.set(descriptor.id, { ...descriptor });
      this.services.set(descriptor.id, taskService);
    }
    if (!this.services.has(INTELLIGENCE_TASK_ID)) throw new IntelligenceServiceError("invalid_manifest", "The intelligence suite must retain TC-SWE-001", 500);
  }

  taskIds(): string[] {
    return [...this.services.keys()];
  }

  taskService(taskId: string = INTELLIGENCE_TASK_ID): IntelligenceBenchmarkService {
    const service = this.services.get(taskId);
    if (!service) throw new IntelligenceServiceError("invalid_request", `Unknown intelligence task ${taskId}`, 404, { task_id: taskId, task_ids: this.taskIds() });
    return service;
  }

  catalog(taskId: string = INTELLIGENCE_TASK_ID): IntelligenceTaskCatalog {
    const descriptor = this.descriptors.get(taskId);
    const catalog = this.taskService(taskId).catalog();
    return descriptor ? { ...catalog, task: { ...catalog.task, category: descriptor.category, summary: descriptor.summary } } : catalog;
  }

  getTaskCatalog(taskId: string = INTELLIGENCE_TASK_ID): IntelligenceTaskCatalog {
    return this.catalog(taskId);
  }

  suiteCatalog(): IntelligenceSuiteCatalog {
    return {
      schema: INTELLIGENCE_SUITE_SCHEMA,
      version: this.manifest.version,
      tasks: this.taskIds().map((taskId) => this.catalog(taskId)),
    };
  }

  publicTests(taskId: string = INTELLIGENCE_TASK_ID): IntelligencePublicCase[] {
    return this.taskService(taskId).publicTests();
  }

  async startRun(input: IntelligenceRunInput | string = {}): Promise<IntelligenceRunView> {
    const normalized: IntelligenceRunInput = typeof input === "string" ? { participant_id: input } : input;
    const taskId = normalized.task_id ?? normalized.taskId ?? INTELLIGENCE_TASK_ID;
    const run = await this.taskService(taskId).startRun({ ...normalized, task_id: taskId });
    this.runOwners.set(run.run_id, this.taskService(taskId));
    return run;
  }

  async getRun(runId: string): Promise<IntelligenceRunView> {
    return (await this.owner(runId)).getRun(runId);
  }

  async getTrial(runId: string, trialId: string): Promise<IntelligenceTrialView> {
    return (await this.owner(runId)).getTrial(runId, trialId);
  }

  async trialFiles(runId: string, trialId: string): Promise<IntelligenceFileView[]> {
    return (await this.owner(runId)).trialFiles(runId, trialId);
  }

  async readFiles(runId: string, trialId: string): Promise<IntelligenceFileView[]> {
    return this.trialFiles(runId, trialId);
  }

  async writeFile(runId: string, trialId: string, relativePath: string, content: string): Promise<{ path: string; bytes: number; sha256: string }> {
    return (await this.owner(runId)).writeFile(runId, trialId, relativePath, content);
  }

  async updateFile(runId: string, trialId: string, relativePath: string, content: string): Promise<{ path: string; bytes: number; sha256: string }> {
    return this.writeFile(runId, trialId, relativePath, content);
  }

  async runPublicTests(runId: string, trialId: string): Promise<IntelligencePublicTestReport> {
    return (await this.owner(runId)).runPublicTests(runId, trialId);
  }

  async runPublic(runId: string, trialId: string): Promise<IntelligencePublicTestReport> {
    return this.runPublicTests(runId, trialId);
  }

  async submitHidden(runId: string, trialId: string): Promise<IntelligenceHiddenSubmissionReceipt> {
    return (await this.owner(runId)).submitHidden(runId, trialId);
  }

  async submitHiddenTests(runId: string, trialId: string): Promise<IntelligenceHiddenSubmissionReceipt> {
    return this.submitHidden(runId, trialId);
  }

  async submitTrial(runId: string, trialId: string): Promise<{ public: IntelligencePublicTestReport; hidden: IntelligenceHiddenSubmissionReceipt }> {
    return (await this.owner(runId)).submitTrial(runId, trialId);
  }

  async getAggregate(runId: string): Promise<IntelligenceAggregate | { run_id: string; task_id: string; complete: false; completed_trials: number; trial_count: typeof INTELLIGENCE_TRIAL_COUNT; score: null; sealed: false; published: false }> {
    return (await this.owner(runId)).getAggregate(runId);
  }

  async aggregate(runId: string): Promise<IntelligenceAggregate | { run_id: string; task_id: string; complete: false; completed_trials: number; trial_count: typeof INTELLIGENCE_TRIAL_COUNT; score: null; sealed: false; published: false }> {
    return this.getAggregate(runId);
  }

  recordSelfReported(input: { task_id?: string; taskId?: string; participant_id: string; score: number; note?: string }): IntelligenceSelfReportedRecord {
    const taskId = input.task_id ?? input.taskId ?? INTELLIGENCE_TASK_ID;
    return this.taskService(taskId).recordSelfReported({ participant_id: input.participant_id, score: input.score, note: input.note });
  }

  recordSelfReportedLeaderboard(input: { task_id?: string; taskId?: string; participant_id: string; score: number; note?: string }): IntelligenceSelfReportedRecord {
    return this.recordSelfReported(input);
  }

  leaderboard(view: IntelligenceLeaderboardView = "official"): Array<IntelligenceLeaderboardRecord | IntelligenceSelfReportedRecord> {
    const rows = this.taskIds().flatMap((taskId) => this.taskService(taskId).leaderboard(view));
    return rows.sort((left, right) => right.score - left.score).map((row) => ({ ...row }));
  }

  getOfficialLeaderboard(): IntelligenceLeaderboardRecord[] {
    return this.taskIds().flatMap((taskId) => this.taskService(taskId).getOfficialLeaderboard()).sort((left, right) => right.score - left.score).map((row) => ({ ...row }));
  }

  getSelfReportedLeaderboard(): IntelligenceSelfReportedRecord[] {
    return this.taskIds().flatMap((taskId) => this.taskService(taskId).getSelfReportedLeaderboard()).sort((left, right) => right.score - left.score).map((row) => ({ ...row }));
  }

  async attest(request: IntelligenceAttestationRequest): Promise<IntelligenceAttestationRecord> {
    return (await this.owner(request.run_id)).attest(request);
  }

  attestOfficial(request: IntelligenceAttestationRequest): Promise<IntelligenceAttestationRecord> {
    return this.attest(request);
  }

  private async owner(runId: string): Promise<IntelligenceBenchmarkService> {
    const known = this.runOwners.get(runId);
    if (known) return known;
    for (const service of this.services.values()) {
      try {
        await service.getRun(runId);
        this.runOwners.set(runId, service);
        return service;
      } catch (error) {
        if (!(error instanceof IntelligenceServiceError) || error.code !== "run_not_found") throw error;
      }
    }
    throw new IntelligenceServiceError("run_not_found", "Intelligence run not found", 404);
  }
}

export const intelligenceService = new IntelligenceSuiteService();

// Short aliases keep the transport layer free to use either the explicit
// benchmark name or the generic service name without duplicating adapters.
export const IntelligenceService = IntelligenceBenchmarkService;
export default IntelligenceBenchmarkService;
