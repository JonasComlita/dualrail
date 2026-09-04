import { createHash } from "node:crypto";
import { lstat, readFile, realpath } from "node:fs/promises";
import path from "node:path";
import { auditIntelligenceV3TaskPackage, type IntelligenceV3PackageAudit } from "./intelligenceV3Packaging";
import {
  EXECUTION_REQUEST_SCHEMA,
  SecureExecutionQueue,
  type ExecutionResult,
  type RunnerEngine,
} from "./runner/secure-runner";

export const INTELLIGENCE_V3_PUBLIC_TASK_SCHEMA = "treatcode.intelligence.public-task.v3" as const;
export const INTELLIGENCE_V3_PRIVATE_GRADER_SCHEMA = "treatcode.intelligence.private-grader.v3" as const;
export const INTELLIGENCE_V3_CASES_SCHEMA = "treatcode.intelligence.cases.v3" as const;

interface PublicTaskManifest {
  schema: typeof INTELLIGENCE_V3_PUBLIC_TASK_SCHEMA;
  version: 3;
  task_id: string;
  title: string;
  source_files: string[];
  entrypoint: string;
  arity: number;
  public_cases_path: string;
  limits: {
    maximum_file_bytes: number;
    wall_clock_ms_per_case: number;
    maximum_cases: number;
  };
}

interface PrivateGraderManifest {
  schema: typeof INTELLIGENCE_V3_PRIVATE_GRADER_SCHEMA;
  version: 3;
  task_id: string;
  participant_manifest_sha256: string;
  hidden_cases_path: string;
  required_suites: string[];
  adversarial_suites: string[];
}

interface ScalarCase {
  id: string;
  args: number[];
  expected: number;
  suite: string;
}

interface CasesFile {
  schema: typeof INTELLIGENCE_V3_CASES_SCHEMA;
  version: 3;
  visibility: "public" | "private";
  task_id: string;
  cases: ScalarCase[];
}

interface ExecutionQueue {
  submit(request: {
    schema: typeof EXECUTION_REQUEST_SCHEMA;
    kind: "trit.compile-and-run";
    code: string;
    engine: RunnerEngine;
    optLevel: "-O0" | "-O1" | "-O2" | "-O3";
    sourceCommit: string;
  }): { result: Promise<ExecutionResult> };
}

export interface IntelligenceV3ExecutorOptions {
  repository_root: string;
  subject_workspace_root: string;
  participant_root: string;
  grader_root: string;
  artifact_root: string;
  worker_path?: string;
  engine?: RunnerEngine;
  opt_level?: "-O0" | "-O1" | "-O2" | "-O3";
  source_commit?: string;
  queue?: ExecutionQueue;
}

export interface IntelligenceV3PublicReport {
  schema: "treatcode.intelligence.public-report.v3";
  task_id: string;
  passed: boolean;
  cases: Array<{ id: string; suite: string; passed: boolean; actual: number | null; expected: number; evidence_hash: string; error?: string }>;
}

export interface IntelligenceV3SealedReport {
  schema: "treatcode.intelligence.sealed-report.v3";
  task_id: string;
  sealed: true;
  passed: boolean;
  passed_cases: number;
  total_cases: number;
  required_suites_passed: number;
  required_suites_total: number;
  adversarial_cases_passed: number;
  adversarial_cases_total: number;
  evidence_hash: string;
  package_audit: Pick<IntelligenceV3PackageAudit, "participant_bundle_hash" | "grader_bundle_hash">;
}

function sha256(value: string): string {
  return createHash("sha256").update(value).digest("hex");
}

function canonicalJson(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(canonicalJson).join(",")}]`;
  if (value && typeof value === "object") {
    return `{${Object.entries(value as Record<string, unknown>).sort(([a], [b]) => a.localeCompare(b)).map(([key, item]) => `${JSON.stringify(key)}:${canonicalJson(item)}`).join(",")}}`;
  }
  return JSON.stringify(value);
}

function safeRelative(value: string, label: string): string {
  if (typeof value !== "string" || value.length === 0 || path.isAbsolute(value)) throw new Error(`${label} must be a non-empty relative path`);
  const normalized = value.replace(/\\/g, "/");
  if (normalized.split("/").some((segment) => segment === "" || segment === "." || segment === "..")) throw new Error(`${label} contains an unsafe segment`);
  return normalized;
}

function inside(root: string, candidate: string): boolean {
  const relative = path.relative(root, candidate);
  return relative === "" || (!relative.startsWith(`..${path.sep}`) && relative !== ".." && !path.isAbsolute(relative));
}

async function readRegularFile(root: string, relative: string, maximumBytes: number): Promise<string> {
  const safe = safeRelative(relative, "package path");
  const absolute = path.resolve(root, safe);
  if (!inside(root, absolute)) throw new Error("package path escaped its root");
  const metadata = await lstat(absolute);
  if (!metadata.isFile() || metadata.isSymbolicLink()) throw new Error(`${safe} must be a regular non-symlink file`);
  if (metadata.size > maximumBytes) throw new Error(`${safe} exceeds its byte limit`);
  return readFile(absolute, "utf8");
}

function assertManifest(manifest: PublicTaskManifest): void {
  if (manifest.schema !== INTELLIGENCE_V3_PUBLIC_TASK_SCHEMA || manifest.version !== 3 || !/^TC-V3-\d{3}$/.test(manifest.task_id)) throw new Error("invalid v3 public task manifest");
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(manifest.entrypoint) || !Number.isInteger(manifest.arity) || manifest.arity < 0 || manifest.arity > 3) throw new Error("invalid v3 scalar entrypoint");
  if (!Array.isArray(manifest.source_files) || manifest.source_files.length < 2 || new Set(manifest.source_files).size !== manifest.source_files.length || manifest.source_files.some((file) => !file.endsWith(".trit"))) throw new Error("v3 task must expose at least two distinct Trit modules");
  if (!Number.isSafeInteger(manifest.limits.maximum_file_bytes) || manifest.limits.maximum_file_bytes <= 0 || manifest.limits.maximum_file_bytes > 1_048_576) throw new Error("invalid v3 task file limit");
  if (!Number.isSafeInteger(manifest.limits.maximum_cases) || manifest.limits.maximum_cases < 1 || manifest.limits.maximum_cases > 256) throw new Error("invalid v3 case limit");
}

function assertCases(payload: CasesFile, taskId: string, visibility: CasesFile["visibility"], arity: number, maximumCases: number): void {
  if (payload.schema !== INTELLIGENCE_V3_CASES_SCHEMA || payload.version !== 3 || payload.visibility !== visibility || payload.task_id !== taskId) throw new Error(`invalid ${visibility} v3 cases file`);
  if (!Array.isArray(payload.cases) || payload.cases.length === 0 || payload.cases.length > maximumCases) throw new Error(`invalid ${visibility} v3 case count`);
  const ids = new Set<string>();
  for (const item of payload.cases) {
    if (!item || !/^[A-Za-z0-9._-]+$/.test(item.id) || ids.has(item.id)) throw new Error(`invalid or duplicate ${visibility} case id`);
    ids.add(item.id);
    if (!Array.isArray(item.args) || item.args.length !== arity || item.args.some((arg) => !Number.isSafeInteger(arg)) || !Number.isSafeInteger(item.expected) || typeof item.suite !== "string" || item.suite.length === 0) throw new Error(`invalid ${visibility} scalar case ${item.id}`);
  }
}

function executionValue(result: ExecutionResult): number | null {
  if (!result.success) return null;
  if (typeof result.r13 === "number") return result.r13;
  const candidate = result.registers?.r13;
  return typeof candidate === "number" ? candidate : null;
}

export class IntelligenceV3TaskExecutor {
  private hiddenSubmitted = false;

  private constructor(
    private readonly options: IntelligenceV3ExecutorOptions,
    private readonly participantRoot: string,
    private readonly graderRoot: string,
    private readonly manifest: PublicTaskManifest,
    private readonly grader: PrivateGraderManifest,
    private readonly sources: string,
    private readonly publicCases: CasesFile,
    private readonly hiddenCases: CasesFile,
    private readonly audit: IntelligenceV3PackageAudit,
    private readonly queue: ExecutionQueue,
  ) {}

  static async create(options: IntelligenceV3ExecutorOptions): Promise<IntelligenceV3TaskExecutor> {
    const [repositoryRoot, subjectWorkspaceRoot, participantRoot, graderRoot] = await Promise.all([
      realpath(options.repository_root), realpath(options.subject_workspace_root), realpath(options.participant_root), realpath(options.grader_root),
    ]);
    if (inside(subjectWorkspaceRoot, graderRoot)) throw new Error("private grader root must be outside the complete subject workspace");
    if (!inside(subjectWorkspaceRoot, participantRoot)) throw new Error("participant root must be inside the subject workspace");
    const manifestText = await readRegularFile(participantRoot, "task.public.v3.json", 1_048_576);
    const manifest = JSON.parse(manifestText) as PublicTaskManifest;
    assertManifest(manifest);
    const graderText = await readRegularFile(graderRoot, "grader.private.v3.json", 1_048_576);
    const grader = JSON.parse(graderText) as PrivateGraderManifest;
    if (grader.schema !== INTELLIGENCE_V3_PRIVATE_GRADER_SCHEMA || grader.version !== 3 || grader.task_id !== manifest.task_id || grader.participant_manifest_sha256 !== sha256(manifestText)) throw new Error("private grader is invalid or bound to a different participant manifest");
    if (!Array.isArray(grader.required_suites) || grader.required_suites.length === 0 || new Set(grader.required_suites).size !== grader.required_suites.length || !Array.isArray(grader.adversarial_suites) || grader.adversarial_suites.length === 0) throw new Error("private grader suites are incomplete");
    const sourceParts = await Promise.all(manifest.source_files.map(async (file) => {
      safeRelative(file, "source file");
      return readRegularFile(participantRoot, file, manifest.limits.maximum_file_bytes);
    }));
    const publicCases = JSON.parse(await readRegularFile(participantRoot, manifest.public_cases_path, 1_048_576)) as CasesFile;
    const hiddenCases = JSON.parse(await readRegularFile(graderRoot, grader.hidden_cases_path, 1_048_576)) as CasesFile;
    assertCases(publicCases, manifest.task_id, "public", manifest.arity, manifest.limits.maximum_cases);
    assertCases(hiddenCases, manifest.task_id, "private", manifest.arity, manifest.limits.maximum_cases);
    const hiddenSuites = new Set(hiddenCases.cases.map((item) => item.suite));
    if ([...grader.required_suites, ...grader.adversarial_suites].some((suite) => !hiddenSuites.has(suite))) throw new Error("private grader declares a suite with no hidden cases");
    const audit = await auditIntelligenceV3TaskPackage({ task_id: manifest.task_id, participant_root: participantRoot, grader_root: graderRoot, subject_workspace_root: subjectWorkspaceRoot, maximum_file_bytes: manifest.limits.maximum_file_bytes });
    if (!audit.passed) throw new Error(`v3 package isolation audit failed: ${audit.issues.join("; ")}`);
    const queue = options.queue || new SecureExecutionQueue({
      artifactRoot: options.artifact_root,
      repositoryRoot,
      workerPath: options.worker_path || path.join(repositoryRoot, "treatcode", "src", "runner", "worker.ts"),
      concurrency: 1,
      maxRetries: 0,
      limits: { wallTimeMs: manifest.limits.wall_clock_ms_per_case, cpuMs: manifest.limits.wall_clock_ms_per_case, outputBytes: 262_144, memoryMb: 256, processCount: 4 },
    });
    return new IntelligenceV3TaskExecutor(options, participantRoot, graderRoot, manifest, grader, sourceParts.join("\n\n"), publicCases, hiddenCases, audit, queue);
  }

  private async execute(test: ScalarCase): Promise<{ actual: number | null; passed: boolean; evidence_hash: string; error?: string }> {
    const code = `${this.sources}\n\nfn main() -> t40 {\n    return ${this.manifest.entrypoint}(${test.args.join(", ")});\n}\n`;
    const result = await this.queue.submit({ schema: EXECUTION_REQUEST_SCHEMA, kind: "trit.compile-and-run", code, engine: this.options.engine || "bootstrap", optLevel: this.options.opt_level || "-O2", sourceCommit: this.options.source_commit || "intelligence-v3-development" }).result;
    const actual = executionValue(result);
    const commandDiagnostics = result.record.exit_status.command_results.flatMap((item) => [item.stderr, item.stdout]).filter(Boolean).join("\n");
    const error = result.success ? undefined : [result.error, result.compilerOutput, commandDiagnostics].filter(Boolean).join("\n").slice(0, 4000) || "execution failed";
    return { actual, passed: result.success && actual === test.expected, evidence_hash: result.evidence.recordHash, ...(error ? { error } : {}) };
  }

  async runPublic(): Promise<IntelligenceV3PublicReport> {
    if (this.hiddenSubmitted) throw new Error("public tests are sealed after the one-shot hidden submission");
    const cases = [] as IntelligenceV3PublicReport["cases"];
    for (const test of this.publicCases.cases) {
      const result = await this.execute(test);
      cases.push({ id: test.id, suite: test.suite, passed: result.passed, actual: result.actual, expected: test.expected, evidence_hash: result.evidence_hash, ...(result.error ? { error: result.error } : {}) });
    }
    return { schema: "treatcode.intelligence.public-report.v3", task_id: this.manifest.task_id, passed: cases.every((item) => item.passed), cases };
  }

  async submitHidden(): Promise<IntelligenceV3SealedReport> {
    if (this.hiddenSubmitted) throw new Error("exactly one hidden submission is permitted for this task attempt");
    this.hiddenSubmitted = true;
    const outcomes: Array<{ suite: string; passed: boolean; evidence_hash: string }> = [];
    for (const test of this.hiddenCases.cases) {
      const result = await this.execute(test);
      outcomes.push({ suite: test.suite, passed: result.passed, evidence_hash: result.evidence_hash });
    }
    const suitePasses = new Map<string, boolean>();
    for (const suite of this.grader.required_suites) suitePasses.set(suite, outcomes.filter((_, index) => this.hiddenCases.cases[index].suite === suite).every((item) => item.passed));
    const adversarial = outcomes.filter((_, index) => this.grader.adversarial_suites.includes(this.hiddenCases.cases[index].suite));
    const sealedPayload = {
      task_id: this.manifest.task_id,
      case_passes: outcomes.map((item) => item.passed),
      evidence_hashes: outcomes.map((item) => item.evidence_hash),
      participant_bundle_hash: this.audit.participant_bundle_hash,
      grader_bundle_hash: this.audit.grader_bundle_hash,
    };
    const passedCases = outcomes.filter((item) => item.passed).length;
    const requiredSuitesPassed = [...suitePasses.values()].filter(Boolean).length;
    return {
      schema: "treatcode.intelligence.sealed-report.v3",
      task_id: this.manifest.task_id,
      sealed: true,
      passed: passedCases === outcomes.length && requiredSuitesPassed === suitePasses.size,
      passed_cases: passedCases,
      total_cases: outcomes.length,
      required_suites_passed: requiredSuitesPassed,
      required_suites_total: suitePasses.size,
      adversarial_cases_passed: adversarial.filter((item) => item.passed).length,
      adversarial_cases_total: adversarial.length,
      evidence_hash: sha256(canonicalJson(sealedPayload)),
      package_audit: { participant_bundle_hash: this.audit.participant_bundle_hash, grader_bundle_hash: this.audit.grader_bundle_hash },
    };
  }
}
