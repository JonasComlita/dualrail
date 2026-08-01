import crypto from "crypto";
import fs from "fs";
import os from "os";
import path from "path";
import { execFileSync } from "child_process";
import { fileURLToPath } from "url";
import {
  WORKSPACE_API_SCHEMA_VERSION,
  type WorkspaceActorKind,
  type WorkspaceCollaborator,
  type WorkspaceContextFile,
  type WorkspaceContextPackage,
  type WorkspaceRecord,
  type WorkspaceScope,
  type WorkspaceSnapshot,
  type WorkspaceTask,
  type WorkspaceToolchain,
} from "./workspaceApi";

export { WORKSPACE_API_SCHEMA_VERSION };
export type { WorkspaceScope } from "./workspaceApi";

export interface WorkspaceActor {
  actor_id: string;
  kind: WorkspaceActorKind;
  scopes: WorkspaceScope[];
  token_expires_at?: string;
}

interface TokenDefinition extends WorkspaceActor {
  token: string;
}

interface TreeEntry {
  path: string;
  sha256: string;
  bytes: number;
}

interface TreeReport {
  entries: TreeEntry[];
  tree_hash: string;
  byte_count: number;
}

interface StoredWorkspace extends WorkspaceRecord {
  _source_path: string;
  _mutable_path: string;
  _snapshot_path: string;
}

interface AuditEvent {
  audit_id: string;
  event_type: string;
  workspace_id?: string;
  actor_id?: string;
  at: string;
  success: boolean;
  details: Record<string, unknown>;
}

export interface WorkspaceServiceErrorShape {
  code: string;
  message: string;
  status: number;
  details?: Record<string, unknown>;
}

export class WorkspaceServiceError extends Error {
  readonly code: string;
  readonly status: number;
  readonly details: Record<string, unknown>;

  constructor(shape: WorkspaceServiceErrorShape) {
    super(shape.message);
    this.name = "WorkspaceServiceError";
    this.code = shape.code;
    this.status = shape.status;
    this.details = shape.details || {};
  }
}

const ALL_SCOPES: WorkspaceScope[] = [
  "workspace:create",
  "workspace:read",
  "workspace:edit",
  "workspace:test",
  "workspace:context",
  "workspace:handoff",
  "workspace:destroy",
  "task:create",
  "task:read",
];

const TRANSFERABLE_SCOPES: WorkspaceScope[] = [
  "workspace:read",
  "workspace:edit",
  "workspace:test",
  "workspace:context",
  "task:create",
  "task:read",
];

const DEFAULT_TOKEN_DEFINITIONS: TokenDefinition[] = [
  {
    token: "tc-human-demo",
    actor_id: "human.demo",
    kind: "human",
    scopes: [...ALL_SCOPES],
  },
  {
    token: "tc-agent-demo",
    actor_id: "agent.demo",
    kind: "agent",
    scopes: [
      "workspace:create",
      "workspace:read",
      "workspace:edit",
      "workspace:test",
      "workspace:context",
      "task:create",
      "task:read",
    ],
  },
  {
    token: "tc-reviewer-demo",
    actor_id: "human.reviewer",
    kind: "human",
    scopes: ["workspace:read", "workspace:test", "workspace:context", "task:read"],
  },
];

const DEFAULT_IDENTITY_ALIASES: TokenDefinition[] = [
  {
    token: "",
    actor_id: "tc:identity:demo-human",
    kind: "human",
    scopes: [...ALL_SCOPES],
  },
  {
    token: "",
    actor_id: "tc:identity:demo-collaborator",
    kind: "collaborator",
    scopes: ["workspace:read", "workspace:edit", "workspace:test", "workspace:context", "task:read"],
  },
  {
    token: "",
    actor_id: "tc:identity:demo-service",
    kind: "service",
    scopes: ["workspace:read", "workspace:test", "workspace:context", "task:read"],
  },
  {
    token: "",
    actor_id: "tc:identity:demo-agent",
    kind: "agent",
    scopes: [...ALL_SCOPES.filter((scope) => scope !== "workspace:destroy")],
  },
];

const MAX_FILE_BYTES = 8 * 1024 * 1024;
const MAX_CONTEXT_FILES = 100;
const MAX_CONTEXT_BYTES = 256 * 1024;

function now(): string {
  return new Date().toISOString();
}

function sha256(value: string | Buffer): string {
  return crypto.createHash("sha256").update(value).digest("hex");
}

function stableJson(value: unknown): string {
  return JSON.stringify(value);
}

function randomSlug(): string {
  return crypto.randomBytes(8).toString("hex");
}

function slugForFile(value: string): string {
  return value.replace(/[^a-zA-Z0-9._-]+/g, "_");
}

function withinPath(candidate: string, root: string): boolean {
  const resolvedCandidate = path.resolve(candidate);
  const resolvedRoot = path.resolve(root);
  return resolvedCandidate === resolvedRoot || resolvedCandidate.startsWith(`${resolvedRoot}${path.sep}`);
}

function serviceError(
  status: number,
  code: string,
  message: string,
  details?: Record<string, unknown>,
): WorkspaceServiceError {
  return new WorkspaceServiceError({ status, code, message, details });
}

function safeString(value: unknown, fallback: string, max = 200): string {
  const text = typeof value === "string" ? value.trim() : "";
  return text ? text.slice(0, max) : fallback;
}

function uniqueScopes(value: unknown): WorkspaceScope[] {
  if (!Array.isArray(value)) return [];
  return [...new Set(value.map((item) => String(item)).filter((item): item is WorkspaceScope => ALL_SCOPES.includes(item as WorkspaceScope)))];
}

function decodeRepositoryInput(value: string): string {
  if (!value.startsWith("file://")) return value;
  try {
    return fileURLToPath(new URL(value));
  } catch {
    throw serviceError(400, "invalid_repository", "The repository file URL is not valid.");
  }
}

function gitCommand(args: string[], cwd?: string): string {
  try {
    return execFileSync("git", args, {
      cwd,
      encoding: "utf8",
      stdio: ["ignore", "pipe", "pipe"],
      maxBuffer: 2 * 1024 * 1024,
    }).trim();
  } catch {
    throw serviceError(422, "repository_unavailable", "The requested repository or Git operation is unavailable.");
  }
}

function discoverRepositoryRoot(): string {
  const candidates = [
    process.env.TREATCODE_REPOSITORY_ROOT,
    path.resolve(__dirname, ".."),
    process.cwd(),
    path.resolve(process.cwd(), ".."),
  ].filter((candidate): candidate is string => Boolean(candidate));

  for (const candidate of candidates) {
    try {
      const resolved = gitCommand(["-C", path.resolve(candidate), "rev-parse", "--show-toplevel"]);
      if (resolved) return path.resolve(resolved);
    } catch {
      // Continue through the candidate list.
    }
  }
  throw serviceError(500, "repository_root_unavailable", "TreatCode could not locate its authoritative Git repository.");
}

function normalizedRemote(value: string): string {
  return value.trim().replace(/\.git$/i, "").replace(/\/$/, "").toLowerCase();
}

function originForRepository(repositoryPath: string): string | null {
  try {
    const origin = execFileSync("git", ["-C", repositoryPath, "config", "--get", "remote.origin.url"], {
      encoding: "utf8",
      stdio: ["ignore", "pipe", "pipe"],
    }).trim();
    return origin || null;
  } catch {
    return null;
  }
}

function tokenDefinitions(): TokenDefinition[] {
  const configured = process.env.TREATCODE_WORKSPACE_TOKENS;
  if (!configured) return DEFAULT_TOKEN_DEFINITIONS;

  try {
    const parsed = JSON.parse(configured) as unknown;
    const candidates: unknown[] = Array.isArray(parsed)
      ? parsed
      : Object.entries((parsed || {}) as Record<string, unknown>).map(([token, value]) => ({ token, ...(value as object) }));
    const definitions = candidates.map((candidate) => {
      const item = (candidate || {}) as Record<string, unknown>;
      const scopes = uniqueScopes(item.scopes);
      if (!item.token || !item.actor_id || !scopes.length) return null;
      return {
        token: String(item.token),
        actor_id: String(item.actor_id),
        kind: (["human", "collaborator", "agent", "service"] as string[]).includes(String(item.kind)) ? String(item.kind) as WorkspaceActorKind : "service",
        scopes,
        token_expires_at: typeof item.expires_at === "string" ? item.expires_at : undefined,
      } as TokenDefinition;
    }).filter((item): item is TokenDefinition => Boolean(item));
    return definitions.length ? definitions : DEFAULT_TOKEN_DEFINITIONS;
  } catch {
    return DEFAULT_TOKEN_DEFINITIONS;
  }
}

function headerValue(headers: Record<string, string | string[] | undefined>, name: string): string {
  const value = headers[name] || headers[name.toLowerCase()];
  return Array.isArray(value) ? String(value[0] || "") : String(value || "");
}

export function authenticateWorkspace(headers: Record<string, string | string[] | undefined>): WorkspaceActor {
  const authorization = headerValue(headers, "authorization");
  const alternate = headerValue(headers, "x-treatcode-token");
  const token = authorization.toLowerCase().startsWith("bearer ") ? authorization.slice(7).trim() : alternate.trim();
  if (!token) throw serviceError(401, "authentication_required", "A workspace bearer token is required.");

  const definition = tokenDefinitions().find((candidate) => candidate.token === token);
  if (!definition) throw serviceError(401, "invalid_token", "The workspace token is invalid or revoked.");
  if (definition.token_expires_at && Date.parse(definition.token_expires_at) <= Date.now()) {
    throw serviceError(401, "token_expired", "The workspace token has expired.");
  }
  const revoked = String(process.env.TREATCODE_REVOKED_WORKSPACE_TOKENS || "")
    .split(",")
    .map((item) => item.trim())
    .filter(Boolean);
  if (revoked.includes(sha256(token)) || revoked.includes(token)) {
    throw serviceError(401, "invalid_token", "The workspace token is invalid or revoked.");
  }

  return {
    actor_id: definition.actor_id,
    kind: definition.kind,
    scopes: [...definition.scopes],
    token_expires_at: definition.token_expires_at,
  };
}

export function actorById(actorId: string): WorkspaceActor | null {
  const definition = [...tokenDefinitions(), ...DEFAULT_IDENTITY_ALIASES].find((candidate) => candidate.actor_id === actorId);
  if (!definition) return null;
  return {
    actor_id: definition.actor_id,
    kind: definition.kind,
    scopes: [...definition.scopes],
    token_expires_at: definition.token_expires_at,
  };
}

export function hasScope(actor: WorkspaceActor, scope: WorkspaceScope): boolean {
  return actor.scopes.includes(scope);
}

export class WorkspaceService {
  readonly repositoryRoot: string;
  readonly storageRoot: string;
  private readonly registryRoot: string;
  private readonly mutableRoot: string;
  private readonly snapshotRoot: string;
  private readonly auditPath: string;

  constructor() {
    this.repositoryRoot = discoverRepositoryRoot();
    const configuredRoot = process.env.TREATCODE_WORKSPACE_ROOT;
    this.storageRoot = path.resolve(configuredRoot || path.join(os.tmpdir(), "treatcode-workspaces"));
    if (withinPath(this.storageRoot, this.repositoryRoot)) {
      throw serviceError(500, "unsafe_storage_root", "Workspace mutable storage must not be inside the authoritative repository.");
    }
    this.registryRoot = path.join(this.storageRoot, "registry");
    this.mutableRoot = path.join(this.storageRoot, "mutable");
    this.snapshotRoot = path.join(this.storageRoot, "snapshots");
    this.auditPath = path.join(this.storageRoot, "audit.ndjson");
    for (const directory of [this.storageRoot, this.registryRoot, this.mutableRoot, this.snapshotRoot]) fs.mkdirSync(directory, { recursive: true });
  }

  capabilities(): Record<string, unknown> {
    return {
      schema_version: WORKSPACE_API_SCHEMA_VERSION,
      api_version: "v1",
      workspace_modes: ["isolated"],
      statuses: ["active", "suspended", "destroyed", "lost"],
      scopes: [...ALL_SCOPES],
      transferable_scopes: [...TRANSFERABLE_SCOPES],
      supported_operations: [
        "create",
        "inspect",
        "edit",
        "snapshot",
        "resume",
        "disconnect",
        "export",
        "handoff",
        "context-package",
        "task",
        "destroy",
      ],
      authentication: "Authorization: Bearer <configured workspace token>",
      built_in_development_actors: ["tc:identity:demo-human", "tc:identity:demo-collaborator", "tc:identity:demo-agent"],
      retention_policy: "Mutable workspace and snapshot storage are removed on destroy; append-only audit records remain.",
      authoritative_repository: this.repositoryRoot,
    };
  }

  createWorkspace(input: Record<string, unknown>, actor: WorkspaceActor): WorkspaceRecord {
    const requestedRepository = safeString(input.repository || input.repository_url, this.repositoryRoot, 1000);
    const source = this.resolveRepository(requestedRepository);
    const requestedCommit = safeString(input.commit || input.base_commit, "HEAD", 100);
    const commit = this.resolveCommit(source.path, requestedCommit);
    const name = safeString(input.name, `Workspace ${commit.slice(0, 8)}`, 100);
    const mode = safeString(input.mode, "isolated", 30);
    if (mode !== "isolated") throw serviceError(400, "unsupported_workspace_mode", "P08 workspaces must use isolated mode.");

    const slug = `workspace-${slugForFile(name).toLowerCase().slice(0, 32)}-${randomSlug()}`;
    const id = `tc:workspace:${slug}`;
    const mutablePath = path.join(this.mutableRoot, slug);
    const snapshotPath = path.join(this.snapshotRoot, slug);
    fs.mkdirSync(mutablePath, { recursive: false });
    fs.mkdirSync(snapshotPath, { recursive: true });

    try {
      this.provisionFromCommit(source.path, commit, mutablePath);
      const tree = this.scanTree(mutablePath);
      const record: StoredWorkspace = {
        id,
        entity_type: "workspace",
        name,
        repository: source.display,
        requested_commit: requestedCommit,
        base_commit: commit,
        commit,
        mode: "isolated",
        image: safeString(input.image, "ghcr.io/trit/treatcode-dev:stable", 160),
        toolchain: this.toolchain(input.toolchain, input.image),
        status: "active",
        owner_actor_id: actor.actor_id,
        owner_kind: actor.kind,
        collaborators: [],
        snapshots: [],
        tasks: [],
        file_version: 0,
        last_tree_hash: tree.tree_hash,
        created_at: now(),
        updated_at: now(),
        last_connected_at: now(),
        retention_policy: "Mutable workspace and snapshot storage are removed on destroy; append-only audit records remain.",
        _source_path: source.path,
        _mutable_path: mutablePath,
        _snapshot_path: snapshotPath,
      };
      this.save(record);
      this.audit("workspace.created", id, actor.actor_id, true, {
        repository: source.display,
        commit,
        image: record.image,
        toolchain: record.toolchain,
        authoritative_checkout_touched: false,
      });
      return this.publicRecord(record);
    } catch (error) {
      this.removeDirectoryIfSafe(mutablePath, this.mutableRoot);
      this.removeDirectoryIfSafe(snapshotPath, this.snapshotRoot);
      throw error;
    }
  }

  list(actor: WorkspaceActor): WorkspaceRecord[] {
    const records: WorkspaceRecord[] = [];
    for (const file of fs.readdirSync(this.registryRoot).filter((candidate) => candidate.endsWith(".json")).sort()) {
      try {
        const record = JSON.parse(fs.readFileSync(path.join(this.registryRoot, file), "utf8")) as StoredWorkspace;
        if (record.status === "destroyed") continue;
        if (record.owner_actor_id === actor.actor_id || record.collaborators.some((collaborator) => collaborator.actor_id === actor.actor_id && collaborator.scopes.includes("workspace:read") && actor.scopes.includes("workspace:read"))) {
          records.push(this.publicRecord(record));
        }
      } catch {
        // A malformed record does not make other workspaces inaccessible.
      }
    }
    return records;
  }

  get(id: string): StoredWorkspace {
    if (!/^tc:workspace:[a-z0-9][a-z0-9._-]*$/.test(id)) throw serviceError(400, "invalid_workspace_id", "The workspace ID is not valid.");
    const file = path.join(this.registryRoot, `${slugForFile(id)}.json`);
    if (!fs.existsSync(file)) throw serviceError(404, "workspace_not_found", "The workspace does not exist.");
    try {
      return JSON.parse(fs.readFileSync(file, "utf8")) as StoredWorkspace;
    } catch {
      throw serviceError(500, "workspace_record_invalid", "The workspace record cannot be read.");
    }
  }

  publicRecord(record: StoredWorkspace): WorkspaceRecord {
    const copy = JSON.parse(JSON.stringify(record)) as Record<string, unknown>;
    delete copy._source_path;
    delete copy._mutable_path;
    delete copy._snapshot_path;
    return copy as unknown as WorkspaceRecord;
  }

  canAccess(record: StoredWorkspace, actor: WorkspaceActor, scope: WorkspaceScope): boolean {
    if (record.status === "destroyed") return false;
    if (!hasScope(actor, scope)) return false;
    if (record.owner_actor_id === actor.actor_id) return true;
    const collaborator = record.collaborators.find((item) => item.actor_id === actor.actor_id);
    return Boolean(collaborator && collaborator.scopes.includes(scope));
  }

  requireAccess(record: StoredWorkspace, actor: WorkspaceActor, scope: WorkspaceScope): void {
    if (record.status === "destroyed") throw serviceError(410, "workspace_destroyed", "The workspace has been destroyed and access is revoked.");
    if (!this.canAccess(record, actor, scope)) {
      throw serviceError(403, "workspace_access_denied", "The actor is not authorized for this workspace action.", { required_scope: scope });
    }
  }

  requireAuditAccess(record: StoredWorkspace, actor: WorkspaceActor): void {
    if (record.status === "destroyed") {
      if (record.owner_actor_id === actor.actor_id && hasScope(actor, "workspace:read")) return;
      throw serviceError(410, "workspace_destroyed", "The workspace has been destroyed and access is revoked.");
    }
    this.requireAccess(record, actor, "workspace:read");
  }

  updateFile(record: StoredWorkspace, actor: WorkspaceActor, relativePath: string, content: string, expectedVersion?: number): Record<string, unknown> {
    this.ensureActive(record);
    const target = this.safeWorkspacePath(record, relativePath);
    if (typeof expectedVersion === "number" && expectedVersion !== record.file_version) {
      throw serviceError(409, "workspace_version_conflict", "The workspace changed since the supplied file version.", { current_version: record.file_version });
    }
    if (Buffer.byteLength(content, "utf8") > MAX_FILE_BYTES) throw serviceError(413, "file_too_large", "Workspace files are limited to 8 MiB.");
    this.assertNoSymlinkAncestors(target, record._mutable_path);
    if (fs.existsSync(target) && fs.statSync(target).isDirectory()) throw serviceError(409, "path_is_directory", "A directory cannot be replaced by a file.");
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.writeFileSync(target, content, "utf8");
    record.file_version += 1;
    const tree = this.scanTree(record._mutable_path);
    record.last_tree_hash = tree.tree_hash;
    record.updated_at = now();
    this.save(record);
    this.audit("workspace.file_updated", record.id, actor.actor_id, true, { path: this.normalizeRelative(relativePath), bytes: Buffer.byteLength(content, "utf8"), file_version: record.file_version });
    return { path: this.normalizeRelative(relativePath), bytes: Buffer.byteLength(content, "utf8"), sha256: sha256(content), file_version: record.file_version, tree_hash: tree.tree_hash };
  }

  readFile(record: StoredWorkspace, relativePath: string): Record<string, unknown> {
    const target = this.safeWorkspacePath(record, relativePath);
    this.assertNoSymlinkAncestors(target, record._mutable_path);
    if (!fs.existsSync(target) || !fs.statSync(target).isFile()) throw serviceError(404, "file_not_found", "The workspace file does not exist.");
    const buffer = fs.readFileSync(target);
    if (buffer.length > MAX_FILE_BYTES) throw serviceError(413, "file_too_large", "The workspace file is larger than the API read limit.");
    return { path: this.normalizeRelative(relativePath), content: buffer.toString("utf8"), bytes: buffer.length, sha256: sha256(buffer), file_version: record.file_version };
  }

  listFiles(record: StoredWorkspace): TreeReport {
    return this.scanTree(record._mutable_path);
  }

  createSnapshot(record: StoredWorkspace, actor: WorkspaceActor, label: unknown): WorkspaceSnapshot {
    this.ensureActive(record);
    const tree = this.scanTree(record._mutable_path);
    const sequence = record.snapshots.length + 1;
    const snapshotId = `tc:snapshot:${slugForFile(record.id).toLowerCase()}-${sequence}-${randomSlug().slice(0, 8)}`;
    const target = path.join(record._snapshot_path, slugForFile(snapshotId));
    fs.mkdirSync(target, { recursive: true });
    this.copyTree(record._mutable_path, path.join(target, "files"));
    const snapshot: WorkspaceSnapshot = {
      snapshot_id: snapshotId,
      workspace_id: record.id,
      sequence,
      label: safeString(label, `Checkpoint ${sequence}`, 80),
      tree_hash: tree.tree_hash,
      file_count: tree.entries.length,
      byte_count: tree.byte_count,
      created_at: now(),
      created_by: actor.actor_id,
    };
    record.snapshots.push(snapshot);
    record.last_tree_hash = tree.tree_hash;
    record.updated_at = now();
    this.save(record);
    this.audit("workspace.snapshot_created", record.id, actor.actor_id, true, { snapshot_id: snapshotId, tree_hash: tree.tree_hash, file_count: tree.entries.length, byte_count: tree.byte_count });
    return snapshot;
  }

  restoreSnapshot(record: StoredWorkspace, actor: WorkspaceActor, snapshotId: string): WorkspaceSnapshot {
    this.ensureActive(record);
    const snapshot = record.snapshots.find((item) => item.snapshot_id === snapshotId);
    if (!snapshot) throw serviceError(404, "snapshot_not_found", "The workspace snapshot does not exist.");
    const source = path.join(record._snapshot_path, slugForFile(snapshotId), "files");
    if (!fs.existsSync(source)) throw serviceError(410, "snapshot_storage_missing", "The snapshot manifest exists but its retained files are unavailable.");
    this.removeDirectoryIfSafe(record._mutable_path, this.mutableRoot);
    fs.mkdirSync(record._mutable_path, { recursive: true });
    this.copyTree(source, record._mutable_path);
    record.file_version += 1;
    record.last_tree_hash = this.scanTree(record._mutable_path).tree_hash;
    record.updated_at = now();
    this.save(record);
    this.audit("workspace.snapshot_restored", record.id, actor.actor_id, true, { snapshot_id: snapshotId, file_version: record.file_version });
    return snapshot;
  }

  disconnect(record: StoredWorkspace, actor: WorkspaceActor): WorkspaceRecord {
    if (record.status === "destroyed") throw serviceError(410, "workspace_destroyed", "The workspace has been destroyed and access is revoked.");
    record.status = "suspended";
    record.last_disconnected_at = now();
    record.updated_at = now();
    this.save(record);
    this.audit("workspace.disconnected", record.id, actor.actor_id, true, { persistent_state: true, tree_hash: this.scanTree(record._mutable_path).tree_hash });
    return this.publicRecord(record);
  }

  resume(record: StoredWorkspace, actor: WorkspaceActor): WorkspaceRecord {
    if (record.status === "destroyed") throw serviceError(410, "workspace_destroyed", "The workspace has been destroyed and access is revoked.");
    if (!fs.existsSync(record._mutable_path)) {
      record.status = "lost";
      record.updated_at = now();
      this.save(record);
      throw serviceError(409, "workspace_storage_lost", "The workspace record exists but its mutable storage is missing.");
    }
    record.status = "active";
    record.last_connected_at = now();
    record.updated_at = now();
    const tree = this.scanTree(record._mutable_path);
    record.last_tree_hash = tree.tree_hash;
    this.save(record);
    this.audit("workspace.resumed", record.id, actor.actor_id, true, { recovered_tree_hash: tree.tree_hash, file_version: record.file_version });
    return this.publicRecord(record);
  }

  exportWorkspace(record: StoredWorkspace, actor: WorkspaceActor): Record<string, unknown> {
    const snapshot = this.createSnapshot(record, actor, "Export checkpoint");
    const tree = this.scanTree(record._mutable_path);
    const result = {
      format: "treatcode.workspace.export.v1",
      exported_at: now(),
      workspace: this.publicRecord(record),
      snapshot,
      files: tree.entries,
      tree_hash: tree.tree_hash,
      retention: record.retention_policy,
    };
    this.audit("workspace.exported", record.id, actor.actor_id, true, { snapshot_id: snapshot.snapshot_id, tree_hash: tree.tree_hash });
    return result;
  }

  handoff(record: StoredWorkspace, actor: WorkspaceActor, targetActorId: string, requestedScopes: unknown): Record<string, unknown> {
    if (record.status === "destroyed") throw serviceError(410, "workspace_destroyed", "The workspace has been destroyed and access is revoked.");
    const target = actorById(targetActorId);
    if (!target) throw serviceError(404, "recipient_not_found", "The requested collaborator is not a configured TreatCode identity.");
    const requested = uniqueScopes(requestedScopes);
    const scopes = (requested.length ? requested : ["workspace:read", "workspace:edit", "workspace:context"] as WorkspaceScope[])
      .filter((scope) => TRANSFERABLE_SCOPES.includes(scope) && target.scopes.includes(scope));
    if (!scopes.length) throw serviceError(400, "no_transferable_scopes", "The handoff did not request a scope allowed for the recipient.");
    const collaborator: WorkspaceCollaborator = {
      actor_id: target.actor_id,
      kind: target.kind,
      scopes,
      granted_at: now(),
      granted_by: actor.actor_id,
    };
    record.collaborators = [...record.collaborators.filter((item) => item.actor_id !== target.actor_id), collaborator];
    record.updated_at = now();
    this.save(record);
    this.audit("workspace.handoff", record.id, actor.actor_id, true, { to_actor_id: target.actor_id, granted_scopes: scopes });
    return { workspace: this.publicRecord(record), handoff: collaborator };
  }

  revokeCollaborator(record: StoredWorkspace, actor: WorkspaceActor, targetActorId: string): WorkspaceRecord {
    const before = record.collaborators.length;
    record.collaborators = record.collaborators.filter((item) => item.actor_id !== targetActorId);
    if (record.collaborators.length === before) throw serviceError(404, "collaborator_not_found", "The collaborator is not attached to this workspace.");
    record.updated_at = now();
    this.save(record);
    this.audit("workspace.handoff_revoked", record.id, actor.actor_id, true, { to_actor_id: targetActorId });
    return this.publicRecord(record);
  }

  destroy(record: StoredWorkspace, actor: WorkspaceActor): WorkspaceRecord {
    if (record.owner_actor_id !== actor.actor_id) throw serviceError(403, "owner_required", "Only the workspace owner may destroy the workspace.");
    const destroyedAt = now();
    this.removeDirectoryIfSafe(record._mutable_path, this.mutableRoot);
    this.removeDirectoryIfSafe(record._snapshot_path, this.snapshotRoot);
    record.status = "destroyed";
    record.destroyed_at = destroyedAt;
    record.updated_at = destroyedAt;
    record.collaborators = [];
    record.last_tree_hash = null;
    this.save(record);
    this.audit("workspace.destroyed", record.id, actor.actor_id, true, { mutable_storage_removed: true, snapshot_storage_removed: true, access_revoked: true, retention_policy: record.retention_policy });
    return this.publicRecord(record);
  }

  auditForWorkspace(workspaceId: string): AuditEvent[] {
    if (!fs.existsSync(this.auditPath)) return [];
    return fs.readFileSync(this.auditPath, "utf8").split(/\r?\n/).filter(Boolean).map((line) => {
      try { return JSON.parse(line) as AuditEvent; } catch { return null; }
    }).filter((item): item is AuditEvent => Boolean(item && item.workspace_id === workspaceId));
  }

  recordDenied(workspaceId: string | undefined, actorId: string | undefined, code: string, details: Record<string, unknown> = {}): void {
    this.audit("workspace.action_denied", workspaceId, actorId, false, { code, ...details });
  }

  createTask(record: StoredWorkspace, actor: WorkspaceActor, input: Record<string, unknown>): WorkspaceTask {
    this.ensureActive(record);
    const title = safeString(input.title, "Untitled workspace task", 160);
    const scope = safeString(input.scope, "Implement the requested change.", 1000);
    const acceptance = Array.isArray(input.acceptance) ? input.acceptance.map((item) => String(item).trim()).filter(Boolean).slice(0, 20) : [];
    if (!acceptance.length) throw serviceError(400, "acceptance_required", "A task needs at least one acceptance criterion.");
    const rawPriority = safeString(input.priority, "normal", 20);
    const priority = (["low", "normal", "high", "critical"] as const).includes(rawPriority as "low" | "normal" | "high" | "critical") ? rawPriority as WorkspaceTask["priority"] : "normal";
    const task: WorkspaceTask = {
      id: `tc:task:${slugForFile(record.id).toLowerCase()}-${record.tasks.length + 1}-${randomSlug().slice(0, 8)}`,
      entity_type: "task",
      title,
      scope,
      acceptance,
      priority,
      assigned_to: typeof input.assigned_to === "string" ? input.assigned_to.trim() : undefined,
      workspace_id: record.id,
      context_scopes: Array.isArray(input.context_scopes) ? input.context_scopes.map((item) => String(item)).slice(0, 20) : [],
      status: "queued",
      created_at: now(),
      updated_at: now(),
      created_by: actor.actor_id,
    };
    record.tasks.push(task);
    record.updated_at = now();
    this.save(record);
    this.audit("task.created", record.id, actor.actor_id, true, { task_id: task.id, priority: task.priority });
    return task;
  }

  task(record: StoredWorkspace, taskId: string): WorkspaceTask {
    const task = record.tasks.find((item) => item.id === taskId);
    if (!task) throw serviceError(404, "task_not_found", "The workspace task does not exist.");
    return task;
  }

  approveTask(record: StoredWorkspace, actor: WorkspaceActor, taskId: string): WorkspaceTask {
    if (actor.kind === "agent" || actor.kind === "service") throw serviceError(403, "human_approval_required", "Only an authorized human or collaborator may approve a workspace task.");
    const task = this.task(record, taskId);
    task.status = "active";
    task.updated_at = now();
    record.updated_at = task.updated_at;
    this.save(record);
    this.audit("task.approved", record.id, actor.actor_id, true, { task_id: task.id, approval_kind: "human" });
    return task;
  }

  contextPackage(record: StoredWorkspace, actor: WorkspaceActor, scopes: unknown, taskId?: string, limits?: Record<string, unknown>): WorkspaceContextPackage {
    const requested = Array.isArray(scopes) ? scopes.map((item) => String(item)).filter(Boolean).slice(0, MAX_CONTEXT_FILES) : [];
    if (!requested.length) throw serviceError(400, "context_scope_required", "A bounded context package needs at least one file or directory scope.");
    const maxFiles = Math.max(1, Math.min(MAX_CONTEXT_FILES, Number(limits?.max_files || MAX_CONTEXT_FILES)));
    const maxBytes = Math.max(1024, Math.min(MAX_CONTEXT_BYTES, Number(limits?.max_bytes || MAX_CONTEXT_BYTES)));
    const paths: string[] = [];
    for (const scope of requested) {
      const target = this.safeWorkspacePath(record, scope);
      this.assertNoSymlinkAncestors(target, record._mutable_path);
      if (!fs.existsSync(target)) throw serviceError(404, "context_scope_missing", `The context scope does not exist: ${scope}.`);
      const stat = fs.statSync(target);
      if (stat.isDirectory()) this.collectFiles(record._mutable_path, target, paths, maxFiles);
      else if (stat.isFile()) paths.push(this.normalizeRelative(scope));
    }
    const selected = [...new Set(paths)].sort().slice(0, maxFiles);
    const files: WorkspaceContextFile[] = [];
    let totalBytes = 0;
    for (const relativePath of selected) {
      const entry = this.readFile(record, relativePath);
      const content = String(entry.content || "");
      const bytes = Buffer.byteLength(content, "utf8");
      if (totalBytes + bytes > maxBytes) break;
      files.push({ path: relativePath, sha256: String(entry.sha256), bytes, content });
      totalBytes += bytes;
    }
    if (!files.length) throw serviceError(413, "context_budget_exceeded", "The requested context exceeds the configured byte budget.");
    const packageId = `tc:context:${slugForFile(record.id).toLowerCase()}-${randomSlug()}`;
    const packageWithoutHash = { package_id: packageId, workspace_id: record.id, task_id: taskId, source_commit: record.commit, requested_scopes: requested, files, total_bytes: totalBytes, max_files: maxFiles, max_bytes: maxBytes };
    const result: WorkspaceContextPackage = { ...packageWithoutHash, content_hash: sha256(stableJson(packageWithoutHash)), created_at: now(), created_by: actor.actor_id };
    this.audit("workspace.context_package_created", record.id, actor.actor_id, true, { package_id: packageId, task_id: taskId, file_count: files.length, total_bytes: totalBytes, content_hash: result.content_hash });
    return result;
  }

  runCheck(record: StoredWorkspace, actor: WorkspaceActor, kind: string): Record<string, unknown> {
    this.ensureActive(record);
    const normalized = kind.toLowerCase();
    if (normalized !== "doctor" && normalized !== "smoke") throw serviceError(400, "unsupported_check", "Only doctor and smoke workspace checks are available.");
    const before = this.authoritativeStatus(record._source_path);
    const requiredFiles = ["AGENTS.md", "TEST_MANIFEST.json", "tools/trit_tool.py", "treatcode/package.json"];
    const missing = requiredFiles.filter((relativePath) => !fs.existsSync(path.join(record._mutable_path, relativePath)));
    let execution: { command: string; returncode: number; output: string };
    if (normalized === "doctor") {
      execution = this.runPython(record._mutable_path, ["tools/trit_tool.py", "doctor", "--no-commands", "--build-dir", path.join(record._mutable_path, ".treatcode-build"), "--json"]);
    } else {
      execution = this.runPython(record._mutable_path, ["-c", "import json, pathlib; required=['AGENTS.md','TEST_MANIFEST.json','tools/trit_tool.py','treatcode/package.json']; missing=[p for p in required if not pathlib.Path(p).exists()]; print(json.dumps({'smoke_preflight': not missing, 'missing': missing}))"]);
    }
    const after = this.authoritativeStatus(record._source_path);
    const result = {
      kind: normalized,
      command: normalized === "doctor" ? "python tools/trit_tool.py doctor --no-commands --json" : "python isolated smoke preflight",
      executed_in: "isolated_workspace",
      isolated_commit: record.commit,
      required_files: requiredFiles,
      missing_files: [...new Set([...missing, ...(normalized === "doctor" ? [] : [])])],
      tool_returncode: execution.returncode,
      tool_output: execution.output.slice(0, 12000),
      authoritative_checkout_unchanged: before === after,
      authoritative_status_before: before,
      authoritative_status_after: after,
      ok: missing.length === 0 && before === after && execution.returncode === 0,
      recorded_at: now(),
    };
    this.audit("workspace.check_ran", record.id, actor.actor_id, Boolean(result.ok), { kind: normalized, authoritative_checkout_unchanged: result.authoritative_checkout_unchanged, tool_returncode: execution.returncode });
    return result;
  }

  private resolveRepository(requested: string): { path: string; display: string } {
    const decoded = decodeRepositoryInput(requested);
    let candidatePath = decoded;
    if (/^https?:\/\//i.test(decoded) || /^ssh:\/\//i.test(decoded) || decoded.includes("@") && decoded.includes(":")) {
      const origin = originForRepository(this.repositoryRoot);
      if (!origin || normalizedRemote(origin) !== normalizedRemote(decoded)) throw serviceError(422, "repository_unavailable", "Only a configured local repository mirror may be provisioned by this development server.");
      candidatePath = this.repositoryRoot;
    } else if (!path.isAbsolute(candidatePath)) {
      candidatePath = path.resolve(process.cwd(), candidatePath);
    }
    let resolvedRoot: string;
    try {
      resolvedRoot = gitCommand(["-C", candidatePath, "rev-parse", "--show-toplevel"]);
    } catch {
      throw serviceError(422, "repository_unavailable", "The requested repository is not a readable Git checkout.");
    }
    return { path: path.resolve(resolvedRoot), display: /^https?:\/\//i.test(decoded) || /^ssh:\/\//i.test(decoded) ? decoded : path.resolve(resolvedRoot) };
  }

  private resolveCommit(repositoryPath: string, requested: string): string {
    if (requested.startsWith("-")) throw serviceError(400, "invalid_commit", "The requested commit is not valid.");
    try {
      const commit = execFileSync("git", ["-C", repositoryPath, "rev-parse", "--verify", `${requested}^{commit}`], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }).trim();
      if (!/^[0-9a-f]{40}$/i.test(commit)) throw new Error("not a commit");
      return commit;
    } catch {
      throw serviceError(422, "commit_not_found", "The requested repository commit does not exist.");
    }
  }

  private provisionFromCommit(repositoryPath: string, commit: string, destination: string): void {
    const staging = fs.mkdtempSync(path.join(this.storageRoot, "archive-"));
    const archivePath = path.join(staging, "workspace.tar");
    const archiveFd = fs.openSync(archivePath, "w");
    try {
      execFileSync("git", ["-C", repositoryPath, "archive", "--format=tar", commit], { stdio: ["ignore", archiveFd, "pipe"], maxBuffer: 4 * 1024 * 1024, timeout: 120000 });
    } catch {
      throw serviceError(422, "workspace_provision_failed", "The exact repository commit could not be archived.");
    } finally {
      fs.closeSync(archiveFd);
    }
    try {
      execFileSync(process.env.TREATCODE_TAR_COMMAND || "tar", ["-xf", archivePath, "-C", destination], { stdio: ["ignore", "pipe", "pipe"], maxBuffer: 4 * 1024 * 1024, timeout: 120000 });
    } catch {
      throw serviceError(500, "workspace_extract_failed", "The isolated workspace archive could not be extracted.");
    } finally {
      this.removeDirectoryIfSafe(staging, this.storageRoot);
    }
  }

  private toolchain(value: unknown, image: unknown): WorkspaceToolchain {
    if (value && typeof value === "object") {
      const item = value as Record<string, unknown>;
      return { name: safeString(item.name, "trit-toolchain", 80), version: safeString(item.version, "v2", 40), image: safeString(item.image, safeString(image, "ghcr.io/trit/treatcode-dev:stable", 160), 160) };
    }
    if (typeof value === "string" && value.trim()) return { name: value.trim().slice(0, 80), version: "declared", image: safeString(image, "ghcr.io/trit/treatcode-dev:stable", 160) };
    return { name: "trit-toolchain", version: "v2", image: safeString(image, "ghcr.io/trit/treatcode-dev:stable", 160) };
  }

  private ensureActive(record: StoredWorkspace): void {
    if (record.status === "destroyed") throw serviceError(410, "workspace_destroyed", "The workspace has been destroyed and access is revoked.");
    if (record.status !== "active") throw serviceError(409, "workspace_not_active", "The workspace must be resumed before it can be edited or checkpointed.");
  }

  private save(record: StoredWorkspace): void {
    const file = path.join(this.registryRoot, `${slugForFile(record.id)}.json`);
    const temporary = `${file}.${process.pid}.${randomSlug()}.tmp`;
    fs.writeFileSync(temporary, `${JSON.stringify(record, null, 2)}\n`, "utf8");
    fs.renameSync(temporary, file);
  }

  private audit(eventType: string, workspaceId: string | undefined, actorId: string | undefined, success: boolean, details: Record<string, unknown>): void {
    const event: AuditEvent = { audit_id: `tc:audit:${randomSlug()}`, event_type: eventType, workspace_id: workspaceId, actor_id: actorId, at: now(), success, details };
    fs.appendFileSync(this.auditPath, `${JSON.stringify(event)}\n`, "utf8");
  }

  private safeWorkspacePath(record: StoredWorkspace, relativePath: string): string {
    const normalized = this.normalizeRelative(relativePath);
    const target = path.resolve(record._mutable_path, normalized);
    if (!withinPath(target, record._mutable_path)) throw serviceError(400, "invalid_workspace_path", "Workspace paths must remain inside the isolated mutable directory.");
    return target;
  }

  private normalizeRelative(relativePath: string): string {
    const raw = String(relativePath || "").replace(/\\/g, "/").trim();
    if (!raw || raw.startsWith("/") || /^[a-zA-Z]:\//.test(raw)) throw serviceError(400, "invalid_workspace_path", "A relative workspace path is required.");
    const parts = raw.split("/").filter(Boolean);
    if (!parts.length || parts.some((part) => part === "." || part === ".." || part === ".git" || part.includes("\0"))) throw serviceError(400, "invalid_workspace_path", "Workspace paths cannot traverse parent or Git metadata directories.");
    return parts.join("/");
  }

  private assertNoSymlinkAncestors(target: string, root: string): void {
    let cursor = path.resolve(target);
    const resolvedRoot = path.resolve(root);
    while (withinPath(cursor, resolvedRoot)) {
      if (fs.existsSync(cursor) && fs.lstatSync(cursor).isSymbolicLink()) throw serviceError(400, "symlink_not_allowed", "Workspace file operations cannot follow symbolic links.");
      if (cursor === resolvedRoot) return;
      cursor = path.dirname(cursor);
    }
    throw serviceError(400, "invalid_workspace_path", "The workspace path is outside the isolated directory.");
  }

  private scanTree(root: string): TreeReport {
    const entries: TreeEntry[] = [];
    const visit = (directory: string): void => {
      for (const item of fs.readdirSync(directory, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
        const fullPath = path.join(directory, item.name);
        if (item.isSymbolicLink()) throw serviceError(400, "symlink_not_allowed", "Workspace snapshots cannot contain symbolic links.");
        if (item.isDirectory()) visit(fullPath);
        else if (item.isFile()) {
          const buffer = fs.readFileSync(fullPath);
          if (buffer.length > MAX_FILE_BYTES) throw serviceError(413, "file_too_large", "A workspace file is larger than the supported limit.");
          entries.push({ path: path.relative(root, fullPath).split(path.sep).join("/"), sha256: sha256(buffer), bytes: buffer.length });
        }
      }
    };
    visit(root);
    entries.sort((a, b) => a.path.localeCompare(b.path));
    return { entries, tree_hash: sha256(stableJson(entries)), byte_count: entries.reduce((total, entry) => total + entry.bytes, 0) };
  }

  private copyTree(source: string, destination: string): void {
    fs.mkdirSync(destination, { recursive: true });
    for (const item of fs.readdirSync(source, { withFileTypes: true })) {
      const from = path.join(source, item.name);
      const to = path.join(destination, item.name);
      if (item.isSymbolicLink()) throw serviceError(400, "symlink_not_allowed", "Workspace snapshots cannot contain symbolic links.");
      if (item.isDirectory()) this.copyTree(from, to);
      else if (item.isFile()) fs.copyFileSync(from, to);
    }
  }

  private collectFiles(root: string, directory: string, output: string[], maxFiles: number): void {
    if (output.length >= maxFiles) return;
    for (const item of fs.readdirSync(directory, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
      if (output.length >= maxFiles) return;
      const full = path.join(directory, item.name);
      if (item.isSymbolicLink()) throw serviceError(400, "symlink_not_allowed", "Context packages cannot follow symbolic links.");
      if (item.isDirectory()) this.collectFiles(root, full, output, maxFiles);
      else if (item.isFile()) output.push(path.relative(root, full).split(path.sep).join("/"));
    }
  }

  private removeDirectoryIfSafe(target: string, root: string): void {
    const resolved = path.resolve(target);
    const resolvedRoot = path.resolve(root);
    if (!withinPath(resolved, resolvedRoot) || resolved === resolvedRoot) throw serviceError(500, "unsafe_storage_operation", "The requested storage operation is outside its scoped directory.");
    fs.rmSync(resolved, { recursive: true, force: true });
  }

  private authoritativeStatus(repositoryPath: string): string {
    try {
      return execFileSync("git", ["-C", repositoryPath, "status", "--short", "--untracked-files=all"], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }).trim();
    } catch {
      return "unavailable";
    }
  }

  private runPython(cwd: string, args: string[]): { command: string; returncode: number; output: string } {
    const command = `python ${args.join(" ")}`;
    try {
      const output = execFileSync("python", args, { cwd, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"], maxBuffer: 2 * 1024 * 1024, timeout: 120000 });
      return { command, returncode: 0, output: output.trim() };
    } catch (error) {
      const item = error as { status?: number; stdout?: string; stderr?: string };
      return { command, returncode: Number(item.status || 1), output: `${String(item.stdout || "")}\n${String(item.stderr || "")}`.trim() };
    }
  }
}

export const workspaceService = new WorkspaceService();
