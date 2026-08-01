import { createHash, randomUUID } from "node:crypto";
import { inflateRawSync } from "node:zlib";

export const CONTRIBUTION_SCHEMA_VERSION = "treatcode.contribution.v1";

export const CONTRIBUTION_ACTIONS = [
  "upload:create",
  "upload:read",
  "upload:write",
  "upload:finalize",
  "workspace:create",
  "workspace:read",
  "workspace:write",
  "validate:run",
  "evidence:attach",
  "draft-pr:approve",
  "branch:create",
  "commit:create",
  "push",
  "draft-pr:create",
  "audit:read",
  "merge",
] as const;

export type ContributionAction = (typeof CONTRIBUTION_ACTIONS)[number];
export type ContributionActorType = "human" | "agent" | "service";
export type UploadStatus = "open" | "accepted" | "rejected";
export type WorkspaceStatus = "isolated" | "validated" | "validation_failed" | "submitted";

export interface ContributionActor {
  actorId: string;
  actorType: ContributionActorType;
  scopes: readonly ContributionAction[];
  taskId?: string;
  projectId?: string;
  tokenId?: string;
  expiresAt?: string;
}

export interface UploadProvenance {
  author: string;
  source: "original" | "adapted" | "third_party";
  sourceUrl?: string;
  sourceCommit?: string;
  declaredAt?: string;
}

export interface CreateUploadInput {
  fileName: string;
  contentType?: string;
  totalBytes: number;
  expectedHash?: string;
  license?: string;
  taskId?: string;
  projectId?: string;
  provenance?: Partial<UploadProvenance> & { origin?: UploadProvenance["source"] };
}

export interface UploadRange {
  start: number;
  end: number;
}

export interface UploadSession {
  schema_version: typeof CONTRIBUTION_SCHEMA_VERSION;
  id: string;
  status: UploadStatus;
  ownerId: string;
  taskId: string;
  projectId?: string;
  fileName: string;
  contentType: string;
  totalBytes: number;
  expectedHash?: string;
  contentHash?: string;
  license: string;
  provenance: UploadProvenance;
  receivedBytes: number;
  contiguousBytes: number;
  receivedRanges: UploadRange[];
  quarantine: true;
  rejectionReasons: string[];
  createdAt: string;
  updatedAt: string;
  validatedAt?: string;
}

export interface ContributionFile {
  path: string;
  bytes: number;
  contentHash: string;
}

export interface WorkspaceFile extends ContributionFile {
  uploadId: string;
}

export interface ValidationCheck {
  id: string;
  passed: boolean;
  detail: string;
}

export interface WorkspaceValidation {
  schema_version: "treatcode.contribution.validation.v1";
  id: string;
  workspaceId: string;
  baseCommit: string;
  passed: boolean;
  checks: ValidationCheck[];
  inputHashes: string[];
  artifactHash: string;
  recordedAt: string;
}

export type EvidenceKind = "correctness" | "benchmark" | "review" | "diagnostic";

export interface EvidenceInput {
  kind: EvidenceKind;
  label: string;
  artifactHash?: string;
  payload?: string | Uint8Array;
  sourceCommit?: string;
  runnerImage?: string;
  command?: string;
}

export interface EvidenceRecord extends EvidenceInput {
  id: string;
  artifactHash: string;
  immutable: true;
  recordedAt: string;
}

export interface HumanApproval {
  id: string;
  reviewer: string;
  actorType: "human";
  decision: "approved" | "rejected";
  scope: "draft-pr";
  commit: string;
  recordedAt: string;
}

export interface ContributionWorkspace {
  schema_version: typeof CONTRIBUTION_SCHEMA_VERSION;
  id: string;
  repository: string;
  baseCommit: string;
  taskId: string;
  projectId?: string;
  ownerId: string;
  status: WorkspaceStatus;
  authoritativeSourceTouched: false;
  files: WorkspaceFile[];
  uploadIds: string[];
  validation?: WorkspaceValidation;
  evidence: EvidenceRecord[];
  approvals: HumanApproval[];
  draftPullRequest?: DraftPullRequest;
  createdAt: string;
  updatedAt: string;
}

export interface DraftPullRequest {
  id: string;
  repository: string;
  url: string;
  number: number;
  title: string;
  branch: string;
  commit: string;
  draft: true;
  evidenceHashes: string[];
  createdAt: string;
}

export interface ContributionAuditRecord {
  schema_version: "treatcode.contribution.audit.v1";
  id: string;
  recordedAt: string;
  actorId: string;
  actorType: ContributionActorType;
  action: string;
  outcome: "allowed" | "denied" | "rejected";
  resourceId: string;
  metadata: Record<string, unknown>;
  previousHash: string | null;
  recordHash: string;
}

export interface ContributionPolicy {
  maxUploadBytes: number;
  maxChunkBytes: number;
  maxArchiveEntries: number;
  maxArchiveUncompressedBytes: number;
  maxCompressionRatio: number;
  allowedLicenses: readonly string[];
  allowedExtensions: readonly string[];
  forbiddenExtensions: readonly string[];
  delegatedDraftPr: boolean;
}

export const DEFAULT_CONTRIBUTION_POLICY: ContributionPolicy = Object.freeze({
  maxUploadBytes: 8 * 1024 * 1024,
  maxChunkBytes: 256 * 1024,
  maxArchiveEntries: 256,
  maxArchiveUncompressedBytes: 16 * 1024 * 1024,
  maxCompressionRatio: 1000,
  allowedLicenses: ["MIT", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause", "ISC", "CC0-1.0", "MPL-2.0", "Unlicense"],
  allowedExtensions: [
    ".trit",
    ".tasm",
    ".h",
    ".hh",
    ".hpp",
    ".c",
    ".cc",
    ".cpp",
    ".md",
    ".markdown",
    ".txt",
    ".json",
    ".toml",
    ".yaml",
    ".yml",
    ".ts",
    ".tsx",
    ".js",
    ".mjs",
    ".css",
    ".html",
    ".svg",
    ".zip",
  ],
  forbiddenExtensions: [
    ".exe",
    ".dll",
    ".so",
    ".dylib",
    ".bin",
    ".img",
    ".iso",
    ".msi",
    ".com",
    ".scr",
    ".bat",
    ".cmd",
    ".ps1",
    ".sh",
    ".bash",
    ".zsh",
    ".jar",
    ".class",
    ".pyc",
    ".pyo",
  ],
  delegatedDraftPr: false,
});

export class ContributionError extends Error {
  constructor(
    public readonly code: string,
    message: string,
    public readonly status = 400,
    public readonly details: Record<string, unknown> = {},
  ) {
    super(message);
    this.name = "ContributionError";
  }
}

export interface GitHubBranchRequest {
  repository: string;
  branch: string;
  baseCommit: string;
}

export interface GitHubCommitRequest {
  repository: string;
  branch: string;
  baseCommit: string;
  message: string;
  files: Array<WorkspaceFile & { contentBase64?: string }>;
  evidence: EvidenceRecord[];
}

export interface GitHubPullRequestRequest {
  repository: string;
  branch: string;
  baseBranch?: string;
  title: string;
  body: string;
  commit: string;
  draft: true;
}

export interface GitHubAppAdapter {
  createBranch(request: GitHubBranchRequest): Promise<{ ref: string }>;
  createCommit(request: GitHubCommitRequest): Promise<{ sha: string }>;
  pushBranch(request: { repository: string; branch: string; commit: string }): Promise<void>;
  createDraftPullRequest(request: GitHubPullRequestRequest): Promise<{ number: number; url: string }>;
}

export interface GitHubAppTokenProvider {
  installationToken(repository: string): Promise<string>;
}

export interface GitHubAppClientOptions {
  tokenProvider: GitHubAppTokenProvider;
  apiBaseUrl?: string;
  fetchImpl?: typeof fetch;
}

export class InMemoryGitHubApp implements GitHubAppAdapter {
  readonly operations: Array<Record<string, unknown>> = [];
  readonly branches = new Map<string, { baseCommit: string; commit?: string }>();
  readonly commits = new Map<string, GitHubCommitRequest>();
  readonly pullRequests: DraftPullRequest[] = [];

  async createBranch(request: GitHubBranchRequest): Promise<{ ref: string }> {
    const key = `${request.repository}#${request.branch}`;
    if (this.branches.has(key)) throw new ContributionError("GITHUB_BRANCH_EXISTS", `Branch ${request.branch} already exists.`, 409);
    this.branches.set(key, { baseCommit: request.baseCommit });
    this.operations.push({ action: "branch:create", repository: request.repository, branch: request.branch, baseCommit: request.baseCommit });
    return { ref: `refs/heads/${request.branch}` };
  }

  async createCommit(request: GitHubCommitRequest): Promise<{ sha: string }> {
    const branchKey = `${request.repository}#${request.branch}`;
    if (!this.branches.has(branchKey)) throw new ContributionError("GITHUB_BRANCH_MISSING", `Branch ${request.branch} does not exist.`, 409);
    const payload = {
      repository: request.repository,
      branch: request.branch,
      baseCommit: request.baseCommit,
      message: request.message,
      files: request.files.map((file) => ({ path: file.path, contentHash: file.contentHash, bytes: file.bytes })),
      evidence: request.evidence.map((item) => ({ id: item.id, kind: item.kind, artifactHash: item.artifactHash })),
    };
    const sha = sha256Text(canonicalJson(payload));
    this.commits.set(sha, request);
    this.operations.push({ action: "commit:create", repository: request.repository, branch: request.branch, sha });
    return { sha };
  }

  async pushBranch(request: { repository: string; branch: string; commit: string }): Promise<void> {
    const branchKey = `${request.repository}#${request.branch}`;
    const branch = this.branches.get(branchKey);
    if (!branch) throw new ContributionError("GITHUB_BRANCH_MISSING", `Branch ${request.branch} does not exist.`, 409);
    if (!this.commits.has(request.commit)) throw new ContributionError("GITHUB_COMMIT_MISSING", `Commit ${request.commit} does not exist.`, 409);
    branch.commit = request.commit;
    this.operations.push({ action: "push", repository: request.repository, branch: request.branch, commit: request.commit });
  }

  async createDraftPullRequest(request: GitHubPullRequestRequest): Promise<{ number: number; url: string }> {
    if (!request.draft) throw new ContributionError("DRAFT_REQUIRED", "Contribution pull requests must be drafts.", 400);
    const number = this.pullRequests.length + 1;
    const repositoryLabel = request.repository.replace(/^https?:\/\//i, "");
    const url = `https://github.example.test/${repositoryLabel}/pull/${number}`;
    this.pullRequests.push({
      id: `pr_${number}`,
      repository: request.repository,
      url,
      number,
      title: request.title,
      branch: request.branch,
      commit: request.commit,
      draft: true,
      evidenceHashes: extractEvidenceHashes(request.body),
      createdAt: new Date(0).toISOString(),
    });
    this.operations.push({ action: "draft-pr:create", repository: request.repository, branch: request.branch, commit: request.commit, number });
    return { number, url };
  }

  getState() {
    return {
      operations: this.operations.map((operation) => ({ ...operation })),
      branches: [...this.branches.entries()].map(([name, value]) => ({ name, ...value })),
      pullRequests: this.pullRequests.map((pullRequest) => ({ ...pullRequest })),
    };
  }
}

/**
 * Production-shaped GitHub App adapter. The token provider is intentionally
 * injected at the application boundary; upload and execution workers never
 * receive a token or a GitHub client. Tests use InMemoryGitHubApp instead.
 */
export class GitHubAppClient implements GitHubAppAdapter {
  private readonly tokenProvider: GitHubAppTokenProvider;
  private readonly apiBaseUrl: string;
  private readonly fetchImpl: typeof fetch;

  constructor(options: GitHubAppClientOptions) {
    this.tokenProvider = options.tokenProvider;
    this.apiBaseUrl = (options.apiBaseUrl || "https://api.github.com").replace(/\/$/, "");
    this.fetchImpl = options.fetchImpl || fetch;
  }

  private repositoryPath(repository: string): string {
    const normalized = repository.replace(/^https?:\/\/github\.com\//i, "").replace(/^github\.com\//i, "").replace(/\/$/, "");
    if (!/^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(normalized)) throw new ContributionError("INVALID_REPOSITORY", "GitHub App operations require an owner/name repository.", 422);
    return normalized;
  }

  private async request<T>(repository: string, method: string, endpoint: string, body?: Record<string, unknown>): Promise<T> {
    const token = await this.tokenProvider.installationToken(repository);
    if (!token) throw new ContributionError("GITHUB_APP_TOKEN_UNAVAILABLE", "A GitHub App installation token is unavailable.", 503);
    const response = await this.fetchImpl(`${this.apiBaseUrl}${endpoint}`, {
      method,
      headers: {
        Accept: "application/vnd.github+json",
        "Content-Type": "application/json",
        Authorization: `Bearer ${token}`,
        "X-GitHub-Api-Version": "2022-11-28",
      },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    if (!response.ok) {
      const message = await response.text().catch(() => "");
      throw new ContributionError("GITHUB_API_ERROR", `GitHub App request failed with status ${response.status}.`, response.status >= 500 ? 502 : response.status, { method, endpoint, response: message.slice(0, 240) });
    }
    return response.json() as Promise<T>;
  }

  async createBranch(request: GitHubBranchRequest): Promise<{ ref: string }> {
    const repository = this.repositoryPath(request.repository);
    const response = await this.request<{ ref: string }>(request.repository, "POST", `/repos/${repository}/git/refs`, { ref: `refs/heads/${request.branch}`, sha: request.baseCommit });
    return { ref: response.ref || `refs/heads/${request.branch}` };
  }

  async createCommit(request: GitHubCommitRequest): Promise<{ sha: string }> {
    const repository = this.repositoryPath(request.repository);
    if (request.files.some((file) => !file.contentBase64)) throw new ContributionError("COMMIT_CONTENT_REQUIRED", "The GitHub App adapter requires file content for blob creation.", 422);
    const base = await this.request<{ tree?: { sha?: string } }>(request.repository, "GET", `/repos/${repository}/git/commits/${request.baseCommit}`);
    const baseTree = base.tree?.sha;
    if (!baseTree) throw new ContributionError("GITHUB_BASE_TREE_MISSING", "GitHub did not return the base tree for the contribution commit.", 502);
    const blobs = [];
    for (const file of request.files) {
      const blob = await this.request<{ sha: string }>(request.repository, "POST", `/repos/${repository}/git/blobs`, { content: file.contentBase64, encoding: "base64" });
      blobs.push({ path: file.path, mode: "100644", type: "blob", sha: blob.sha });
    }
    const tree = await this.request<{ sha: string }>(request.repository, "POST", `/repos/${repository}/git/trees`, { base_tree: baseTree, tree: blobs });
    const commit = await this.request<{ sha: string }>(request.repository, "POST", `/repos/${repository}/git/commits`, { message: request.message, tree: tree.sha, parents: [request.baseCommit] });
    return { sha: commit.sha };
  }

  async pushBranch(request: { repository: string; branch: string; commit: string }): Promise<void> {
    const repository = this.repositoryPath(request.repository);
    await this.request(request.repository, "PATCH", `/repos/${repository}/git/refs/heads/${encodeURIComponent(request.branch)}`, { sha: request.commit, force: false });
  }

  async createDraftPullRequest(request: GitHubPullRequestRequest): Promise<{ number: number; url: string }> {
    const repository = this.repositoryPath(request.repository);
    const response = await this.request<{ number: number; html_url: string }>(request.repository, "POST", `/repos/${repository}/pulls`, { title: request.title, head: request.branch, base: request.baseBranch || "main", body: request.body, draft: true });
    return { number: response.number, url: response.html_url };
  }
}

interface QuarantineFile extends ContributionFile {
  bytesValue: Uint8Array;
}

interface UploadRecord {
  session: UploadSession;
  chunks: Map<number, Uint8Array>;
  files?: QuarantineFile[];
  bytesValue?: Uint8Array;
}

interface WorkspaceRecord {
  workspace: ContributionWorkspace;
  files: Map<string, WorkspaceFile & { bytesValue: Uint8Array }>;
}

export interface ContributionServiceOptions {
  policy?: Partial<ContributionPolicy>;
  github?: GitHubAppAdapter;
  now?: () => Date;
}

function asBytes(value: Uint8Array | ArrayBuffer): Uint8Array {
  if (value instanceof Uint8Array) return new Uint8Array(value);
  return new Uint8Array(value);
}

function textBytes(value: string): Uint8Array {
  return new TextEncoder().encode(value);
}

function sha256(value: Uint8Array): string {
  return `sha256:${createHash("sha256").update(Buffer.from(value)).digest("hex")}`;
}

function sha256Text(value: string): string {
  return sha256(textBytes(value));
}

function stableValue(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(stableValue);
  if (value && typeof value === "object") {
    return Object.fromEntries(Object.entries(value as Record<string, unknown>).sort(([a], [b]) => a.localeCompare(b)).map(([key, item]) => [key, stableValue(item)]));
  }
  return value;
}

function canonicalJson(value: unknown): string {
  return JSON.stringify(stableValue(value));
}

function normalizeHash(value: string | undefined, required = false): string | undefined {
  if (!value) {
    if (required) throw new ContributionError("HASH_REQUIRED", "A SHA-256 content hash is required.", 422);
    return undefined;
  }
  const normalized = value.startsWith("sha256:") ? value : `sha256:${value}`;
  if (!/^sha256:[0-9a-f]{64}$/i.test(normalized)) throw new ContributionError("INVALID_HASH", "Hashes must be SHA-256 values.", 422);
  return normalized.toLowerCase();
}

function nowIso(now: () => Date): string {
  return now().toISOString();
}

function decodePathCandidate(value: string): string {
  let current = value;
  for (let index = 0; index < 3; index += 1) {
    try {
      const decoded = decodeURIComponent(current);
      if (decoded === current) break;
      current = decoded;
    } catch {
      throw new ContributionError("INVALID_PATH", "A path contains invalid URL encoding.", 422);
    }
  }
  return current;
}

function safeRelativePath(value: string, allowDirectory = false): string {
  const raw = decodePathCandidate(String(value || ""));
  const normalized = raw.replace(/\\/g, "/");
  if (!normalized || normalized.includes("\0") || /^[A-Za-z]:/.test(normalized) || normalized.startsWith("/")) {
    throw new ContributionError("PATH_TRAVERSAL", `Unsafe path: ${value}`, 422);
  }
  const parts = normalized.split("/");
  if (parts.some((part) => part === ".." || part === "" && !allowDirectory)) {
    throw new ContributionError("PATH_TRAVERSAL", `Unsafe path: ${value}`, 422);
  }
  if (!allowDirectory && parts.some((part) => part === ".")) {
    throw new ContributionError("PATH_TRAVERSAL", `Unsafe path: ${value}`, 422);
  }
  const result = parts.filter((part) => part !== ".").join("/");
  if (!result || (!allowDirectory && result.endsWith("/"))) {
    throw new ContributionError("INVALID_PATH", `A file path is required: ${value}`, 422);
  }
  return result;
}

function safeWorkspacePrefix(value: string | undefined): string {
  if (!value || value === ".") return "";
  const normalized = safeRelativePath(value, true).replace(/\/+$/, "");
  return normalized === "." ? "" : normalized;
}

function extensionOf(fileName: string): string {
  const lower = fileName.toLowerCase();
  if (lower.endsWith(".tar.gz")) return ".tar.gz";
  const dot = lower.lastIndexOf(".");
  return dot === -1 ? "" : lower.slice(dot);
}

function validateRepository(repository: string): string {
  try {
    const url = new URL(repository);
    if (url.protocol !== "https:" || !url.hostname || url.username || url.password) throw new Error("invalid");
    return `${url.protocol}//${url.hostname}${url.pathname}`.replace(/\/$/, "");
  } catch {
    throw new ContributionError("INVALID_REPOSITORY", "Repository must be an HTTPS URL without embedded credentials.", 422);
  }
}

function validateCommit(value: string): string {
  if (!/^[0-9a-f]{7,64}$/i.test(value)) throw new ContributionError("INVALID_COMMIT", "A repository commit must be a hexadecimal id.", 422);
  return value.toLowerCase();
}

function validateBranch(value: string): string {
  const branch = String(value || "").trim();
  if (!branch || branch.length > 200 || branch.startsWith("/") || branch.endsWith("/") || branch.includes("..") || /[\\~^:?*\[\]\x00-\x20]/.test(branch)) {
    throw new ContributionError("INVALID_BRANCH", "Branch names must be safe Git refs.", 422);
  }
  return branch;
}

function validateProvenance(value: CreateUploadInput["provenance"]): UploadProvenance {
  if (!value || typeof value !== "object") throw new ContributionError("PROVENANCE_REQUIRED", "Upload provenance is required.", 422);
  const author = String(value.author || "").trim();
  const source = (value.source || value.origin) as UploadProvenance["source"] | undefined;
  if (!author || !source || !["original", "adapted", "third_party"].includes(source)) {
    throw new ContributionError("PROVENANCE_INCOMPLETE", "Provenance must include an author and an allowed source classification.", 422);
  }
  const sourceUrl = value.sourceUrl ? String(value.sourceUrl).trim() : undefined;
  if (source !== "original" && !sourceUrl) {
    throw new ContributionError("PROVENANCE_INCOMPLETE", "Adapted or third-party uploads require a source URL.", 422);
  }
  if (sourceUrl) {
    try {
      const url = new URL(sourceUrl);
      if (!/^https?:$/.test(url.protocol)) throw new Error("invalid");
    } catch {
      throw new ContributionError("INVALID_PROVENANCE_URL", "Provenance sourceUrl must be an HTTP(S) URL.", 422);
    }
  }
  const sourceCommit = value.sourceCommit ? validateCommit(String(value.sourceCommit)) : undefined;
  return { author, source, sourceUrl, sourceCommit, declaredAt: value.declaredAt || new Date(0).toISOString() };
}

function validateLicense(value: string | undefined, provenance: CreateUploadInput["provenance"], policy: ContributionPolicy): string {
  const license = String(value || (provenance as Record<string, unknown> | undefined)?.license || "").trim();
  if (!license) throw new ContributionError("LICENSE_REQUIRED", "An SPDX license declaration is required.", 422);
  if (!policy.allowedLicenses.includes(license)) throw new ContributionError("LICENSE_UNAUTHORIZED", `License ${license} is not allowed by project policy.`, 422);
  return license;
}

function rangeList(chunks: Map<number, Uint8Array>): UploadRange[] {
  const ranges = [...chunks.entries()].map(([start, bytes]) => ({ start, end: start + bytes.byteLength })).sort((a, b) => a.start - b.start);
  const merged: UploadRange[] = [];
  for (const range of ranges) {
    const previous = merged[merged.length - 1];
    if (previous && range.start <= previous.end) previous.end = Math.max(previous.end, range.end);
    else merged.push({ ...range });
  }
  return merged;
}

function contiguousBytes(ranges: UploadRange[]): number {
  return ranges.length && ranges[0].start === 0 ? ranges[0].end : 0;
}

function readU16(bytes: Uint8Array, offset: number): number {
  return bytes[offset] | (bytes[offset + 1] << 8);
}

function readU32(bytes: Uint8Array, offset: number): number {
  return (bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16) | (bytes[offset + 3] * 0x1000000)) >>> 0;
}

function signatureAt(bytes: Uint8Array, offset: number, signature: number): boolean {
  return offset + 4 <= bytes.length && readU32(bytes, offset) === signature;
}

function hasMagic(bytes: Uint8Array, magic: number[]): boolean {
  return magic.every((value, index) => bytes[index] === value);
}

function unsafeContentReason(fileName: string, bytes: Uint8Array): string | undefined {
  if (hasMagic(bytes, [0x4d, 0x5a])) return "executable_pe_signature";
  if (hasMagic(bytes, [0x7f, 0x45, 0x4c, 0x46])) return "executable_elf_signature";
  if (hasMagic(bytes, [0xcf, 0xfa, 0xed, 0xfe]) || hasMagic(bytes, [0xfe, 0xed, 0xfa, 0xcf])) return "executable_macho_signature";
  if (bytes.some((value) => value === 0)) return "binary_nul_content";
  const prefix = new TextDecoder().decode(bytes.slice(0, 256));
  if (/^#!\s*\/.*\b(?:sh|bash|zsh|fish|powershell|pwsh)\b/m.test(prefix)) return "executable_script_shebang";
  if (/^#!\s*.*\b(?:cmd|command\.com)\b/m.test(prefix)) return "executable_script_shebang";
  if (extensionOf(fileName) === ".zip") return "nested_archive";
  return undefined;
}

function validateAllowedFileName(fileName: string, policy: ContributionPolicy): string {
  const safeName = safeRelativePath(fileName);
  const extension = extensionOf(safeName);
  if (policy.forbiddenExtensions.includes(extension)) throw new ContributionError("FORBIDDEN_TYPE", `File type ${extension || "(none)"} is not accepted.`, 422);
  if (!policy.allowedExtensions.includes(extension)) throw new ContributionError("FORBIDDEN_TYPE", `File type ${extension || "(none)"} is not accepted.`, 422);
  return safeName;
}

function parseZip(bytes: Uint8Array, policy: ContributionPolicy): QuarantineFile[] {
  if (!signatureAt(bytes, 0, 0x04034b50)) throw new ContributionError("INVALID_ARCHIVE", "Only a valid ZIP archive may use the .zip upload type.", 422);
  const files: QuarantineFile[] = [];
  const names = new Set<string>();
  let offset = 0;
  let uncompressedTotal = 0;
  let centralOffset = -1;
  while (offset + 4 <= bytes.length) {
    if (signatureAt(bytes, offset, 0x04034b50)) {
      if (offset + 30 > bytes.length) throw new ContributionError("INVALID_ARCHIVE", "ZIP local header is truncated.", 422);
      const flags = readU16(bytes, offset + 6);
      const method = readU16(bytes, offset + 8);
      const compressedSize = readU32(bytes, offset + 18);
      const uncompressedSize = readU32(bytes, offset + 22);
      const nameLength = readU16(bytes, offset + 26);
      const extraLength = readU16(bytes, offset + 28);
      if ((flags & 0x0001) !== 0 || (flags & 0x0008) !== 0) throw new ContributionError("UNSAFE_ARCHIVE", "Encrypted and data-descriptor ZIP entries are not accepted.", 422);
      if (compressedSize === 0xffffffff || uncompressedSize === 0xffffffff) throw new ContributionError("ARCHIVE_LIMIT", "ZIP64 entries are not accepted by the quarantine scanner.", 422);
      const nameStart = offset + 30;
      const dataStart = nameStart + nameLength + extraLength;
      const dataEnd = dataStart + compressedSize;
      if (dataStart > bytes.length || dataEnd > bytes.length) throw new ContributionError("INVALID_ARCHIVE", "ZIP entry data is truncated.", 422);
      const name = new TextDecoder().decode(bytes.slice(nameStart, nameStart + nameLength));
      const isDirectory = name.endsWith("/");
      const safeName = safeRelativePath(name, isDirectory);
      if (names.has(safeName)) throw new ContributionError("DUPLICATE_PATH", `Archive contains duplicate path ${safeName}.`, 422);
      names.add(safeName);
      if (files.length >= policy.maxArchiveEntries) throw new ContributionError("ARCHIVE_LIMIT", "Archive contains too many entries.", 422);
      if (uncompressedSize > policy.maxArchiveUncompressedBytes || uncompressedTotal + uncompressedSize > policy.maxArchiveUncompressedBytes) throw new ContributionError("ARCHIVE_LIMIT", "Archive expands beyond the quarantine limit.", 422);
      if (!isDirectory) {
        if (method !== 0 && method !== 8) throw new ContributionError("UNSAFE_ARCHIVE", `ZIP compression method ${method} is not accepted.`, 422);
        const compressed = bytes.slice(dataStart, dataEnd);
        if (uncompressedSize / Math.max(1, compressedSize) > policy.maxCompressionRatio) throw new ContributionError("ARCHIVE_BOMB", `Archive entry ${safeName} exceeds the compression-ratio limit.`, 422);
        let expanded: Uint8Array;
        try {
          expanded = method === 0 ? new Uint8Array(compressed) : new Uint8Array(inflateRawSync(Buffer.from(compressed)));
        } catch {
          throw new ContributionError("INVALID_ARCHIVE", `Archive entry ${safeName} could not be decompressed.`, 422);
        }
        if (expanded.byteLength !== uncompressedSize) throw new ContributionError("INVALID_ARCHIVE", `Archive entry ${safeName} has inconsistent size metadata.`, 422);
        const type = extensionOf(safeName);
        if (policy.forbiddenExtensions.includes(type) || !policy.allowedExtensions.includes(type) || type === ".zip") throw new ContributionError("FORBIDDEN_TYPE", `Archive entry ${safeName} has a forbidden type.`, 422);
        const unsafe = unsafeContentReason(safeName, expanded);
        if (unsafe) throw new ContributionError("UNSAFE_CONTENT", `Archive entry ${safeName} failed quarantine scanning: ${unsafe}.`, 422);
        const contentHash = sha256(expanded);
        files.push({ path: safeName, bytes: expanded.byteLength, contentHash, bytesValue: expanded });
        uncompressedTotal += expanded.byteLength;
      }
      offset = dataEnd;
      continue;
    }
    if (signatureAt(bytes, offset, 0x02014b50)) {
      centralOffset = offset;
      break;
    }
    if (signatureAt(bytes, offset, 0x06054b50) || signatureAt(bytes, offset, 0x06064b50)) break;
    throw new ContributionError("INVALID_ARCHIVE", "ZIP contains an unrecognized record.", 422);
  }
  if (centralOffset < 0 || files.length === 0) throw new ContributionError("INVALID_ARCHIVE", "ZIP archive has no safe file entries.", 422);

  // Central-directory metadata is checked for symlink entries. Local headers
  // do not carry the Unix mode bits used to represent symlinks.
  let central = centralOffset;
  while (signatureAt(bytes, central, 0x02014b50)) {
    if (central + 46 > bytes.length) throw new ContributionError("INVALID_ARCHIVE", "ZIP central directory is truncated.", 422);
    const nameLength = readU16(bytes, central + 28);
    const extraLength = readU16(bytes, central + 30);
    const commentLength = readU16(bytes, central + 32);
    const externalAttributes = readU32(bytes, central + 38);
    const name = new TextDecoder().decode(bytes.slice(central + 46, central + 46 + nameLength));
    const unixMode = externalAttributes >>> 16;
    if ((unixMode & 0xf000) === 0xa000) throw new ContributionError("UNSAFE_ARCHIVE", `Archive symlink ${name} is not accepted.`, 422);
    central += 46 + nameLength + extraLength + commentLength;
  }
  return files;
}

function scanSingleFile(fileName: string, bytes: Uint8Array, policy: ContributionPolicy): QuarantineFile[] {
  const safeName = validateAllowedFileName(fileName, policy);
  const unsafe = unsafeContentReason(safeName, bytes);
  if (unsafe) throw new ContributionError("UNSAFE_CONTENT", `File failed quarantine scanning: ${unsafe}.`, 422);
  return [{ path: safeName, bytes: bytes.byteLength, contentHash: sha256(bytes), bytesValue: new Uint8Array(bytes) }];
}

function publicWorkspace(record: WorkspaceRecord): ContributionWorkspace {
  return {
    ...record.workspace,
    files: [...record.files.values()].sort((a, b) => a.path.localeCompare(b.path)).map((file) => ({ path: file.path, bytes: file.bytes, contentHash: file.contentHash, uploadId: file.uploadId })),
    uploadIds: [...record.workspace.uploadIds],
    evidence: record.workspace.evidence.map((item) => ({ ...item })),
    approvals: record.workspace.approvals.map((item) => ({ ...item })),
  };
}

function publicUpload(record: UploadRecord): UploadSession {
  return { ...record.session, receivedRanges: record.session.receivedRanges.map((range) => ({ ...range })), rejectionReasons: [...record.session.rejectionReasons], provenance: { ...record.session.provenance } };
}

function extractEvidenceHashes(body: string): string[] {
  return [...body.matchAll(/artifactHash=([^\s]+)/g)].map((match) => match[1]);
}

export class ContributionService {
  readonly policy: ContributionPolicy;
  readonly github: GitHubAppAdapter;
  private readonly uploads = new Map<string, UploadRecord>();
  private readonly workspaces = new Map<string, WorkspaceRecord>();
  private readonly auditRecords: ContributionAuditRecord[] = [];
  private readonly now: () => Date;

  constructor(options: ContributionServiceOptions = {}) {
    this.policy = {
      ...DEFAULT_CONTRIBUTION_POLICY,
      ...options.policy,
      allowedLicenses: options.policy?.allowedLicenses || DEFAULT_CONTRIBUTION_POLICY.allowedLicenses,
      allowedExtensions: options.policy?.allowedExtensions || DEFAULT_CONTRIBUTION_POLICY.allowedExtensions,
      forbiddenExtensions: options.policy?.forbiddenExtensions || DEFAULT_CONTRIBUTION_POLICY.forbiddenExtensions,
    };
    this.github = options.github || new InMemoryGitHubApp();
    this.now = options.now || (() => new Date());
  }

  capabilities() {
    return {
      schema_version: CONTRIBUTION_SCHEMA_VERSION,
      upload: {
        resumable: true,
        content_hash: "sha256",
        quarantine: true,
        max_upload_bytes: this.policy.maxUploadBytes,
        max_chunk_bytes: this.policy.maxChunkBytes,
      },
      actions: [...CONTRIBUTION_ACTIONS],
      github: { integration: "github-app", draft_pull_requests: true, merge_authority: false },
      policy: {
        allowed_licenses: [...this.policy.allowedLicenses],
        allowed_extensions: [...this.policy.allowedExtensions],
        forbidden_extensions: [...this.policy.forbiddenExtensions],
        delegated_draft_pr: this.policy.delegatedDraftPr,
      },
    };
  }

  private audit(actor: ContributionActor, action: string, outcome: ContributionAuditRecord["outcome"], resourceId: string, metadata: Record<string, unknown> = {}): ContributionAuditRecord {
    const previousHash = this.auditRecords.length ? this.auditRecords[this.auditRecords.length - 1].recordHash : null;
    const base = { schema_version: "treatcode.contribution.audit.v1" as const, id: `audit_${randomUUID()}`, recordedAt: nowIso(this.now), actorId: actor.actorId, actorType: actor.actorType, action, outcome, resourceId, metadata, previousHash };
    const recordHash = sha256Text(canonicalJson(base));
    const record = { ...base, recordHash };
    this.auditRecords.push(record);
    return record;
  }

  private authorize(actor: ContributionActor, action: ContributionAction, resourceId: string): void {
    const expired = actor.expiresAt ? Number.isNaN(Date.parse(actor.expiresAt)) || Date.parse(actor.expiresAt) <= this.now().getTime() : false;
    if (expired) {
      this.audit(actor, action, "denied", resourceId, { reason: "expired_token" });
      throw new ContributionError("TOKEN_EXPIRED", "The task-scoped credential has expired.", 401);
    }
    if (!actor.actorId || !actor.scopes.includes(action)) {
      this.audit(actor, action, "denied", resourceId, { reason: "missing_scope", required: action });
      throw new ContributionError("FORBIDDEN_SCOPE", `Action ${action} is not allowed for this credential.`, 403, { required: action });
    }
  }

  private requireUpload(actor: ContributionActor, uploadId: string, action: ContributionAction): UploadRecord {
    this.authorize(actor, action, uploadId);
    const record = this.uploads.get(uploadId);
    if (!record) throw new ContributionError("UPLOAD_NOT_FOUND", `Upload ${uploadId} was not found.`, 404);
    this.checkResourceAccess(actor, record.session.ownerId, record.session.taskId, record.session.projectId, uploadId);
    return record;
  }

  private checkResourceAccess(actor: ContributionActor, ownerId: string, taskId: string, projectId: string | undefined, resourceId: string): void {
    const sameOwner = actor.actorId === ownerId;
    const sameTask = !actor.taskId || actor.taskId === taskId;
    const sameProject = !actor.projectId || !projectId || actor.projectId === projectId;
    const collaborator = actor.actorType === "service" || (sameTask && sameProject);
    if ((!sameOwner && !collaborator) || !sameTask || !sameProject) {
      this.audit(actor, "resource:read", "denied", resourceId, { reason: "task_or_project_scope" });
      throw new ContributionError("RESOURCE_SCOPE_MISMATCH", "The credential is not bound to this contribution task or project.", 403);
    }
  }

  createUploadSession(actor: ContributionActor, input: CreateUploadInput): UploadSession {
    const id = `upl_${randomUUID()}`;
    this.authorize(actor, "upload:create", id);
    if (!Number.isSafeInteger(input.totalBytes) || input.totalBytes <= 0 || input.totalBytes > this.policy.maxUploadBytes) {
      this.audit(actor, "upload:create", "rejected", id, { reason: "size_limit" });
      throw new ContributionError("SIZE_LIMIT", `Upload must be between 1 and ${this.policy.maxUploadBytes} bytes.`, 413);
    }
    const fileName = validateAllowedFileName(input.fileName, this.policy);
    const provenance = validateProvenance(input.provenance);
    const license = validateLicense(input.license, input.provenance, this.policy);
    const expectedHash = normalizeHash(input.expectedHash);
    const contentType = String(input.contentType || "application/octet-stream").toLowerCase();
    const taskId = String(input.taskId || actor.taskId || "contribution-intake").trim();
    if (!taskId) throw new ContributionError("TASK_REQUIRED", "Uploads must be bound to a task.", 422);
    const timestamp = nowIso(this.now);
    const session: UploadSession = {
      schema_version: CONTRIBUTION_SCHEMA_VERSION,
      id,
      status: "open",
      ownerId: actor.actorId,
      taskId,
      projectId: input.projectId || actor.projectId,
      fileName,
      contentType,
      totalBytes: input.totalBytes,
      expectedHash,
      license,
      provenance,
      receivedBytes: 0,
      contiguousBytes: 0,
      receivedRanges: [],
      quarantine: true,
      rejectionReasons: [],
      createdAt: timestamp,
      updatedAt: timestamp,
    };
    this.uploads.set(id, { session, chunks: new Map() });
    this.audit(actor, "upload:create", "allowed", id, { totalBytes: input.totalBytes, fileName, license, taskId, projectId: session.projectId || null });
    return publicUpload({ session, chunks: new Map() });
  }

  getUploadSession(actor: ContributionActor, uploadId: string): UploadSession {
    return publicUpload(this.requireUpload(actor, uploadId, "upload:read"));
  }

  putUploadChunk(actor: ContributionActor, uploadId: string, offset: number, value: Uint8Array | ArrayBuffer): UploadSession {
    const record = this.requireUpload(actor, uploadId, "upload:write");
    if (record.session.status !== "open") throw new ContributionError("UPLOAD_CLOSED", `Upload ${uploadId} is already ${record.session.status}.`, 409);
    const bytes = asBytes(value);
    if (!Number.isSafeInteger(offset) || offset < 0 || offset + bytes.byteLength > record.session.totalBytes) throw new ContributionError("INVALID_RANGE", "Chunk range is outside the declared upload size.", 422);
    if (bytes.byteLength === 0 || bytes.byteLength > this.policy.maxChunkBytes) throw new ContributionError("CHUNK_LIMIT", `Chunks must be between 1 and ${this.policy.maxChunkBytes} bytes.`, 413);
    const existing = record.chunks.get(offset);
    if (existing) {
      if (sha256(existing) !== sha256(bytes)) throw new ContributionError("CHUNK_CONFLICT", "A resumed chunk offset already contains different content.", 409);
      return publicUpload(record);
    }
    for (const [start, existingBytes] of record.chunks) {
      const end = start + existingBytes.byteLength;
      if (offset < end && start < offset + bytes.byteLength) throw new ContributionError("CHUNK_OVERLAP", "Chunk overlaps a previously uploaded byte range.", 409);
    }
    record.chunks.set(offset, new Uint8Array(bytes));
    const ranges = rangeList(record.chunks);
    record.session.receivedRanges = ranges;
    record.session.receivedBytes = [...record.chunks.values()].reduce((total, chunk) => total + chunk.byteLength, 0);
    record.session.contiguousBytes = contiguousBytes(ranges);
    record.session.updatedAt = nowIso(this.now);
    this.audit(actor, "upload:write", "allowed", uploadId, { offset, bytes: bytes.byteLength, receivedBytes: record.session.receivedBytes });
    return publicUpload(record);
  }

  finalizeUpload(actor: ContributionActor, uploadId: string): UploadSession {
    const record = this.requireUpload(actor, uploadId, "upload:finalize");
    if (record.session.status === "accepted" || record.session.status === "rejected") return publicUpload(record);
    const ranges = rangeList(record.chunks);
    if (ranges.length !== 1 || ranges[0].start !== 0 || ranges[0].end !== record.session.totalBytes) {
      this.audit(actor, "upload:finalize", "rejected", uploadId, { reason: "incomplete_upload" });
      throw new ContributionError("UPLOAD_INCOMPLETE", "All upload byte ranges must be present before finalization.", 409, { receivedRanges: ranges });
    }
    const orderedChunks = [...record.chunks.entries()].sort(([a], [b]) => a - b).map(([, bytes]) => bytes);
    const content = new Uint8Array(orderedChunks.reduce((total, chunk) => total + chunk.byteLength, 0));
    let cursor = 0;
    for (const chunk of orderedChunks) {
      content.set(chunk, cursor);
      cursor += chunk.byteLength;
    }
    const contentHash = sha256(content);
    try {
      if (record.session.expectedHash && record.session.expectedHash !== contentHash) throw new ContributionError("CONTENT_HASH_MISMATCH", "The assembled upload does not match its expected content hash.", 422, { expectedHash: record.session.expectedHash, contentHash });
      const isArchive = extensionOf(record.session.fileName) === ".zip";
      const files = isArchive ? parseZip(content, this.policy) : scanSingleFile(record.session.fileName, content, this.policy);
      record.files = files;
      record.bytesValue = content;
      record.session.status = "accepted";
      record.session.contentHash = contentHash;
      record.session.validatedAt = nowIso(this.now);
      record.session.updatedAt = record.session.validatedAt;
      this.audit(actor, "upload:finalize", "allowed", uploadId, { contentHash, fileCount: files.length, quarantined: true });
      return publicUpload(record);
    } catch (error) {
      const reason = error instanceof ContributionError ? error.code : "QUARANTINE_REJECTED";
      record.session.status = "rejected";
      record.session.rejectionReasons = [reason];
      record.session.contentHash = contentHash;
      record.session.updatedAt = nowIso(this.now);
      record.chunks.clear();
      this.audit(actor, "upload:finalize", "rejected", uploadId, { reason, contentHash });
      if (error instanceof ContributionError) throw error;
      throw new ContributionError("QUARANTINE_REJECTED", "Upload failed quarantine validation.", 422);
    }
  }

  createWorkspace(actor: ContributionActor, input: { repository: string; baseCommit: string; taskId?: string }): ContributionWorkspace {
    const id = `wsp_${randomUUID()}`;
    this.authorize(actor, "workspace:create", id);
    const repository = validateRepository(input.repository);
    const baseCommit = validateCommit(input.baseCommit);
    const taskId = String(input.taskId || actor.taskId || "").trim();
    if (!taskId) throw new ContributionError("TASK_REQUIRED", "Workspace contributions must be bound to a task.", 422);
    const timestamp = nowIso(this.now);
    const workspace: ContributionWorkspace = {
      schema_version: CONTRIBUTION_SCHEMA_VERSION,
      id,
      repository,
      baseCommit,
      taskId,
      projectId: actor.projectId,
      ownerId: actor.actorId,
      status: "isolated",
      authoritativeSourceTouched: false,
      files: [],
      uploadIds: [],
      evidence: [],
      approvals: [],
      createdAt: timestamp,
      updatedAt: timestamp,
    };
    const record = { workspace, files: new Map<string, WorkspaceFile & { bytesValue: Uint8Array }>() };
    this.workspaces.set(id, record);
    this.audit(actor, "workspace:create", "allowed", id, { repository, baseCommit, taskId, isolated: true });
    return publicWorkspace(record);
  }

  getWorkspace(actor: ContributionActor, workspaceId: string): ContributionWorkspace {
    this.authorize(actor, "workspace:read", workspaceId);
    const record = this.workspaces.get(workspaceId);
    if (!record) throw new ContributionError("WORKSPACE_NOT_FOUND", `Workspace ${workspaceId} was not found.`, 404);
    this.checkResourceAccess(actor, record.workspace.ownerId, record.workspace.taskId, record.workspace.projectId, workspaceId);
    return publicWorkspace(record);
  }

  materializeUpload(actor: ContributionActor, workspaceId: string, uploadId: string, targetPath?: string): ContributionWorkspace {
    this.authorize(actor, "workspace:write", workspaceId);
    const workspace = this.workspaces.get(workspaceId);
    if (!workspace) throw new ContributionError("WORKSPACE_NOT_FOUND", `Workspace ${workspaceId} was not found.`, 404);
    this.checkResourceAccess(actor, workspace.workspace.ownerId, workspace.workspace.taskId, workspace.workspace.projectId, workspaceId);
    const upload = this.uploads.get(uploadId);
    if (!upload) throw new ContributionError("UPLOAD_NOT_FOUND", `Upload ${uploadId} was not found.`, 404);
    this.checkResourceAccess(actor, upload.session.ownerId, upload.session.taskId, upload.session.projectId, uploadId);
    if (upload.session.status !== "accepted" || !upload.files) throw new ContributionError("UPLOAD_NOT_ACCEPTED", "Only accepted quarantined uploads can enter a workspace.", 409);
    const prefix = extensionOf(upload.session.fileName) === ".zip" ? safeWorkspacePrefix(targetPath) : "";
    const plainTarget = extensionOf(upload.session.fileName) === ".zip" ? undefined : safeRelativePath(targetPath || upload.session.fileName);
    for (const file of upload.files) {
      const pathName = plainTarget || [prefix, file.path].filter(Boolean).join("/");
      const safeName = safeRelativePath(pathName);
      if (workspace.files.has(safeName)) throw new ContributionError("WORKSPACE_PATH_CONFLICT", `Workspace already contains ${safeName}.`, 409);
      workspace.files.set(safeName, { path: safeName, bytes: file.bytes, contentHash: file.contentHash, uploadId, bytesValue: new Uint8Array(file.bytesValue) });
    }
    workspace.workspace.uploadIds = [...new Set([...workspace.workspace.uploadIds, uploadId])];
    workspace.workspace.files = [];
    workspace.workspace.updatedAt = nowIso(this.now);
    this.audit(actor, "workspace:write", "allowed", workspaceId, { uploadId, fileCount: upload.files.length, authoritativeSourceTouched: false });
    return publicWorkspace(workspace);
  }

  validateWorkspace(actor: ContributionActor, workspaceId: string): WorkspaceValidation {
    this.authorize(actor, "validate:run", workspaceId);
    const record = this.workspaces.get(workspaceId);
    if (!record) throw new ContributionError("WORKSPACE_NOT_FOUND", `Workspace ${workspaceId} was not found.`, 404);
    this.checkResourceAccess(actor, record.workspace.ownerId, record.workspace.taskId, record.workspace.projectId, workspaceId);
    const checks: ValidationCheck[] = [];
    const inputHashes = [...record.files.values()].sort((a, b) => a.path.localeCompare(b.path)).map((file) => file.contentHash);
    checks.push({ id: "workspace-isolation", passed: record.workspace.authoritativeSourceTouched === false, detail: "Authoritative source remains untouched until explicit submission." });
    checks.push({ id: "content-hashes", passed: inputHashes.length > 0 && inputHashes.every((hash) => /^sha256:[0-9a-f]{64}$/.test(hash)), detail: "Every materialized file has a content hash." });
    for (const file of record.files.values()) {
      try {
        scanSingleFile(file.path, file.bytesValue, this.policy);
      } catch (error) {
        checks.push({ id: `quarantine:${file.path}`, passed: false, detail: error instanceof Error ? error.message : String(error) });
      }
    }
    if (!checks.some((check) => check.id.startsWith("quarantine:"))) checks.push({ id: "quarantine-recheck", passed: true, detail: "Materialized files passed a second quarantine scan." });
    const passed = checks.every((check) => check.passed);
    const validationBase = { schema_version: "treatcode.contribution.validation.v1" as const, id: `val_${randomUUID()}`, workspaceId, baseCommit: record.workspace.baseCommit, passed, checks, inputHashes, recordedAt: nowIso(this.now) };
    const validation: WorkspaceValidation = { ...validationBase, artifactHash: sha256Text(canonicalJson(validationBase)) };
    record.workspace.validation = validation;
    record.workspace.status = passed ? "validated" : "validation_failed";
    record.workspace.updatedAt = validation.recordedAt;
    this.audit(actor, "validate:run", passed ? "allowed" : "rejected", workspaceId, { passed, artifactHash: validation.artifactHash, inputHashes });
    if (!passed) throw new ContributionError("WORKSPACE_VALIDATION_FAILED", "Workspace validation failed.", 422, { validation });
    return validation;
  }

  attachEvidence(actor: ContributionActor, workspaceId: string, input: EvidenceInput): EvidenceRecord {
    this.authorize(actor, "evidence:attach", workspaceId);
    const record = this.workspaces.get(workspaceId);
    if (!record) throw new ContributionError("WORKSPACE_NOT_FOUND", `Workspace ${workspaceId} was not found.`, 404);
    this.checkResourceAccess(actor, record.workspace.ownerId, record.workspace.taskId, record.workspace.projectId, workspaceId);
    if (record.workspace.status !== "validated" || !record.workspace.validation?.passed) throw new ContributionError("VALIDATION_REQUIRED", "Workspace validation must pass before evidence is attached.", 409);
    if (!String(input.label || "").trim() || !["correctness", "benchmark", "review", "diagnostic"].includes(input.kind)) throw new ContributionError("INVALID_EVIDENCE", "Evidence requires a label and supported kind.", 422);
    const payloadBytes = input.payload === undefined ? undefined : typeof input.payload === "string" ? textBytes(input.payload) : asBytes(input.payload);
    const artifactHash = normalizeHash(input.artifactHash || (payloadBytes ? sha256(payloadBytes) : undefined), true)!;
    if (payloadBytes && sha256(payloadBytes) !== artifactHash) throw new ContributionError("EVIDENCE_HASH_MISMATCH", "Evidence payload does not match artifactHash.", 422);
    if (input.sourceCommit) validateCommit(input.sourceCommit);
    const evidence: EvidenceRecord = { ...input, id: `ev_${randomUUID()}`, artifactHash, immutable: true, recordedAt: nowIso(this.now) };
    delete (evidence as { payload?: string | Uint8Array }).payload;
    record.workspace.evidence.push(evidence);
    record.workspace.updatedAt = evidence.recordedAt;
    this.audit(actor, "evidence:attach", "allowed", workspaceId, { evidenceId: evidence.id, kind: evidence.kind, artifactHash });
    return { ...evidence };
  }

  recordHumanApproval(actor: ContributionActor, workspaceId: string, input: { decision: "approved" | "rejected"; reviewer?: string }): HumanApproval {
    this.authorize(actor, "draft-pr:approve", workspaceId);
    const record = this.workspaces.get(workspaceId);
    if (!record) throw new ContributionError("WORKSPACE_NOT_FOUND", `Workspace ${workspaceId} was not found.`, 404);
    this.checkResourceAccess(actor, record.workspace.ownerId, record.workspace.taskId, record.workspace.projectId, workspaceId);
    if (actor.actorType !== "human") {
      this.audit(actor, "draft-pr:approve", "denied", workspaceId, { reason: "human_approval_required" });
      throw new ContributionError("HUMAN_APPROVAL_REQUIRED", "Only a human identity may approve a draft pull request.", 403);
    }
    if (input.reviewer && input.reviewer !== actor.actorId) throw new ContributionError("APPROVAL_IDENTITY_MISMATCH", "Approval reviewer must match the authenticated human identity.", 403);
    const approval: HumanApproval = { id: `apr_${randomUUID()}`, reviewer: actor.actorId, actorType: "human", decision: input.decision, scope: "draft-pr", commit: record.workspace.baseCommit, recordedAt: nowIso(this.now) };
    record.workspace.approvals.push(approval);
    record.workspace.updatedAt = approval.recordedAt;
    this.audit(actor, "draft-pr:approve", input.decision === "approved" ? "allowed" : "rejected", workspaceId, { approvalId: approval.id, decision: input.decision, commit: approval.commit });
    return { ...approval };
  }

  async submitDraftPullRequest(actor: ContributionActor, workspaceId: string, input: { title: string; message?: string; branch?: string }): Promise<DraftPullRequest> {
    this.authorize(actor, "draft-pr:create", workspaceId);
    const record = this.workspaces.get(workspaceId);
    if (!record) throw new ContributionError("WORKSPACE_NOT_FOUND", `Workspace ${workspaceId} was not found.`, 404);
    this.checkResourceAccess(actor, record.workspace.ownerId, record.workspace.taskId, record.workspace.projectId, workspaceId);
    if (record.workspace.status !== "validated" || !record.workspace.validation?.passed) throw new ContributionError("VALIDATION_REQUIRED", "A passing workspace validation is required before draft-PR creation.", 409);
    const requiredEvidence = new Set<EvidenceKind>(["correctness", "benchmark"]);
    const evidenceKinds = new Set(record.workspace.evidence.map((item) => item.kind));
    const missingEvidence = [...requiredEvidence].filter((kind) => !evidenceKinds.has(kind));
    if (missingEvidence.length) throw new ContributionError("EVIDENCE_REQUIRED", `Draft-PR creation requires ${missingEvidence.join(" and ")} evidence.`, 409, { missingEvidence });
    if (!this.policy.delegatedDraftPr && !record.workspace.approvals.some((approval) => approval.actorType === "human" && approval.decision === "approved")) {
      this.audit(actor, "draft-pr:create", "denied", workspaceId, { reason: "human_approval_required" });
      throw new ContributionError("HUMAN_APPROVAL_REQUIRED", "Recorded human approval is required before draft-PR creation.", 403);
    }
    const requiredActions: ContributionAction[] = ["branch:create", "commit:create", "push", "draft-pr:create"];
    for (const action of requiredActions) this.authorize(actor, action, workspaceId);
    const branch = validateBranch(input.branch || `contrib/${workspaceId.slice(-12)}`);
    const title = String(input.title || "").trim();
    if (!title) throw new ContributionError("TITLE_REQUIRED", "A pull-request title is required.", 422);
    const message = String(input.message || `TreatCode contribution: ${title}`).trim();
    const files = [...record.files.values()].sort((a, b) => a.path.localeCompare(b.path)).map((file) => ({ path: file.path, bytes: file.bytes, contentHash: file.contentHash, uploadId: file.uploadId, contentBase64: Buffer.from(file.bytesValue).toString("base64") }));
    const evidence = record.workspace.evidence.map((item) => ({ ...item }));
    const evidenceLines = evidence.map((item) => `- ${item.kind}: ${item.label} (artifactHash=${item.artifactHash})`).join("\n");
    const body = [
      "## TreatCode contribution",
      "",
      `- Base commit: ${record.workspace.baseCommit}`,
      `- Workspace validation: ${record.workspace.validation.artifactHash}`,
      `- Uploaded content: ${record.workspace.uploadIds.join(", ")}`,
      "- Immutable evidence:",
      evidenceLines,
      "",
      "This pull request was created as a draft. Merge authority remains with the repository owner.",
    ].join("\n");
    await this.github.createBranch({ repository: record.workspace.repository, branch, baseCommit: record.workspace.baseCommit });
    const commitResult = await this.github.createCommit({ repository: record.workspace.repository, branch, baseCommit: record.workspace.baseCommit, message, files, evidence });
    await this.github.pushBranch({ repository: record.workspace.repository, branch, commit: commitResult.sha });
    const pullRequest = await this.github.createDraftPullRequest({ repository: record.workspace.repository, branch, title, body, commit: commitResult.sha, draft: true });
    const result: DraftPullRequest = { id: `pr_${randomUUID()}`, repository: record.workspace.repository, url: pullRequest.url, number: pullRequest.number, title, branch, commit: commitResult.sha, draft: true, evidenceHashes: evidence.map((item) => item.artifactHash), createdAt: nowIso(this.now) };
    record.workspace.draftPullRequest = result;
    record.workspace.status = "submitted";
    record.workspace.updatedAt = result.createdAt;
    this.audit(actor, "draft-pr:create", "allowed", workspaceId, { url: result.url, branch, commit: result.commit, evidenceHashes: result.evidenceHashes });
    return { ...result };
  }

  mergePullRequest(actor: ContributionActor, _workspaceId: string): never {
    this.authorize(actor, "merge", _workspaceId);
    throw new ContributionError("MERGE_NOT_ALLOWED", "TreatCode contribution workers never receive merge authority.", 403);
  }

  auditLog(actor: ContributionActor): ContributionAuditRecord[] {
    this.authorize(actor, "audit:read", "audit");
    return this.auditRecords.map((record) => ({ ...record, metadata: { ...record.metadata } }));
  }
}
