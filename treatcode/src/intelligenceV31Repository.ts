import { createHash } from "node:crypto";
import {
  chmod,
  copyFile,
  lstat,
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  realpath,
  rm,
  writeFile,
} from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import {
  createSanitizedEnvironment,
  normalizeRunLimits,
  spawnBounded,
  type BoundedCommandResult,
  type RunLimits,
  type SafeCommand,
} from "./runner/secure-runner";

export const INTELLIGENCE_V31_REPOSITORY_TASK_SCHEMA = "treatcode.intelligence.repository-task.v3.1" as const;
export const INTELLIGENCE_V31_REPOSITORY_GRADER_SCHEMA = "treatcode.intelligence.repository-grader.v3.1" as const;
export const INTELLIGENCE_V31_REPOSITORY_REPORT_SCHEMA = "treatcode.intelligence.repository-sealed-report.v3.1" as const;
export const INTELLIGENCE_V31_FROZEN_SUITE_SCHEMA = "treatcode.intelligence.frozen-suite.v3.1" as const;
export const INTELLIGENCE_V31_REPOSITORY_PATCH_SCHEMA = "treatcode.intelligence.repository-patch.v3.1" as const;

const HASH_PATTERN = /^[a-f0-9]{64}$/;
const SAFE_ID = /^[A-Za-z0-9._-]+$/;
const MANIFEST_FILE = "task.repository.v3.1.json";
const GRADER_FILE = "grader.repository.v3.1.json";

export type IntelligenceV31Phase = "diagnostic" | "pilot" | "calibration" | "frozen" | "official";
export type IntelligenceV31SuiteKind = "behavioral" | "adversarial" | "performance";

export interface IntelligenceV31ParticipantFile {
  path: string;
  editable: boolean;
  sha256: string;
}

export interface IntelligenceV31RepositoryTaskManifest {
  schema: typeof INTELLIGENCE_V31_REPOSITORY_TASK_SCHEMA;
  version: "3.1";
  task_id: string;
  title: string;
  phase: IntelligenceV31Phase;
  initial_signal: string;
  known_failing_command_id: string;
  public_command_ids: string[];
  required_trit_change: true;
  files: IntelligenceV31ParticipantFile[];
  limits: {
    wall_clock_ms: number;
    cpu_ms: number;
    memory_mb: number;
    process_count: number;
    output_bytes: number;
    maximum_changed_files: number;
  };
}

export interface IntelligenceV31PrivateSuite {
  id: string;
  kind: IntelligenceV31SuiteKind;
  command_id: string;
  required: true;
}

export interface IntelligenceV31RepositoryGraderManifest {
  schema: typeof INTELLIGENCE_V31_REPOSITORY_GRADER_SCHEMA;
  version: "3.1";
  task_id: string;
  participant_manifest_sha256: string;
  baseline_bundle_sha256: string;
  suites: IntelligenceV31PrivateSuite[];
}

export interface IntelligenceV31CommandContext {
  task_id: string;
  workspace_root: string;
  repository_root: string;
  visibility: "public" | "private";
}

export interface IntelligenceV31CommandSpec {
  executable: string;
  args: string[];
  cwd_relative?: string;
}

export interface IntelligenceV31CommandRegistry {
  resolve(commandId: string, context: IntelligenceV31CommandContext): IntelligenceV31CommandSpec | undefined;
}

export interface IntelligenceV31RepositoryGradeOptions {
  task_id: string;
  attempt_id: string;
  repository_root: string;
  subject_workspace_root: string;
  baseline_root: string;
  submission_root?: string;
  submission_patch_path?: string;
  grader_root: string;
  evidence_root: string;
  released_bundle_hash: string;
  command_registry: IntelligenceV31CommandRegistry;
  require_verified_infrastructure?: boolean;
  network_isolation?: {
    enforcement: "development_proxy_only" | "verified_os_or_container";
    provider: string;
    evidence_sha256?: string;
  };
  resource_isolation?: {
    enforcement: "development_wall_output_only" | "verified_os_or_container";
    provider: string;
    evidence_sha256?: string;
  };
  evidence_store?: {
    enforcement: "development_hash_bound_files" | "verified_append_only";
    provider: string;
    evidence_sha256?: string;
  };
}

export interface IntelligenceV31RepositoryPatch {
  schema: typeof INTELLIGENCE_V31_REPOSITORY_PATCH_SCHEMA;
  version: "3.1";
  task_id: string;
  base_bundle_sha256: string;
  files: Array<{ path: string; base_sha256: string; replacement_utf8: string }>;
}

export interface IntelligenceV31RepositorySealedReport {
  schema: typeof INTELLIGENCE_V31_REPOSITORY_REPORT_SCHEMA;
  task_id: string;
  attempt_id: string;
  sealed: true;
  official: false;
  submission_kind: "snapshot" | "replacement_patch";
  passed: boolean;
  public_commands_passed: number;
  public_commands_total: number;
  required_suites_passed: number;
  required_suites_total: number;
  behavioral_passed: number;
  behavioral_total: number;
  adversarial_passed: number;
  adversarial_total: number;
  performance_passed: number;
  performance_total: number;
  changed_files: number;
  trit_files_changed: number;
  wall_clock_ms: number;
  command_runtime_ms: number;
  network_isolation_verified: boolean;
  resource_limits_verified: boolean;
  append_only_evidence_verified: boolean;
  participant_bundle_hash: string;
  submission_bundle_hash: string;
  grader_bundle_hash: string;
  evidence_hash: string;
}

export interface IntelligenceV31FrozenTask {
  task_id: string;
  participant_bundle_hash: string;
  grader_bundle_hash: string;
}

export interface IntelligenceV31FrozenSuiteManifest {
  schema: typeof INTELLIGENCE_V31_FROZEN_SUITE_SCHEMA;
  version: "3.1";
  frozen_at: string;
  subject_model_families: ["gpt-5.6-luna", "gpt-5.6-sol"];
  task_order: string[];
  tasks: IntelligenceV31FrozenTask[];
  protocol_sha256: string;
  suite_sha256: string;
}

interface CollectedFile {
  path: string;
  bytes: number;
  sha256: string;
}

interface CommandOutcome {
  id: string;
  visibility: "public" | "private";
  kind: "public" | IntelligenceV31SuiteKind;
  passed: boolean;
  command: SafeCommand;
  result: BoundedCommandResult;
}

function slash(value: string): string {
  return value.replace(/\\/g, "/");
}

function sha256(value: string | Uint8Array): string {
  return createHash("sha256").update(value).digest("hex");
}

function canonical(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(canonical);
  if (value && typeof value === "object") {
    return Object.fromEntries(Object.entries(value as Record<string, unknown>)
      .sort(([left], [right]) => left.localeCompare(right))
      .map(([key, item]) => [key, canonical(item)]));
  }
  return value;
}

function canonicalHash(value: unknown): string {
  return sha256(JSON.stringify(canonical(value)));
}

function inside(root: string, candidate: string): boolean {
  const relative = path.relative(path.resolve(root), path.resolve(candidate));
  return relative === "" || (relative !== ".." && !relative.startsWith(`..${path.sep}`) && !path.isAbsolute(relative));
}

function safeRelative(value: string, label: string): string {
  if (typeof value !== "string" || !value || path.isAbsolute(value)) throw new Error(`${label} must be a relative path`);
  const normalized = slash(value);
  if (normalized.split("/").some((segment) => !segment || segment === "." || segment === "..")) throw new Error(`${label} contains an unsafe segment`);
  return normalized;
}

function assertHash(value: string, label: string): void {
  if (!HASH_PATTERN.test(value)) throw new Error(`${label} must be a lowercase SHA-256 hash`);
}

async function writeOnceReadOnly(filePath: string, content: string): Promise<void> {
  await writeFile(filePath, content, { encoding: "utf8", flag: "wx", mode: 0o444 });
  try {
    await chmod(filePath, 0o444);
  } catch {
    // Windows read-only enforcement is weaker. The one-shot lock, exclusive
    // create, and content hash remain the application-level immutability gate.
  }
}

function assertExactKeys(value: Record<string, unknown>, allowed: readonly string[], label: string): void {
  const unexpected = Object.keys(value).filter((key) => !allowed.includes(key));
  if (unexpected.length > 0) throw new Error(`${label} contains unsupported fields: ${unexpected.join(", ")}`);
}

async function collectFiles(root: string): Promise<CollectedFile[]> {
  const files: CollectedFile[] = [];
  const visit = async (directory: string): Promise<void> => {
    const entries = await readdir(directory, { withFileTypes: true });
    entries.sort((left, right) => left.name.localeCompare(right.name));
    for (const entry of entries) {
      const absolute = path.join(directory, entry.name);
      const metadata = await lstat(absolute);
      const relative = slash(path.relative(root, absolute));
      if (metadata.isSymbolicLink()) throw new Error(`workspace contains a symbolic link: ${relative}`);
      if (entry.isDirectory()) await visit(absolute);
      else if (entry.isFile()) {
        const content = await readFile(absolute);
        files.push({ path: relative, bytes: content.byteLength, sha256: sha256(content) });
      } else throw new Error(`workspace contains an unsupported entry: ${relative}`);
    }
  };
  await visit(root);
  return files;
}

function bundleHash(files: CollectedFile[]): string {
  return sha256(files.map((file) => `${file.path}\0${file.bytes}\0${file.sha256}\n`).join(""));
}

async function copyTree(source: string, destination: string): Promise<void> {
  await mkdir(destination, { recursive: true });
  const entries = await readdir(source, { withFileTypes: true });
  for (const entry of entries) {
    const from = path.join(source, entry.name);
    const to = path.join(destination, entry.name);
    const metadata = await lstat(from);
    if (metadata.isSymbolicLink()) throw new Error(`cannot copy symbolic link ${entry.name}`);
    if (entry.isDirectory()) await copyTree(from, to);
    else if (entry.isFile()) {
      await mkdir(path.dirname(to), { recursive: true });
      await copyFile(from, to);
    }
  }
}

function assertPublicManifest(manifest: IntelligenceV31RepositoryTaskManifest): void {
  assertExactKeys(manifest as unknown as Record<string, unknown>, ["schema", "version", "task_id", "title", "phase", "initial_signal", "known_failing_command_id", "public_command_ids", "required_trit_change", "files", "limits"], "repository task manifest");
  if (manifest.schema !== INTELLIGENCE_V31_REPOSITORY_TASK_SCHEMA || manifest.version !== "3.1" || !/^TC-V31-[A-Z0-9-]+$/.test(manifest.task_id)) throw new Error("invalid v3.1 repository task manifest");
  if (!["diagnostic", "pilot", "calibration", "frozen", "official"].includes(manifest.phase)) throw new Error("repository task phase is invalid");
  if (!manifest.title || !manifest.initial_signal || manifest.required_trit_change !== true) throw new Error("repository task contract is incomplete");
  if (!SAFE_ID.test(manifest.known_failing_command_id) || !Array.isArray(manifest.public_command_ids) || manifest.public_command_ids.length === 0 || new Set(manifest.public_command_ids).size !== manifest.public_command_ids.length || manifest.public_command_ids.some((id) => !SAFE_ID.test(id))) throw new Error("repository task command ids are invalid");
  if (!Array.isArray(manifest.files) || manifest.files.length < 4 || new Set(manifest.files.map((file) => file.path)).size !== manifest.files.length) throw new Error("repository task must expose at least four distinct files");
  let editableTrit = 0;
  for (const file of manifest.files) {
    assertExactKeys(file as unknown as Record<string, unknown>, ["path", "editable", "sha256"], "participant file");
    file.path = safeRelative(file.path, "participant file");
    assertHash(file.sha256, `participant file ${file.path}`);
    if (file.editable && file.path.endsWith(".trit")) editableTrit += 1;
  }
  if (editableTrit === 0) throw new Error("repository task must contain an editable Trit file");
  const limits = manifest.limits;
  assertExactKeys(limits as unknown as Record<string, unknown>, ["wall_clock_ms", "cpu_ms", "memory_mb", "process_count", "output_bytes", "maximum_changed_files"], "repository task limits");
  if (![limits.wall_clock_ms, limits.cpu_ms, limits.memory_mb, limits.process_count, limits.output_bytes, limits.maximum_changed_files].every((value) => Number.isSafeInteger(value) && value > 0)) throw new Error("repository task limits must be positive safe integers");
  if (limits.wall_clock_ms > 1_200_000 || limits.cpu_ms > 1_200_000 || limits.memory_mb > 2048 || limits.process_count > 32 || limits.output_bytes > 8 * 1_048_576 || limits.maximum_changed_files > manifest.files.filter((file) => file.editable).length) throw new Error("repository task limits exceed protocol bounds");
}

function assertPrivateManifest(grader: IntelligenceV31RepositoryGraderManifest, taskId: string): void {
  assertExactKeys(grader as unknown as Record<string, unknown>, ["schema", "version", "task_id", "participant_manifest_sha256", "baseline_bundle_sha256", "suites"], "private grader manifest");
  if (grader.schema !== INTELLIGENCE_V31_REPOSITORY_GRADER_SCHEMA || grader.version !== "3.1" || grader.task_id !== taskId) throw new Error("invalid v3.1 private grader manifest");
  assertHash(grader.participant_manifest_sha256, "participant manifest hash");
  assertHash(grader.baseline_bundle_sha256, "baseline bundle hash");
  if (!Array.isArray(grader.suites) || grader.suites.length < 3 || new Set(grader.suites.map((suite) => suite.id)).size !== grader.suites.length) throw new Error("private grader must define distinct behavioral, adversarial, and performance suites");
  const kinds = new Set<IntelligenceV31SuiteKind>();
  for (const suite of grader.suites) {
    assertExactKeys(suite as unknown as Record<string, unknown>, ["id", "kind", "command_id", "required"], "private grader suite");
    if (!SAFE_ID.test(suite.id) || !SAFE_ID.test(suite.command_id) || suite.required !== true || !["behavioral", "adversarial", "performance"].includes(suite.kind)) throw new Error("private grader suite is invalid");
    kinds.add(suite.kind);
  }
  if (!["behavioral", "adversarial", "performance"].every((kind) => kinds.has(kind as IntelligenceV31SuiteKind))) throw new Error("private grader is missing a required suite kind");
}

function assertReplacementPatch(patch: IntelligenceV31RepositoryPatch, taskId: string, releasedBundleHash: string): void {
  assertExactKeys(patch as unknown as Record<string, unknown>, ["schema", "version", "task_id", "base_bundle_sha256", "files"], "repository replacement patch");
  if (patch.schema !== INTELLIGENCE_V31_REPOSITORY_PATCH_SCHEMA || patch.version !== "3.1" || patch.task_id !== taskId || patch.base_bundle_sha256 !== releasedBundleHash) throw new Error("replacement patch is not bound to the released task bundle");
  if (!Array.isArray(patch.files) || patch.files.length === 0 || new Set(patch.files.map((file) => file.path)).size !== patch.files.length) throw new Error("replacement patch must contain distinct file replacements");
  let totalBytes = 0;
  for (const file of patch.files) {
    assertExactKeys(file as unknown as Record<string, unknown>, ["path", "base_sha256", "replacement_utf8"], "replacement patch file");
    file.path = safeRelative(file.path, "replacement patch file");
    assertHash(file.base_sha256, `replacement patch base ${file.path}`);
    if (typeof file.replacement_utf8 !== "string" || file.replacement_utf8.includes("\0")) throw new Error(`replacement patch content is invalid: ${file.path}`);
    totalBytes += Buffer.byteLength(file.replacement_utf8, "utf8");
  }
  if (totalBytes > 16 * 1_048_576) throw new Error("replacement patch exceeds the 16 MiB artifact limit");
}

function commandLimits(manifest: IntelligenceV31RepositoryTaskManifest, remainingWallTimeMs: number): RunLimits {
  return normalizeRunLimits({
    // The task receives a 20-minute aggregate budget, while each individual
    // allowlisted subprocess remains under the runner's two-minute hard cap.
    wallTimeMs: Math.min(manifest.limits.wall_clock_ms, remainingWallTimeMs, 120_000),
    cpuMs: Math.min(manifest.limits.cpu_ms, 120_000),
    memoryMb: manifest.limits.memory_mb,
    processCount: manifest.limits.process_count,
    outputBytes: manifest.limits.output_bytes,
  });
}

async function runCommand(input: {
  commandId: string;
  taskId: string;
  kind: CommandOutcome["kind"];
  visibility: CommandOutcome["visibility"];
  workspaceRoot: string;
  repositoryRoot: string;
  registry: IntelligenceV31CommandRegistry;
  limits: RunLimits;
}): Promise<CommandOutcome> {
  if (!SAFE_ID.test(input.commandId)) throw new Error("unsafe command id");
  const context: IntelligenceV31CommandContext = { task_id: input.taskId, workspace_root: input.workspaceRoot, repository_root: input.repositoryRoot, visibility: input.visibility };
  const spec = input.registry.resolve(input.commandId, context);
  if (!spec) throw new Error(`command id is not allowlisted: ${input.commandId}`);
  if (!path.isAbsolute(spec.executable) || inside(input.workspaceRoot, spec.executable)) throw new Error(`allowlisted executable must be absolute and outside the participant workspace: ${input.commandId}`);
  if (!Array.isArray(spec.args) || spec.args.some((arg) => typeof arg !== "string" || arg.includes("\0"))) throw new Error(`allowlisted command contains invalid arguments: ${input.commandId}`);
  const relativeCwd = spec.cwd_relative ? safeRelative(spec.cwd_relative, "command cwd") : "";
  const cwd = relativeCwd ? path.resolve(input.workspaceRoot, relativeCwd) : input.workspaceRoot;
  if (!inside(input.workspaceRoot, cwd)) throw new Error("allowlisted command cwd escaped the workspace");
  const environment = {
    ...createSanitizedEnvironment(input.workspaceRoot, [path.join(input.repositoryRoot, "build")]).values,
    TREATCODE_NETWORK: "disabled",
    HTTP_PROXY: "http://127.0.0.1:9",
    HTTPS_PROXY: "http://127.0.0.1:9",
    ALL_PROXY: "http://127.0.0.1:9",
    NO_PROXY: "",
  };
  const command: SafeCommand = { executable: spec.executable, args: spec.args, cwd };
  const result = await spawnBounded(command, { env: environment, limits: input.limits });
  return { id: input.commandId, visibility: input.visibility, kind: input.kind, passed: result.code === 0 && !result.signal && !result.timedOut && !result.outputLimitExceeded && !result.spawnError, command: { executable: path.basename(spec.executable), args: [...spec.args], cwd: "<participant>" }, result };
}

export async function gradeIntelligenceV31RepositoryAttempt(options: IntelligenceV31RepositoryGradeOptions): Promise<IntelligenceV31RepositorySealedReport> {
  if (!/^TC-V31-[A-Z0-9-]+$/.test(options.task_id) || !SAFE_ID.test(options.attempt_id)) throw new Error("invalid task or attempt id");
  assertHash(options.released_bundle_hash, "released bundle hash");
  if (Boolean(options.submission_root) === Boolean(options.submission_patch_path)) throw new Error("provide exactly one snapshot or replacement-patch submission");
  const [repositoryRoot, subjectWorkspaceRoot, baselineRoot, graderRoot] = await Promise.all([
    realpath(options.repository_root), realpath(options.subject_workspace_root), realpath(options.baseline_root), realpath(options.grader_root),
  ]);
  const submissionRoot = options.submission_root ? await realpath(options.submission_root) : null;
  const submissionPatchPath = options.submission_patch_path ? await realpath(options.submission_patch_path) : null;
  if (!inside(subjectWorkspaceRoot, baselineRoot) || (submissionRoot && !inside(subjectWorkspaceRoot, submissionRoot)) || (submissionPatchPath && !inside(subjectWorkspaceRoot, submissionPatchPath))) throw new Error("baseline and submission must be inside the complete subject workspace");
  if (inside(subjectWorkspaceRoot, graderRoot) || inside(graderRoot, subjectWorkspaceRoot)) throw new Error("private grader must be outside the complete subject workspace");
  const evidenceRoot = path.resolve(options.evidence_root);
  if (inside(subjectWorkspaceRoot, evidenceRoot)) throw new Error("private evidence must be outside the complete subject workspace");
  await mkdir(evidenceRoot, { recursive: true });
  const lockPath = path.join(evidenceRoot, `${options.task_id}-${options.attempt_id}.lock.json`);
  await writeOnceReadOnly(lockPath, `${JSON.stringify({ schema: "treatcode.intelligence.repository-grade-lock.v3.1", task_id: options.task_id, attempt_id: options.attempt_id, created_at: new Date().toISOString(), retry_permitted: false }, null, 2)}\n`);

  const manifestText = await readFile(path.join(baselineRoot, MANIFEST_FILE), "utf8");
  const graderText = await readFile(path.join(graderRoot, GRADER_FILE), "utf8");
  const manifest = JSON.parse(manifestText) as IntelligenceV31RepositoryTaskManifest;
  const grader = JSON.parse(graderText) as IntelligenceV31RepositoryGraderManifest;
  assertPublicManifest(manifest);
  if (manifest.task_id !== options.task_id) throw new Error("participant manifest belongs to a different task");
  assertPrivateManifest(grader, options.task_id);
  const networkIsolation = options.network_isolation ?? { enforcement: "development_proxy_only" as const, provider: "sanitized-environment" };
  if (!SAFE_ID.test(networkIsolation.provider)) throw new Error("network isolation provider id is invalid");
  if (networkIsolation.enforcement === "verified_os_or_container") {
    if (!networkIsolation.evidence_sha256) throw new Error("verified network isolation requires evidence hash");
    assertHash(networkIsolation.evidence_sha256, "network isolation evidence hash");
  } else if (options.require_verified_infrastructure || ["frozen", "official"].includes(manifest.phase)) throw new Error("frozen and official grading requires verified OS or container network isolation");
  const resourceIsolation = options.resource_isolation ?? { enforcement: "development_wall_output_only" as const, provider: "bounded-child-process" };
  if (!SAFE_ID.test(resourceIsolation.provider)) throw new Error("resource isolation provider id is invalid");
  if (resourceIsolation.enforcement === "verified_os_or_container") {
    if (!resourceIsolation.evidence_sha256) throw new Error("verified resource isolation requires evidence hash");
    assertHash(resourceIsolation.evidence_sha256, "resource isolation evidence hash");
  } else if (options.require_verified_infrastructure || ["frozen", "official"].includes(manifest.phase)) throw new Error("frozen and official grading requires verified per-process CPU, memory, and process accounting");
  const evidenceStore = options.evidence_store ?? { enforcement: "development_hash_bound_files" as const, provider: "exclusive-create-hash" };
  if (!SAFE_ID.test(evidenceStore.provider)) throw new Error("evidence store provider id is invalid");
  if (evidenceStore.enforcement === "verified_append_only") {
    if (!evidenceStore.evidence_sha256) throw new Error("verified append-only evidence storage requires evidence hash");
    assertHash(evidenceStore.evidence_sha256, "append-only evidence store hash");
  } else if (options.require_verified_infrastructure || ["frozen", "official"].includes(manifest.phase)) throw new Error("frozen and official grading requires a verified append-only evidence store");
  if (sha256(manifestText) !== grader.participant_manifest_sha256) throw new Error("private grader is bound to a different participant manifest");

  const baselineFiles = await collectFiles(baselineRoot);
  const baselineBundleHash = bundleHash(baselineFiles);
  if (baselineBundleHash !== grader.baseline_bundle_sha256 || baselineBundleHash !== options.released_bundle_hash) throw new Error("released baseline bundle hash does not match the frozen grader contract");
  const baselineByPath = new Map(baselineFiles.map((file) => [file.path, file]));
  const declaredPaths = new Set([MANIFEST_FILE, ...manifest.files.map((file) => file.path)]);
  if (baselineFiles.some((file) => !declaredPaths.has(file.path)) || declaredPaths.size !== baselineFiles.length) throw new Error("baseline participant bundle differs from its declared file allowlist");
  for (const file of manifest.files) if (baselineByPath.get(file.path)?.sha256 !== file.sha256) throw new Error(`baseline file hash mismatch: ${file.path}`);

  let changed: IntelligenceV31ParticipantFile[];
  let submissionBundleHash: string;
  const patchReplacements = new Map<string, Uint8Array>();
  if (submissionRoot) {
    const submissionFiles = await collectFiles(submissionRoot);
    if (submissionFiles.some((file) => !declaredPaths.has(file.path)) || submissionFiles.length !== declaredPaths.size) throw new Error("submission contains missing or non-allowlisted files");
    const submissionByPath = new Map(submissionFiles.map((file) => [file.path, file]));
    if (submissionByPath.get(MANIFEST_FILE)?.sha256 !== sha256(manifestText)) throw new Error("submission changed the public task manifest");
    changed = manifest.files.filter((file) => submissionByPath.get(file.path)?.sha256 !== file.sha256);
    submissionBundleHash = bundleHash(submissionFiles);
  } else {
    const patchText = await readFile(submissionPatchPath!, "utf8");
    const patch = JSON.parse(patchText) as IntelligenceV31RepositoryPatch;
    assertReplacementPatch(patch, options.task_id, options.released_bundle_hash);
    for (const replacement of patch.files) {
      const declared = manifest.files.find((file) => file.path === replacement.path);
      if (!declared || !declared.editable) throw new Error(`replacement patch targeted a non-editable or unknown file: ${replacement.path}`);
      if (replacement.base_sha256 !== declared.sha256) throw new Error(`replacement patch base hash mismatch: ${replacement.path}`);
      patchReplacements.set(replacement.path, Buffer.from(replacement.replacement_utf8, "utf8"));
    }
    changed = manifest.files.filter((file) => {
      const replacement = patchReplacements.get(file.path);
      return replacement ? sha256(replacement) !== file.sha256 : false;
    });
    submissionBundleHash = sha256(patchText);
  }
  if (changed.length === 0) throw new Error("submission did not change any participant files");
  if (changed.length > manifest.limits.maximum_changed_files) throw new Error("submission exceeded the changed-file budget");
  if (changed.some((file) => !file.editable)) throw new Error("submission modified a read-only participant file");
  const tritChanged = changed.filter((file) => file.path.endsWith(".trit"));
  if (manifest.required_trit_change && tritChanged.length === 0) throw new Error("submission must change at least one editable Trit file");

  const temporaryRoot = await mkdtemp(path.join(os.tmpdir(), "treatcode-v31-grade-"));
  const workspaceRoot = path.join(temporaryRoot, "workspace");
  const startedAt = Date.now();
  const outcomes: CommandOutcome[] = [];
  try {
    await copyTree(baselineRoot, workspaceRoot);
    for (const file of changed) {
      const to = path.join(workspaceRoot, file.path);
      await mkdir(path.dirname(to), { recursive: true });
      if (submissionRoot) await copyFile(path.join(submissionRoot, file.path), to);
      else await writeFile(to, patchReplacements.get(file.path)!);
    }
    const runWithinAggregateBudget = async (commandId: string, kind: CommandOutcome["kind"], visibility: CommandOutcome["visibility"]): Promise<void> => {
      const remainingWallTimeMs = manifest.limits.wall_clock_ms - (Date.now() - startedAt);
      if (remainingWallTimeMs <= 0) throw new Error("repository attempt exhausted its aggregate wall-clock budget");
      outcomes.push(await runCommand({ commandId, taskId: options.task_id, kind, visibility, workspaceRoot, repositoryRoot, registry: options.command_registry, limits: commandLimits(manifest, remainingWallTimeMs) }));
    };
    for (const commandId of manifest.public_command_ids) await runWithinAggregateBudget(commandId, "public", "public");
    for (const suite of grader.suites) await runWithinAggregateBudget(suite.command_id, suite.kind, "private");
  } finally {
    await rm(temporaryRoot, { recursive: true, force: true });
  }
  const wallClockMs = Date.now() - startedAt;
  const publicOutcomes = outcomes.filter((item) => item.kind === "public");
  const privateOutcomes = outcomes.filter((item) => item.kind !== "public");
  const countKind = (kind: IntelligenceV31SuiteKind) => ({ passed: privateOutcomes.filter((item) => item.kind === kind && item.passed).length, total: privateOutcomes.filter((item) => item.kind === kind).length });
  const behavioral = countKind("behavioral");
  const adversarial = countKind("adversarial");
  const performance = countKind("performance");
  const graderFiles = await collectFiles(graderRoot);
  const privateRecord = {
    schema: "treatcode.intelligence.repository-grade-evidence.v3.1",
    task_id: options.task_id,
    attempt_id: options.attempt_id,
    network_disabled: networkIsolation.enforcement === "verified_os_or_container",
    network_isolation: networkIsolation,
    resource_isolation: resourceIsolation,
    evidence_store: evidenceStore,
    shell_disabled: true,
    retry_permitted: false,
    baseline_bundle_hash: baselineBundleHash,
    submission_kind: submissionRoot ? "snapshot" : "replacement_patch",
    submission_bundle_hash: submissionBundleHash,
    grader_bundle_hash: bundleHash(graderFiles),
    changed_files: changed.map((file) => file.path),
    outcomes,
    wall_clock_ms: wallClockMs,
    command_runtime_ms: outcomes.reduce((total, item) => total + item.result.durationMs, 0),
  };
  const evidenceHash = canonicalHash(privateRecord);
  const evidencePath = path.join(evidenceRoot, `${options.task_id}-${options.attempt_id}.private.json`);
  await writeOnceReadOnly(evidencePath, `${JSON.stringify({ ...privateRecord, evidence_hash: evidenceHash }, null, 2)}\n`);
  return {
    schema: INTELLIGENCE_V31_REPOSITORY_REPORT_SCHEMA,
    task_id: options.task_id,
    attempt_id: options.attempt_id,
    sealed: true,
    official: false,
    submission_kind: submissionRoot ? "snapshot" : "replacement_patch",
    passed: outcomes.every((item) => item.passed),
    public_commands_passed: publicOutcomes.filter((item) => item.passed).length,
    public_commands_total: publicOutcomes.length,
    required_suites_passed: privateOutcomes.filter((item) => item.passed).length,
    required_suites_total: privateOutcomes.length,
    behavioral_passed: behavioral.passed,
    behavioral_total: behavioral.total,
    adversarial_passed: adversarial.passed,
    adversarial_total: adversarial.total,
    performance_passed: performance.passed,
    performance_total: performance.total,
    changed_files: changed.length,
    trit_files_changed: tritChanged.length,
    wall_clock_ms: wallClockMs,
    command_runtime_ms: outcomes.reduce((total, item) => total + item.result.durationMs, 0),
    network_isolation_verified: networkIsolation.enforcement === "verified_os_or_container",
    resource_limits_verified: resourceIsolation.enforcement === "verified_os_or_container",
    append_only_evidence_verified: evidenceStore.enforcement === "verified_append_only",
    participant_bundle_hash: baselineBundleHash,
    submission_bundle_hash: submissionBundleHash,
    grader_bundle_hash: bundleHash(graderFiles),
    evidence_hash: evidenceHash,
  };
}

export function createIntelligenceV31FrozenSuite(input: {
  frozen_at: string;
  protocol_sha256: string;
  tasks: IntelligenceV31FrozenTask[];
}): IntelligenceV31FrozenSuiteManifest {
  assertHash(input.protocol_sha256, "protocol hash");
  if (!Number.isFinite(Date.parse(input.frozen_at))) throw new Error("frozen_at must be an ISO date-time");
  if (input.tasks.length !== 100 || new Set(input.tasks.map((task) => task.task_id)).size !== 100) throw new Error("frozen v3.1 suite must contain exactly 100 distinct tasks");
  for (const task of input.tasks) {
    if (!/^TC-V31-[A-Z0-9-]+$/.test(task.task_id)) throw new Error("frozen suite contains an invalid task id");
    assertHash(task.participant_bundle_hash, `${task.task_id} participant hash`);
    assertHash(task.grader_bundle_hash, `${task.task_id} grader hash`);
  }
  const base = {
    schema: INTELLIGENCE_V31_FROZEN_SUITE_SCHEMA,
    version: "3.1" as const,
    frozen_at: input.frozen_at,
    subject_model_families: ["gpt-5.6-luna", "gpt-5.6-sol"] as ["gpt-5.6-luna", "gpt-5.6-sol"],
    task_order: input.tasks.map((task) => task.task_id),
    tasks: input.tasks.map((task) => ({ ...task })),
    protocol_sha256: input.protocol_sha256,
  };
  return { ...base, suite_sha256: canonicalHash(base) };
}

export function verifyIntelligenceV31FrozenSuite(manifest: IntelligenceV31FrozenSuiteManifest): string[] {
  const issues: string[] = [];
  if (manifest.schema !== INTELLIGENCE_V31_FROZEN_SUITE_SCHEMA || manifest.version !== "3.1") issues.push("invalid frozen-suite schema");
  if (manifest.tasks.length !== 100 || manifest.task_order.length !== 100) issues.push("frozen suite must contain exactly 100 tasks");
  if (new Set(manifest.task_order).size !== manifest.task_order.length || manifest.task_order.some((taskId, index) => taskId !== manifest.tasks[index]?.task_id)) issues.push("task order does not match frozen task records");
  const { suite_sha256: ignored, ...base } = manifest;
  if (!ignored || canonicalHash(base) !== ignored) issues.push("frozen suite hash mismatch");
  return issues;
}
