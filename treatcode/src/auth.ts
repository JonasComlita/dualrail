import { appendFileSync, existsSync, mkdirSync, readFileSync, renameSync, unlinkSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { createHash, randomBytes, scryptSync, timingSafeEqual } from "node:crypto";

export const AUTH_API_SCHEMA_VERSION = "treatcode.auth.api.v1" as const;
export const AUTH_AUDIT_SCHEMA_VERSION = "treatcode.auth.audit.v1" as const;
export const AUTH_STATE_SCHEMA_VERSION = "treatcode.auth.state.v1" as const;
export const AUTH_POLICY_VERSION = "treatcode.authz.policy.v1" as const;
export const DEFAULT_PROJECT_ID = "tc:project:trit";

export const PERMISSION_ACTIONS = [
  "read",
  "workspace",
  "edit",
  "test",
  "benchmark",
  "artifact",
  "branch",
  "commit",
  "push",
  "draft_pr",
  "review",
  "merge",
] as const;

export type PermissionAction = (typeof PERMISSION_ACTIONS)[number];
export type IdentityKind = "human" | "collaborator" | "service" | "agent";

/** Actions granted to pseudonymous community participants. Keep this list deliberately narrow. */
export const PARTICIPANT_PERMISSION_ACTIONS = ["read", "test", "benchmark", "artifact"] as const;
export type ParticipantPermissionAction = (typeof PARTICIPANT_PERMISSION_ACTIONS)[number];

export const PARTICIPANT_HANDLE_MIN_LENGTH = 3;
export const PARTICIPANT_HANDLE_MAX_LENGTH = 24;
export const PARTICIPANT_PASSWORD_MIN_LENGTH = 12;
export const PARTICIPANT_PASSWORD_MAX_LENGTH = 128;
const PARTICIPANT_HANDLE_PATTERN = /^[A-Za-z0-9][A-Za-z0-9_-]{2,23}$/;

const MUTATING_ACTIONS = new Set<PermissionAction>([
  "workspace",
  "edit",
  "test",
  "benchmark",
  "artifact",
  "branch",
  "commit",
  "push",
  "draft_pr",
  "review",
  "merge",
]);
const PROJECT_ID_PATTERN = /^tc:project:[a-z0-9][a-z0-9-]*$/;
const TASK_ID_PATTERN = /^tc:task:[a-z0-9][a-z0-9-]*$/;
const IDENTITY_ID_PATTERN = /^tc:identity:[a-z0-9][a-z0-9-]*$/;
const CREDENTIAL_ID_PATTERN = /^tc:credential:[a-f0-9]{24}$/;
const MIN_NONCE_LENGTH = 4;
const DEFAULT_SESSION_TTL_SECONDS = 15 * 60;
const DEFAULT_TASK_TTL_SECONDS = 15 * 60;
const MAX_TASK_TTL_SECONDS = 60 * 60;
const GENESIS_HASH = "0".repeat(64);

const ACTION_DESCRIPTIONS: Record<PermissionAction, string> = {
  read: "Read public and authorized project information.",
  workspace: "Create, inspect, or hand off a scoped workspace.",
  edit: "Change files in the authorized workspace.",
  test: "Run tests or a bounded compiler task.",
  benchmark: "Run or record a benchmark.",
  artifact: "Create, inspect, or publish a task artifact.",
  branch: "Create or update a branch reference.",
  commit: "Create a commit in the authorized workspace.",
  push: "Push an already reviewed branch to its remote.",
  draft_pr: "Create or update a draft pull request.",
  review: "Read or record a review decision.",
  merge: "Merge an approved change; never implied by another action.",
};

export interface PermissionGrant {
  project_id: string;
  task_id: string | null;
  actions: PermissionAction[];
}

export interface IdentityView {
  id: string;
  kind: IdentityKind;
  display_name: string;
  /** Pseudonymous participant handle, when this identity has one. */
  handle?: string;
  roles: string[];
  status: "active" | "revoked";
}

export interface CredentialView {
  credential_id: string;
  token_type: "session" | "task";
  identity_id: string;
  project_id: string;
  task_id: string | null;
  actions: PermissionAction[];
  issued_at: string;
  expires_at: string;
  revoked_at?: string;
}

export interface IssuedCredential {
  token: string;
  credential: CredentialView;
}

export interface AuditEvent {
  schema_version: typeof AUTH_AUDIT_SCHEMA_VERSION;
  event_id: string;
  sequence: number;
  occurred_at: string;
  event_type: "authentication" | "authorization" | "credential" | "audit";
  outcome: "allowed" | "denied";
  actor_id: string;
  credential_id: string | null;
  project_id: string | null;
  task_id: string | null;
  action: PermissionAction | "login" | "issue_credential" | "revoke_credential" | "read_audit";
  reason_code: string | null;
  request_nonce_hash: string | null;
  previous_hash: string;
  record_hash: string;
}

export interface AuthorizationRequest {
  token?: string | null;
  action: PermissionAction;
  project_id: string;
  task_id?: string | null;
  request_nonce?: string | null;
  resource?: string;
  require_nonce?: boolean;
}

export interface AuthorizationDenial {
  schema_version: typeof AUTH_API_SCHEMA_VERSION;
  code:
    | "auth_required"
    | "invalid_token"
    | "token_expired"
    | "token_revoked"
    | "identity_revoked"
    | "invalid_scope"
    | "project_scope_denied"
    | "task_scope_denied"
    | "action_not_granted"
    | "nonce_required"
    | "nonce_invalid"
    | "replay_detected"
    | "invalid_credentials"
    | "agent_requires_task_credential"
    | "credential_not_found"
    | "target_scope_not_granted"
    | "audit_unavailable";
  reason: string;
  action: PermissionAction | "login" | "issue_credential" | "revoke_credential";
  project_id: string | null;
  task_id: string | null;
  audit_event_id: string | null;
  retryable: boolean;
  http_status: number;
}

export type AuthorizationResult =
  | {
      allowed: true;
      actor: IdentityView;
      credential: CredentialView;
    }
  | {
      allowed: false;
      denial: AuthorizationDenial;
    };

interface StoredIdentity extends IdentityView {
  access_key_salt: Buffer;
  access_key_hash: Buffer;
  grants: PermissionGrant[];
  interactive_login: boolean;
}

interface PersistedIdentity {
  id: string;
  kind: IdentityKind;
  display_name: string;
  handle?: string;
  roles: string[];
  status: "active" | "revoked";
  access_key_salt: string;
  access_key_hash: string;
  grants: PermissionGrant[];
  interactive_login: boolean;
}

interface PersistedAuthState {
  schema_version: typeof AUTH_STATE_SCHEMA_VERSION;
  updated_at: string;
  identities: PersistedIdentity[];
}

interface StoredCredential extends CredentialView {
  token_hash: string;
  expires_at_ms: number;
  revoked_at_ms?: number;
  used_nonce_hashes: Set<string>;
}

interface AuditFields {
  event_type: AuditEvent["event_type"];
  outcome: AuditEvent["outcome"];
  actor_id: string;
  credential_id?: string | null;
  project_id?: string | null;
  task_id?: string | null;
  action: AuditEvent["action"];
  reason_code?: string | null;
  request_nonce_hash?: string | null;
  occurred_at: string;
}

export interface AuthStoreOptions {
  now?: () => number;
  audit_path?: string | null;
  /** Durable identity state. `null` explicitly disables persistence. */
  identity_path?: string | null;
  /** Backwards/embedding aliases for the durable identity state path. */
  identity_store_path?: string | null;
  auth_state_path?: string | null;
  state_path?: string | null;
  session_ttl_seconds?: number;
  task_ttl_seconds?: number;
}

export interface ParticipantLoginInput {
  handle: string;
  password: string;
}

function canonicalJson(value: unknown): string {
  if (value === null || typeof value !== "object") return JSON.stringify(value) ?? "null";
  if (Array.isArray(value)) return `[${value.map((item) => canonicalJson(item)).join(",")}]`;
  return `{${Object.keys(value as Record<string, unknown>)
    .sort()
    .map((key) => `${JSON.stringify(key)}:${canonicalJson((value as Record<string, unknown>)[key])}`)
    .join(",")}}`;
}

function sha256(value: string): string {
  return createHash("sha256").update(value, "utf8").digest("hex");
}

function newId(prefix: string, bytes = 12): string {
  return `${prefix}${randomBytes(bytes).toString("hex")}`;
}

function newToken(): string {
  return `tc1.${randomBytes(32).toString("base64url")}`;
}

function clampTtl(value: number | undefined, fallback: number): number {
  if (!Number.isFinite(value)) return fallback;
  return Math.max(30, Math.min(MAX_TASK_TTL_SECONDS, Math.floor(value as number)));
}

function isPermissionAction(value: string): value is PermissionAction {
  return (PERMISSION_ACTIONS as readonly string[]).includes(value);
}

function isValidProjectId(value: string): boolean {
  return PROJECT_ID_PATTERN.test(value);
}

function isValidTaskId(value: string | null | undefined): value is string {
  return typeof value === "string" && TASK_ID_PATTERN.test(value);
}

function publicCredential(credential: StoredCredential): CredentialView {
  return {
    credential_id: credential.credential_id,
    token_type: credential.token_type,
    identity_id: credential.identity_id,
    project_id: credential.project_id,
    task_id: credential.task_id,
    actions: [...credential.actions],
    issued_at: credential.issued_at,
    expires_at: credential.expires_at,
    ...(credential.revoked_at ? { revoked_at: credential.revoked_at } : {}),
  };
}

function publicIdentity(identity: StoredIdentity): IdentityView {
  return {
    id: identity.id,
    kind: identity.kind,
    display_name: identity.display_name,
    ...(identity.handle ? { handle: identity.handle } : {}),
    roles: [...identity.roles],
    status: identity.status,
  };
}

function safeTimingEqual(left: Buffer, right: Buffer): boolean {
  return left.length === right.length && timingSafeEqual(left, right);
}

function defaultIdentityPath(): string {
  return process.env.TREATCODE_AUTH_IDENTITY_PATH || resolve("build/treatcode-auth/identities.json");
}

function normalizeHandleForLookup(handle: string): string {
  return handle.normalize("NFKC").toLocaleLowerCase("en-US");
}

function validateParticipantHandle(handle: unknown): string {
  if (typeof handle !== "string") throw new Error("handle_required");
  if (handle.length < PARTICIPANT_HANDLE_MIN_LENGTH || handle.length > PARTICIPANT_HANDLE_MAX_LENGTH || !PARTICIPANT_HANDLE_PATTERN.test(handle)) {
    throw new Error("invalid_handle");
  }
  // Reject visually/semantically ambiguous variants instead of silently changing
  // the account name the caller asked to register.
  const normalized = handle.normalize("NFKC");
  if (normalized !== handle || normalizeHandleForLookup(handle) !== handle.toLowerCase()) throw new Error("invalid_handle");
  return handle;
}

function validateParticipantPassword(password: unknown): string {
  if (typeof password !== "string") throw new Error("password_required");
  if (password.length < PARTICIPANT_PASSWORD_MIN_LENGTH || password.length > PARTICIPANT_PASSWORD_MAX_LENGTH) {
    throw new Error("invalid_password_length");
  }
  if (/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(password)) throw new Error("invalid_password");
  return password;
}

export function isValidParticipantHandle(handle: unknown): handle is string {
  try {
    validateParticipantHandle(handle);
    return true;
  } catch {
    return false;
  }
}

export function isValidParticipantPassword(password: unknown): password is string {
  try {
    validateParticipantPassword(password);
    return true;
  } catch {
    return false;
  }
}

function persistedIdentity(identity: StoredIdentity): PersistedIdentity {
  return {
    id: identity.id,
    kind: identity.kind,
    display_name: identity.display_name,
    ...(identity.handle ? { handle: identity.handle } : {}),
    roles: [...identity.roles],
    status: identity.status,
    access_key_salt: identity.access_key_salt.toString("base64").replace(/=+$/g, ""),
    access_key_hash: identity.access_key_hash.toString("base64").replace(/=+$/g, ""),
    grants: identity.grants.map((grant) => ({
      project_id: grant.project_id,
      task_id: grant.task_id,
      actions: [...grant.actions],
    })),
    interactive_login: identity.interactive_login,
  };
}

function restoredBuffer(value: unknown, expectedLength: number, field: string): Buffer {
  if (typeof value !== "string" || !value) throw new Error(`auth_state_invalid:${field}`);
  const decoded = Buffer.from(value, "base64");
  if (decoded.length !== expectedLength) throw new Error(`auth_state_invalid:${field}`);
  return decoded;
}

export class AuditLog {
  private readonly file_path: string | null;
  private readonly events: AuditEvent[] = [];
  private last_hash = GENESIS_HASH;

  constructor(filePath?: string | null) {
    this.file_path = filePath === null ? null : resolve(filePath || "build/treatcode-auth/audit.jsonl");
    this.load();
  }

  private load(): void {
    if (!this.file_path || !existsSync(this.file_path)) return;
    const lines = readFileSync(this.file_path, "utf8").split(/\r?\n/).filter(Boolean);
    for (const line of lines) {
      const parsed = JSON.parse(line) as AuditEvent;
      if (!this.verifyEvent(parsed, this.last_hash) || parsed.sequence !== this.events.length + 1) {
        throw new Error("audit_chain_invalid");
      }
      this.events.push(Object.freeze(parsed));
      this.last_hash = parsed.record_hash;
    }
  }

  private verifyEvent(event: AuditEvent, previousHash: string): boolean {
    if (event.previous_hash !== previousHash || !event.record_hash) return false;
    const { record_hash: _recordHash, ...payload } = event;
    return sha256(canonicalJson(payload)) === event.record_hash;
  }

  append(fields: AuditFields): AuditEvent {
    const payload: Omit<AuditEvent, "record_hash"> = {
      schema_version: AUTH_AUDIT_SCHEMA_VERSION,
      event_id: newId("tc:audit:"),
      sequence: this.events.length + 1,
      occurred_at: fields.occurred_at,
      event_type: fields.event_type,
      outcome: fields.outcome,
      actor_id: fields.actor_id,
      credential_id: fields.credential_id ?? null,
      project_id: fields.project_id ?? null,
      task_id: fields.task_id ?? null,
      action: fields.action,
      reason_code: fields.reason_code ?? null,
      request_nonce_hash: fields.request_nonce_hash ?? null,
      previous_hash: this.last_hash,
    };
    const event = Object.freeze({ ...payload, record_hash: sha256(canonicalJson(payload)) });
    if (this.file_path) {
      mkdirSync(dirname(this.file_path), { recursive: true });
      appendFileSync(this.file_path, `${JSON.stringify(event)}\n`, "utf8");
    }
    this.events.push(event);
    this.last_hash = event.record_hash;
    return event;
  }

  list(): AuditEvent[] {
    return this.events.map((event) => ({ ...event }));
  }

  verify(): { ok: boolean; count: number; last_hash: string } {
    let previous = GENESIS_HASH;
    for (let index = 0; index < this.events.length; index += 1) {
      const event = this.events[index];
      if (event.sequence !== index + 1 || !this.verifyEvent(event, previous)) {
        return { ok: false, count: index, last_hash: previous };
      }
      previous = event.record_hash;
    }
    return { ok: true, count: this.events.length, last_hash: previous };
  }
}

export class AuthStore {
  readonly audit: AuditLog;
  private readonly now: () => number;
  private readonly identities = new Map<string, StoredIdentity>();
  private readonly credentials = new Map<string, StoredCredential>();
  private readonly identity_path: string | null;
  private readonly session_ttl_seconds: number;
  private readonly task_ttl_seconds: number;

  constructor(options: AuthStoreOptions = {}) {
    this.now = options.now || (() => Date.now());
    this.audit = new AuditLog(options.audit_path);
    const configuredIdentityPath = options.identity_path !== undefined
      ? options.identity_path
      : options.identity_store_path !== undefined
        ? options.identity_store_path
        : options.auth_state_path !== undefined
          ? options.auth_state_path
          : options.state_path !== undefined
            ? options.state_path
            : defaultIdentityPath();
    this.identity_path = configuredIdentityPath === null ? null : resolve(configuredIdentityPath || defaultIdentityPath());
    this.loadIdentities();
    this.session_ttl_seconds = Math.max(30, Math.floor(options.session_ttl_seconds || DEFAULT_SESSION_TTL_SECONDS));
    this.task_ttl_seconds = clampTtl(options.task_ttl_seconds, DEFAULT_TASK_TTL_SECONDS);
  }

  private loadIdentities(): void {
    if (!this.identity_path || !existsSync(this.identity_path)) return;
    let parsed: unknown;
    try {
      parsed = JSON.parse(readFileSync(this.identity_path, "utf8")) as unknown;
    } catch {
      throw new Error("auth_state_invalid:json");
    }
    if (!parsed || typeof parsed !== "object") throw new Error("auth_state_invalid:root");
    const state = parsed as Partial<PersistedAuthState>;
    if (state.schema_version !== AUTH_STATE_SCHEMA_VERSION || !Array.isArray(state.identities)) {
      throw new Error("auth_state_invalid:schema");
    }
    for (const record of state.identities) {
      if (!record || typeof record !== "object") throw new Error("auth_state_invalid:identity");
      if (typeof record.id !== "string" || !IDENTITY_ID_PATTERN.test(record.id)) throw new Error("auth_state_invalid:identity_id");
      if (!["human", "collaborator", "service", "agent"].includes(record.kind)) throw new Error("auth_state_invalid:identity_kind");
      if (typeof record.display_name !== "string" || !Array.isArray(record.roles) || !Array.isArray(record.grants)) {
        throw new Error("auth_state_invalid:identity_fields");
      }
      const handle = record.handle === undefined ? undefined : validateParticipantHandle(record.handle);
      const grants: PermissionGrant[] = record.grants.map((grant) => {
        if (!grant || typeof grant !== "object" || typeof grant.project_id !== "string" || !isValidProjectId(grant.project_id) && grant.project_id !== "*") {
          throw new Error("auth_state_invalid:grant");
        }
        const taskId = grant.task_id === null || grant.task_id === undefined ? null : grant.task_id;
        if (taskId !== null && !isValidTaskId(taskId)) throw new Error("auth_state_invalid:grant_task");
        if (!Array.isArray(grant.actions)) throw new Error("auth_state_invalid:grant_actions");
        const actions = [...new Set(grant.actions)].filter((action): action is PermissionAction => typeof action === "string" && isPermissionAction(action));
        if (actions.length !== grant.actions.length) throw new Error("auth_state_invalid:grant_actions");
        return { project_id: grant.project_id, task_id: taskId, actions };
      });
      const salt = restoredBuffer(record.access_key_salt, 16, "access_key_salt");
      const hash = restoredBuffer(record.access_key_hash, 32, "access_key_hash");
      const restored: StoredIdentity = {
        id: record.id,
        kind: record.kind,
        display_name: record.display_name,
        ...(handle ? { handle } : {}),
        roles: [...new Set(record.roles.filter((role): role is string => typeof role === "string"))],
        status: record.status === "revoked" ? "revoked" : record.status === "active" ? "active" : (() => { throw new Error("auth_state_invalid:identity_status"); })(),
        access_key_salt: salt,
        access_key_hash: hash,
        grants,
        interactive_login: record.interactive_login !== false,
      };
      const existingHandle = restored.handle && this.identityForHandle(restored.handle);
      if (existingHandle && existingHandle.id !== restored.id) throw new Error("auth_state_invalid:duplicate_handle");
      this.identities.set(restored.id, restored);
    }
  }

  private persistIdentities(): void {
    if (!this.identity_path) return;
    const state: PersistedAuthState = {
      schema_version: AUTH_STATE_SCHEMA_VERSION,
      updated_at: new Date(this.now()).toISOString(),
      identities: [...this.identities.values()].map(persistedIdentity),
    };
    const temporaryPath = `${this.identity_path}.${process.pid}.${randomBytes(8).toString("hex")}.tmp`;
    mkdirSync(dirname(this.identity_path), { recursive: true });
    try {
      writeFileSync(temporaryPath, `${JSON.stringify(state)}\n`, { encoding: "utf8", mode: 0o600 });
      renameSync(temporaryPath, this.identity_path);
    } catch (error) {
      try { if (existsSync(temporaryPath)) unlinkSync(temporaryPath); } catch { /* Preserve the original persistence failure. */ }
      throw error;
    }
  }

  private identityForHandle(handle: string): StoredIdentity | undefined {
    const normalized = normalizeHandleForLookup(handle);
    return [...this.identities.values()].find((identity) => identity.handle && normalizeHandleForLookup(identity.handle) === normalized);
  }

  participant(handle: string): IdentityView | null {
    const identity = this.identityForHandle(handle);
    return identity?.roles.includes("participant") ? publicIdentity(identity) : null;
  }

  identityByHandle(handle: string): IdentityView | null {
    const identity = this.identityForHandle(handle);
    return identity ? publicIdentity(identity) : null;
  }

  registerParticipant(input: { handle: string; password: string; display_name?: string }): IdentityView {
    if (!input || typeof input !== "object") throw new Error("registration_invalid");
    const handle = validateParticipantHandle(input.handle);
    const password = validateParticipantPassword(input?.password);
    if (this.identityForHandle(handle)) throw new Error("duplicate_handle");
    if (password === handle || password.toLocaleLowerCase("en-US") === handle.toLocaleLowerCase("en-US")) throw new Error("password_must_differ_from_handle");
    if (input.display_name !== undefined && typeof input.display_name !== "string") throw new Error("invalid_display_name");
    const displayName = input.display_name === undefined ? handle : input.display_name;
    if (!displayName.trim() || displayName.length > 80 || /[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(displayName)) throw new Error("invalid_display_name");
    if (displayName.includes(password)) throw new Error("invalid_display_name");
    const identity = this.registerIdentity({
      id: newId("tc:identity:participant-"),
      // Keep the established identity-kind union/API intact. The `participant`
      // role plus handle identifies this least-privilege account class.
      kind: "human",
      handle,
      display_name: displayName,
      roles: ["participant"],
      access_key: password,
      grants: [{ project_id: "*", task_id: null, actions: [...PARTICIPANT_PERMISSION_ACTIONS] }],
      interactive_login: true,
    });
    return identity;
  }

  /** Alias used by account-oriented callers. */
  registerAccount(input: { handle: string; password: string; display_name?: string }): IdentityView {
    return this.registerParticipant(input);
  }

  createParticipant(input: { handle: string; password: string; display_name?: string }): IdentityView {
    return this.registerParticipant(input);
  }

  registerAndLoginParticipant(input: ParticipantLoginInput & { display_name?: string }): { identity: IdentityView; credential: IssuedCredential } {
    const identity = this.registerParticipant(input);
    const login = this.login(input);
    if (!login.ok) throw new Error("participant_registration_login_failed");
    return { identity, credential: login.credential };
  }

  loginParticipant(handle: string, password: string): { ok: true; identity: IdentityView; credential: IssuedCredential } | { ok: false; denial: AuthorizationDenial } {
    return this.login(handle, password);
  }

  loginAccount(input: ParticipantLoginInput): { ok: true; identity: IdentityView; credential: IssuedCredential } | { ok: false; denial: AuthorizationDenial } {
    return this.login(input);
  }

  registerIdentity(input: {
    id: string;
    kind: IdentityKind;
    display_name: string;
    handle?: string;
    roles?: string[];
    access_key: string;
    grants: PermissionGrant[];
    interactive_login?: boolean;
  }): IdentityView {
    if (!IDENTITY_ID_PATTERN.test(input.id)) throw new Error(`invalid_identity_id:${input.id}`);
    if (!input.access_key || input.access_key.length < 8) throw new Error("access_key_too_short");
    const handle = input.handle === undefined ? undefined : validateParticipantHandle(input.handle);
    const existingHandle = handle && this.identityForHandle(handle);
    if (existingHandle && existingHandle.id !== input.id) throw new Error("duplicate_handle");
    const salt = randomBytes(16);
    const normalizedGrants = input.grants.map((grant) => ({
      project_id: grant.project_id,
      task_id: grant.task_id ?? null,
      actions: [...new Set(grant.actions)].filter(isPermissionAction),
    }));
    const stored: StoredIdentity = {
      id: input.id,
      kind: input.kind,
      display_name: input.display_name,
      ...(handle ? { handle } : {}),
      roles: [...new Set(input.roles || [])],
      status: "active",
      access_key_salt: salt,
      access_key_hash: scryptSync(input.access_key, salt, 32),
      grants: normalizedGrants,
      interactive_login: input.interactive_login ?? input.kind !== "agent",
    };
    this.identities.set(stored.id, stored);
    this.persistIdentities();
    return publicIdentity(stored);
  }

  revokeIdentity(identityId: string): boolean {
    const identity = this.identities.get(identityId);
    if (!identity) return false;
    identity.status = "revoked";
    this.persistIdentities();
    return true;
  }

  identity(identityId: string): IdentityView | null {
    const identity = this.identities.get(identityId);
    return identity ? publicIdentity(identity) : null;
  }

  login(input: ParticipantLoginInput): { ok: true; identity: IdentityView; credential: IssuedCredential } | { ok: false; denial: AuthorizationDenial };
  login(identityIdOrHandle: string, accessKey: string): { ok: true; identity: IdentityView; credential: IssuedCredential } | { ok: false; denial: AuthorizationDenial };
  login(identityIdOrHandleOrInput: string | ParticipantLoginInput, suppliedAccessKey?: string): { ok: true; identity: IdentityView; credential: IssuedCredential } | { ok: false; denial: AuthorizationDenial } {
    // The identity-id/access-key contract remains the primary compatibility
    // path. Participant handles are an additional lookup key and never alter
    // the credential or authorization semantics.
    const identityIdOrHandle = typeof identityIdOrHandleOrInput === "string" ? identityIdOrHandleOrInput : identityIdOrHandleOrInput.handle;
    const accessKey = typeof identityIdOrHandleOrInput === "string" ? suppliedAccessKey || "" : identityIdOrHandleOrInput.password;
    const identity = this.identities.get(identityIdOrHandle) || this.identityForHandle(identityIdOrHandle);
    if (!identity || identity.status !== "active" || !identity.interactive_login || !this.verifyAccessKey(identity, accessKey)) {
      const denial = this.recordDenial({
        code: "invalid_credentials",
        reason: "The supplied credentials are not valid.",
        action: "login",
        project_id: null,
        task_id: null,
        actor_id: "tc:identity:anonymous",
        credential_id: null,
        http_status: 401,
        retryable: false,
      });
      return { ok: false, denial };
    }

    const actions = this.grantedActions(identity, "*", null);
    const credential = this.mintCredential(identity, "session", "*", null, [...actions], this.session_ttl_seconds);
    this.audit.append({
      event_type: "authentication",
      outcome: "allowed",
      actor_id: identity.id,
      credential_id: credential.credential.credential_id,
      project_id: null,
      task_id: null,
      action: "login",
      occurred_at: this.timestamp(),
    });
    return { ok: true, identity: publicIdentity(identity), credential };
  }

  issueTaskCredential(input: {
    requester_token?: string | null;
    target_identity_id: string;
    project_id: string;
    task_id: string;
    actions: PermissionAction[];
    ttl_seconds?: number;
    request_nonce?: string | null;
  }): { ok: true; identity: IdentityView; credential: IssuedCredential } | { ok: false; denial: AuthorizationDenial } {
    const requester = this.authorize({
      token: input.requester_token,
      action: "workspace",
      project_id: input.project_id,
      task_id: input.task_id,
      request_nonce: input.request_nonce,
      resource: "credential",
    });
    if (!requester.allowed) return { ok: false, denial: requester.denial };

    const target = this.identities.get(input.target_identity_id);
    const requested = [...new Set(input.actions)].filter(isPermissionAction);
    if (!target || target.status !== "active" || target.kind !== "agent") {
      return {
        ok: false,
        denial: this.recordDenial({
          code: "target_scope_not_granted",
          reason: "Task credentials can only target an active agent identity.",
          action: "issue_credential",
          project_id: input.project_id,
          task_id: input.task_id,
          actor_id: requester.actor.id,
          credential_id: requester.credential.credential_id,
          http_status: 403,
          retryable: false,
        }),
      };
    }
    if (!isValidTaskId(input.task_id) || !isValidProjectId(input.project_id) || requested.length === 0) {
      return {
        ok: false,
        denial: this.recordDenial({
          code: "invalid_scope",
          reason: "Project, task, and at least one allowed action are required.",
          action: "issue_credential",
          project_id: input.project_id,
          task_id: input.task_id,
          actor_id: requester.actor.id,
          credential_id: requester.credential.credential_id,
          http_status: 400,
          retryable: false,
        }),
      };
    }

    const targetActions = this.grantedActions(target, input.project_id, input.task_id);
    if (requested.some((action) => !targetActions.has(action))) {
      return {
        ok: false,
        denial: this.recordDenial({
          code: "target_scope_not_granted",
          reason: "The requested task actions exceed the target agent policy.",
          action: "issue_credential",
          project_id: input.project_id,
          task_id: input.task_id,
          actor_id: requester.actor.id,
          credential_id: requester.credential.credential_id,
          http_status: 403,
          retryable: false,
        }),
      };
    }

    const credential = this.mintCredential(target, "task", input.project_id, input.task_id, requested, clampTtl(input.ttl_seconds, this.task_ttl_seconds));
    this.audit.append({
      event_type: "credential",
      outcome: "allowed",
      actor_id: requester.actor.id,
      credential_id: credential.credential.credential_id,
      project_id: input.project_id,
      task_id: input.task_id,
      action: "issue_credential",
      occurred_at: this.timestamp(),
    });
    return { ok: true, identity: publicIdentity(target), credential };
  }

  revokeCredential(input: {
    requester_token?: string | null;
    credential_id: string;
    project_id: string;
    task_id?: string | null;
    request_nonce?: string | null;
  }): { ok: true; credential: CredentialView } | { ok: false; denial: AuthorizationDenial } {
    const requester = this.authorize({
      token: input.requester_token,
      action: "workspace",
      project_id: input.project_id,
      task_id: input.task_id,
      request_nonce: input.request_nonce,
      resource: "credential",
    });
    if (!requester.allowed) return { ok: false, denial: requester.denial };

    const credential = [...this.credentials.values()].find((candidate) => candidate.credential_id === input.credential_id);
    if (!credential || !CREDENTIAL_ID_PATTERN.test(input.credential_id)) {
      return {
        ok: false,
        denial: this.recordDenial({
          code: "credential_not_found",
          reason: "The requested credential does not exist.",
          action: "revoke_credential",
          project_id: input.project_id,
          task_id: input.task_id ?? null,
          actor_id: requester.actor.id,
          credential_id: requester.credential.credential_id,
          http_status: 404,
          retryable: false,
        }),
      };
    }
    if (credential.project_id !== input.project_id || (credential.task_id && credential.task_id !== input.task_id)) {
      return {
        ok: false,
        denial: this.recordDenial({
          code: "project_scope_denied",
          reason: "The credential is outside the requester project scope.",
          action: "revoke_credential",
          project_id: input.project_id,
          task_id: input.task_id ?? null,
          actor_id: requester.actor.id,
          credential_id: requester.credential.credential_id,
          http_status: 403,
          retryable: false,
        }),
      };
    }
    credential.revoked_at_ms = this.now();
    credential.revoked_at = this.timestamp(credential.revoked_at_ms);
    this.audit.append({
      event_type: "credential",
      outcome: "allowed",
      actor_id: requester.actor.id,
      credential_id: credential.credential_id,
      project_id: credential.project_id,
      task_id: credential.task_id,
      action: "revoke_credential",
      occurred_at: this.timestamp(),
    });
    return { ok: true, credential: publicCredential(credential) };
  }

  authorize(request: AuthorizationRequest): AuthorizationResult {
    const projectId = request.project_id;
    const taskId = request.task_id ?? null;
    if (!isPermissionAction(request.action) || !isValidProjectId(projectId) || (taskId !== null && !isValidTaskId(taskId))) {
      return {
        allowed: false,
        denial: this.recordDenial({
          code: "invalid_scope",
          reason: "The project, task, and action scope is invalid.",
          action: request.action,
          project_id: isValidProjectId(projectId) ? projectId : null,
          task_id: isValidTaskId(taskId) ? taskId : null,
          actor_id: "tc:identity:anonymous",
          credential_id: null,
          http_status: 400,
          retryable: false,
          request_nonce: request.request_nonce,
        }),
      };
    }

    const rawToken = typeof request.token === "string" ? request.token.trim() : "";
    if (!rawToken) {
      return {
        allowed: false,
        denial: this.recordDenial({
          code: "auth_required",
          reason: "An authenticated credential is required for this action.",
          action: request.action,
          project_id: projectId,
          task_id: taskId,
          actor_id: "tc:identity:anonymous",
          credential_id: null,
          http_status: 401,
          retryable: false,
          request_nonce: request.request_nonce,
        }),
      };
    }

    const credential = this.credentials.get(sha256(rawToken));
    if (!credential) {
      return {
        allowed: false,
        denial: this.recordDenial({
          code: "invalid_token",
          reason: "The supplied credential is not valid.",
          action: request.action,
          project_id: projectId,
          task_id: taskId,
          actor_id: "tc:identity:anonymous",
          credential_id: null,
          http_status: 401,
          retryable: false,
          request_nonce: request.request_nonce,
        }),
      };
    }

    const identity = this.identities.get(credential.identity_id);
    if (!identity || identity.status !== "active") {
      return {
        allowed: false,
        denial: this.recordDenial({
          code: "identity_revoked",
          reason: "The identity attached to this credential is no longer active.",
          action: request.action,
          project_id: projectId,
          task_id: taskId,
          actor_id: credential.identity_id,
          credential_id: credential.credential_id,
          http_status: 401,
          retryable: false,
          request_nonce: request.request_nonce,
        }),
      };
    }
    const now = this.now();
    if (credential.revoked_at_ms) {
      return {
        allowed: false,
        denial: this.recordDenial({
          code: "token_revoked",
          reason: "This credential has been revoked.",
          action: request.action,
          project_id: projectId,
          task_id: taskId,
          actor_id: identity.id,
          credential_id: credential.credential_id,
          http_status: 401,
          retryable: false,
          request_nonce: request.request_nonce,
        }),
      };
    }
    if (now >= credential.expires_at_ms) {
      return {
        allowed: false,
        denial: this.recordDenial({
          code: "token_expired",
          reason: "This credential has expired; request a new task credential.",
          action: request.action,
          project_id: projectId,
          task_id: taskId,
          actor_id: identity.id,
          credential_id: credential.credential_id,
          http_status: 401,
          retryable: true,
          request_nonce: request.request_nonce,
        }),
      };
    }
    if (credential.project_id !== "*" && credential.project_id !== projectId) {
      return {
        allowed: false,
        denial: this.recordDenial({
          code: "project_scope_denied",
          reason: "This credential is bound to a different project.",
          action: request.action,
          project_id: projectId,
          task_id: taskId,
          actor_id: identity.id,
          credential_id: credential.credential_id,
          http_status: 403,
          retryable: false,
          request_nonce: request.request_nonce,
        }),
      };
    }
    if (credential.task_id !== null && credential.task_id !== taskId) {
      return {
        allowed: false,
        denial: this.recordDenial({
          code: "task_scope_denied",
          reason: "This credential is bound to a different task.",
          action: request.action,
          project_id: projectId,
          task_id: taskId,
          actor_id: identity.id,
          credential_id: credential.credential_id,
          http_status: 403,
          retryable: false,
          request_nonce: request.request_nonce,
        }),
      };
    }
    if (!credential.actions.includes(request.action)) {
      return {
        allowed: false,
        denial: this.recordDenial({
          code: "action_not_granted",
          reason: `The credential does not grant the independent '${request.action}' action.`,
          action: request.action,
          project_id: projectId,
          task_id: taskId,
          actor_id: identity.id,
          credential_id: credential.credential_id,
          http_status: 403,
          retryable: false,
          request_nonce: request.request_nonce,
        }),
      };
    }

    const requiresNonce = request.require_nonce ?? MUTATING_ACTIONS.has(request.action);
    let nonceHash: string | null = null;
    if (requiresNonce) {
      if (typeof request.request_nonce !== "string" || request.request_nonce.length < MIN_NONCE_LENGTH || request.request_nonce.length > 128) {
        return {
          allowed: false,
          denial: this.recordDenial({
            code: "nonce_required",
            reason: "Mutating actions require a fresh X-Action-Nonce value.",
            action: request.action,
            project_id: projectId,
            task_id: taskId,
            actor_id: identity.id,
            credential_id: credential.credential_id,
            http_status: 400,
            retryable: true,
            request_nonce: request.request_nonce,
          }),
        };
      }
      nonceHash = sha256(request.request_nonce);
      if (credential.used_nonce_hashes.has(nonceHash)) {
        return {
          allowed: false,
          denial: this.recordDenial({
            code: "replay_detected",
            reason: "This action nonce was already consumed by the credential.",
            action: request.action,
            project_id: projectId,
            task_id: taskId,
            actor_id: identity.id,
            credential_id: credential.credential_id,
            http_status: 409,
            retryable: false,
            request_nonce: request.request_nonce,
          }),
        };
      }
      credential.used_nonce_hashes.add(nonceHash);
    }

    this.audit.append({
      event_type: "authorization",
      outcome: "allowed",
      actor_id: identity.id,
      credential_id: credential.credential_id,
      project_id: projectId,
      task_id: taskId,
      action: request.action,
      request_nonce_hash: nonceHash,
      occurred_at: this.timestamp(),
    });
    return { allowed: true, actor: publicIdentity(identity), credential: publicCredential(credential) };
  }

  capabilities(input: { token?: string | null; project_id: string; task_id?: string | null }):
    | { ok: true; principal: IdentityView | null; credential: CredentialView | null; project_id: string; task_id: string | null; actions: PermissionAction[]; action_catalog: Array<{ action: PermissionAction; granted: boolean; description: string }> }
    | { ok: false; denial: AuthorizationDenial } {
    if (!input.token) {
      return {
        ok: true,
        principal: null,
        credential: null,
        project_id: input.project_id,
        task_id: input.task_id ?? null,
        actions: ["read"],
        action_catalog: PERMISSION_ACTIONS.map((action) => ({ action, granted: action === "read", description: ACTION_DESCRIPTIONS[action] })),
      };
    }
    const readDecision = this.authorize({
      token: input.token,
      action: "read",
      project_id: input.project_id,
      task_id: input.task_id,
      resource: "capabilities",
      require_nonce: false,
    });
    if (!readDecision.allowed) return { ok: false, denial: readDecision.denial };
    const actionSet = new Set(readDecision.credential.actions);
    return {
      ok: true,
      principal: readDecision.actor,
      credential: readDecision.credential,
      project_id: input.project_id,
      task_id: input.task_id ?? null,
      actions: [...readDecision.credential.actions],
      action_catalog: PERMISSION_ACTIONS.map((action) => ({ action, granted: actionSet.has(action), description: ACTION_DESCRIPTIONS[action] })),
    };
  }

  auditEvents(input: { token?: string | null; project_id: string; task_id?: string | null }): { ok: true; events: AuditEvent[] } | { ok: false; denial: AuthorizationDenial } {
    const decision = this.authorize({
      token: input.token,
      action: "review",
      project_id: input.project_id,
      task_id: input.task_id,
      resource: "audit",
      require_nonce: false,
    });
    if (!decision.allowed) return { ok: false, denial: decision.denial };
    this.audit.append({
      event_type: "audit",
      outcome: "allowed",
      actor_id: decision.actor.id,
      credential_id: decision.credential.credential_id,
      project_id: input.project_id,
      task_id: input.task_id ?? null,
      action: "read_audit",
      occurred_at: this.timestamp(),
    });
    return { ok: true, events: this.audit.list() };
  }

  verifyAudit(): { ok: boolean; count: number; last_hash: string } {
    return this.audit.verify();
  }

  private verifyAccessKey(identity: StoredIdentity, accessKey: string): boolean {
    if (!accessKey) return false;
    try {
      return safeTimingEqual(identity.access_key_hash, scryptSync(accessKey, identity.access_key_salt, 32));
    } catch {
      return false;
    }
  }

  private grantedActions(identity: StoredIdentity, projectId: string, taskId: string | null): Set<PermissionAction> {
    const actions = new Set<PermissionAction>();
    for (const grant of identity.grants) {
      const projectMatches = grant.project_id === "*" || grant.project_id === projectId;
      const taskMatches = grant.task_id === null || grant.task_id === taskId;
      if (projectMatches && taskMatches) for (const action of grant.actions) actions.add(action);
    }
    return actions;
  }

  private mintCredential(identity: StoredIdentity, tokenType: "session" | "task", projectId: string, taskId: string | null, actions: PermissionAction[], ttlSeconds: number): IssuedCredential {
    const token = newToken();
    const now = this.now();
    const credential: StoredCredential = {
      credential_id: newId("tc:credential:"),
      token_type: tokenType,
      identity_id: identity.id,
      project_id: projectId,
      task_id: taskId,
      actions: [...new Set(actions)],
      issued_at: this.timestamp(now),
      expires_at: this.timestamp(now + ttlSeconds * 1000),
      token_hash: sha256(token),
      expires_at_ms: now + ttlSeconds * 1000,
      used_nonce_hashes: new Set<string>(),
    };
    this.credentials.set(credential.token_hash, credential);
    return { token, credential: publicCredential(credential) };
  }

  private recordDenial(input: {
    code: AuthorizationDenial["code"];
    reason: string;
    action: AuthorizationDenial["action"];
    project_id: string | null;
    task_id: string | null;
    actor_id: string;
    credential_id: string | null;
    http_status: number;
    retryable: boolean;
    request_nonce?: string | null;
  }): AuthorizationDenial {
    let eventId: string | null = null;
    try {
      const event = this.audit.append({
        event_type: input.action === "login" ? "authentication" : "authorization",
        outcome: "denied",
        actor_id: input.actor_id,
        credential_id: input.credential_id,
        project_id: input.project_id,
        task_id: input.task_id,
        action: input.action,
        reason_code: input.code,
        request_nonce_hash: input.request_nonce ? sha256(input.request_nonce) : null,
        occurred_at: this.timestamp(),
      });
      eventId = event.event_id;
    } catch {
      return {
        schema_version: AUTH_API_SCHEMA_VERSION,
        code: "audit_unavailable",
        reason: "The security audit sink is unavailable; the action was denied.",
        action: input.action,
        project_id: input.project_id,
        task_id: input.task_id,
        audit_event_id: null,
        retryable: true,
        http_status: 503,
      };
    }
    return {
      schema_version: AUTH_API_SCHEMA_VERSION,
      code: input.code,
      reason: input.reason,
      action: input.action,
      project_id: input.project_id,
      task_id: input.task_id,
      audit_event_id: eventId,
      retryable: input.retryable,
      http_status: input.http_status,
    };
  }

  private timestamp(milliseconds = this.now()): string {
    return new Date(milliseconds).toISOString();
  }
}

export function actionCatalog(): Array<{ action: PermissionAction; description: string }> {
  return PERMISSION_ACTIONS.map((action) => ({ action, description: ACTION_DESCRIPTIONS[action] }));
}

export function createDefaultAuthStore(options: AuthStoreOptions = {}): AuthStore {
  const store = new AuthStore(options);
  const configuredOrDevelopmentKey = (name: string, developmentKey: string): string => {
    const configured = process.env[name];
    if (configured) return configured;
    return process.env.NODE_ENV === "production" ? randomBytes(32).toString("hex") : developmentKey;
  };
  const defaultKey = configuredOrDevelopmentKey("TREATCODE_AUTH_DEMO_KEY", "local-human-key");
  const projectGrant = (actions: PermissionAction[]): PermissionGrant => ({ project_id: "*", task_id: null, actions });
  const nonMergeActions = PERMISSION_ACTIONS.filter((action) => action !== "merge");

  store.registerIdentity({
    id: "tc:identity:demo-human",
    kind: "human",
    display_name: "Demo human",
    roles: ["member", "workspace-owner"],
    access_key: defaultKey,
    grants: [projectGrant(nonMergeActions)],
  });
  store.registerIdentity({
    id: "tc:identity:demo-collaborator",
    kind: "collaborator",
    display_name: "Demo collaborator",
    roles: ["collaborator"],
    access_key: configuredOrDevelopmentKey("TREATCODE_AUTH_COLLABORATOR_KEY", "local-collaborator-key"),
    grants: [projectGrant(["read", "workspace", "edit", "test", "benchmark", "artifact", "branch", "commit", "draft_pr", "review"])],
  });
  store.registerIdentity({
    id: "tc:identity:demo-service",
    kind: "service",
    display_name: "Demo build service",
    roles: ["automation"],
    access_key: configuredOrDevelopmentKey("TREATCODE_AUTH_SERVICE_KEY", "local-service-key"),
    grants: [projectGrant(["read", "test", "benchmark", "artifact"])],
  });
  store.registerIdentity({
    id: "tc:identity:demo-agent",
    kind: "agent",
    display_name: "Demo task agent",
    roles: ["agent"],
    access_key: configuredOrDevelopmentKey("TREATCODE_AUTH_AGENT_KEY", "local-agent-key"),
    interactive_login: false,
    grants: [projectGrant(nonMergeActions)],
  });
  return store;
}

export function bearerToken(authorizationHeader: string | undefined): string | null {
  if (!authorizationHeader) return null;
  const match = authorizationHeader.match(/^Bearer\s+(.+)$/i);
  return match ? match[1].trim() : null;
}

export function safeCredentialView(credential: CredentialView): CredentialView {
  return {
    ...credential,
    actions: [...credential.actions],
  };
}
