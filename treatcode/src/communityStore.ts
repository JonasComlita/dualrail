import { existsSync, mkdirSync, readFileSync, renameSync, unlinkSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { createHash, randomBytes } from "node:crypto";
import {
  type AuthorizationResult,
  type CredentialView,
  type IdentityView,
  type PermissionAction,
} from "./auth";

export const COMMUNITY_STATE_SCHEMA_VERSION = "treatcode.community.state.v1" as const;
export const COMMUNITY_API_SCHEMA_VERSION = "treatcode.community.api.v1" as const;

export const COMMUNITY_LIMITS = Object.freeze({
  solution_title: 160,
  solution_code: 256 * 1024,
  discussion_title: 160,
  discussion_body: 8 * 1024,
  submission_code: 256 * 1024,
  metadata: 16 * 1024,
  identifier: 120,
  list_limit: 100,
});

export type CommunityWriteAction = "artifact" | "test";
export type SolutionVisibility = "private" | "public";

export interface CommunityStoreOptions {
  /** Durable JSON state path. `null` explicitly selects process-local state. */
  state_path?: string | null;
  /** Alias accepted by embedding callers. */
  community_state_path?: string | null;
  path?: string | null;
  file_path?: string | null;
  storage_root?: string | null;
  storageRoot?: string | null;
  now?: () => number;
  limits?: Partial<typeof COMMUNITY_LIMITS>;
}

export interface CommunityActorContext {
  identity: IdentityView;
  credential?: CredentialView;
  actions?: readonly PermissionAction[];
}

export type CommunityActor = IdentityView | CommunityActorContext | AuthorizationResult;

export interface SavedSolutionRevision {
  schema_version: typeof COMMUNITY_API_SCHEMA_VERSION;
  id: string;
  solution_id: string;
  revision: number;
  problem_id: string;
  challenge_id: string;
  task_id: string;
  title: string;
  code: string;
  language: string;
  visibility: SolutionVisibility;
  owner_identity_id: string;
  owner_handle: string;
  metadata: Record<string, unknown>;
  created_at: string;
  updated_at: string;
}

export interface SaveSolutionRevisionInput {
  problem_id?: string;
  challenge_id?: string;
  task_id?: string;
  solution_id?: string;
  title?: string;
  code: string;
  language?: string;
  visibility?: SolutionVisibility;
  metadata?: Record<string, unknown>;
}

export interface SolutionPerformanceMetrics {
  runtime_ms: number | null;
  memory_kib: number | null;
  cycles: number | null;
  compile_cycles: number | null;
  tests_passed: number;
  tests_total: number;
  engine: string | null;
  opt_level: string | null;
}

export interface PublishSolutionInput extends SaveSolutionRevisionInput {
  explanation: string;
  pseudocode?: string;
}

export interface ChallengeSubmissionRecord {
  schema_version: typeof COMMUNITY_API_SCHEMA_VERSION;
  id: string;
  problem_id: string;
  challenge_id: string;
  task_id: string;
  solution_id: string | null;
  code: string;
  code_hash: string;
  language: string;
  engine: string | null;
  owner_identity_id: string;
  owner_handle: string;
  metadata: Record<string, unknown>;
  outcome: "accepted" | "rejected" | "pending" | "unknown";
  cycles: number | null;
  metrics: SolutionPerformanceMetrics;
  created_at: string;
}

export interface ChallengeSubmissionInput {
  problem_id?: string;
  challenge_id?: string;
  task_id?: string;
  solution_id?: string;
  code: string;
  language?: string;
  engine?: string;
  metadata?: Record<string, unknown>;
  outcome?: ChallengeSubmissionRecord["outcome"];
  accepted?: boolean;
  cycles?: number;
  metrics?: Partial<SolutionPerformanceMetrics>;
}

export interface DiscussionPost {
  schema_version: typeof COMMUNITY_API_SCHEMA_VERSION;
  id: string;
  problem_id: string;
  challenge_id: string;
  task_id: string;
  solution_id: string | null;
  parent_id: string | null;
  title: string | null;
  body: string;
  author_identity_id: string;
  author_handle: string;
  metadata: Record<string, unknown>;
  created_at: string;
  updated_at: string;
}

export interface AddDiscussionInput {
  problem_id?: string;
  challenge_id?: string;
  task_id?: string;
  solution_id?: string;
  parent_id?: string;
  title?: string;
  body: string;
  metadata?: Record<string, unknown>;
}

export interface SolutionVoteRecord {
  schema_version: typeof COMMUNITY_API_SCHEMA_VERSION;
  id: string;
  solution_id: string;
  voter_identity_id: string;
  created_at: string;
}

export interface PublicSolutionRecord extends SavedSolutionRevision {
  solved: boolean;
  upvotes: number;
  viewer_has_upvoted: boolean;
  discussion_body: string;
  metrics: SolutionPerformanceMetrics | null;
}

interface CommunityState {
  schema_version: typeof COMMUNITY_STATE_SCHEMA_VERSION;
  revision: number;
  updated_at: string;
  solution_revisions: SavedSolutionRevision[];
  submissions: ChallengeSubmissionRecord[];
  discussions: DiscussionPost[];
  solution_votes: SolutionVoteRecord[];
}

export class CommunityStoreError extends Error {
  readonly code: string;
  readonly status: number;

  constructor(code: string, message: string, status = 400) {
    super(message);
    this.name = "CommunityStoreError";
    this.code = code;
    this.status = status;
  }
}

const SECRET_KEY_PATTERN = /(password|passwd|secret|token|access[_-]?key|authorization|cookie|session)/i;
const CONTROL_CHARACTER_PATTERN = /[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f\u0080-\u009f]/g;
const IDENTIFIER_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._:-]{0,119}$/;

function defaultStatePath(): string {
  return process.env.TREATCODE_COMMUNITY_STATE_PATH || resolve("build/treatcode-community/state.json");
}

function clone<T>(value: T): T {
  return JSON.parse(JSON.stringify(value)) as T;
}

function newId(prefix: string): string {
  return `${prefix}${randomBytes(12).toString("hex")}`;
}

function sha256Text(value: string): string {
  return createHash("sha256").update(value, "utf8").digest("hex");
}

function plainText(value: unknown, field: string, maxLength: number, required = true, stripMarkup = false): string {
  if (typeof value !== "string") {
    if (!required && (value === undefined || value === null)) return "";
    throw new CommunityStoreError(`${field}_invalid`, `${field} must be plain text.`);
  }
  const normalized = value.normalize("NFKC").replace(/\r\n?/g, "\n").replace(CONTROL_CHARACTER_PATTERN, "");
  if (required && !normalized.trim()) throw new CommunityStoreError(`${field}_required`, `${field} is required.`);
  if (normalized.length > maxLength || Buffer.byteLength(normalized, "utf8") > maxLength) throw new CommunityStoreError(`${field}_too_large`, `${field} exceeds its size limit.`);
  // Treat all content as text. Strip only HTML tags that could otherwise be
  // accidentally interpreted by a caller that renders a community field as
  // markup; comparison operators and Trit generic syntax remain intact.
  return (stripMarkup ? normalized.replace(/<[^>]*>/g, "") : normalized.replace(/<\/?(?:script|style|iframe|object|embed|form)(?:\s[^>]*)?>/gi, "")).trim();
}

function identifier(value: unknown, field: string, required = true): string {
  if (value === undefined || value === null || value === "") {
    if (!required) return "";
    throw new CommunityStoreError(`${field}_required`, `${field} is required.`);
  }
  if (typeof value !== "string" || !IDENTIFIER_PATTERN.test(value)) throw new CommunityStoreError(`${field}_invalid`, `${field} is invalid.`);
  return value;
}

function finiteNumber(value: unknown, field: string, min = 0, max = Number.MAX_SAFE_INTEGER): number | null {
  if (value === undefined || value === null) return null;
  if (typeof value !== "number" || !Number.isFinite(value) || value < min || value > max) throw new CommunityStoreError(`${field}_invalid`, `${field} is invalid.`);
  return Math.floor(value);
}

function safeMetadata(value: unknown, maxBytes: number): Record<string, unknown> {
  if (value === undefined || value === null) return {};
  const seen = new WeakSet<object>();
  if (!value || typeof value !== "object" || Array.isArray(value)) throw new CommunityStoreError("metadata_invalid", "metadata must be a JSON object.");
  const scrub = (candidate: unknown, key = ""): unknown => {
    if (SECRET_KEY_PATTERN.test(key)) return undefined;
    if (candidate === null || typeof candidate === "string" || typeof candidate === "boolean") {
      return typeof candidate === "string" ? plainText(candidate, "metadata", maxBytes, false, true) : candidate;
    }
    if (typeof candidate === "number") return Number.isFinite(candidate) ? candidate : undefined;
    if (Array.isArray(candidate)) {
      if (seen.has(candidate)) return undefined;
      seen.add(candidate);
      return candidate.slice(0, 100).map((item) => scrub(item)).filter((item) => item !== undefined);
    }
    if (typeof candidate === "object") {
      if (seen.has(candidate)) return undefined;
      seen.add(candidate);
      const output: Record<string, unknown> = {};
      for (const [childKey, childValue] of Object.entries(candidate as Record<string, unknown>).slice(0, 100)) {
        const cleaned = scrub(childValue, childKey);
        if (cleaned !== undefined) output[childKey.slice(0, 80)] = cleaned;
      }
      return output;
    }
    return undefined;
  };
  const scrubbed = scrub(value);
  if (!scrubbed || typeof scrubbed !== "object" || Array.isArray(scrubbed)) throw new CommunityStoreError("metadata_invalid", "metadata must be a JSON object.");
  const json = JSON.stringify(scrubbed);
  if (Buffer.byteLength(json, "utf8") > maxBytes) throw new CommunityStoreError("metadata_too_large", "metadata exceeds its size limit.");
  return scrubbed as Record<string, unknown>;
}

function actorContext(actor: CommunityActor): CommunityActorContext {
  if (!actor) throw new CommunityStoreError("auth_required", "An authenticated participant is required.", 401);
  if ("allowed" in actor) {
    if (!actor.allowed) throw new CommunityStoreError("auth_required", "An authenticated participant is required.", 401);
    return { identity: actor.actor, credential: actor.credential, actions: actor.credential.actions };
  }
  if ("identity" in actor) return actor;
  return { identity: actor };
}

function requireActor(actor: CommunityActor, action?: CommunityWriteAction): CommunityActorContext {
  const context = actorContext(actor);
  const identity = context.identity;
  if (!identity || typeof identity.id !== "string" || !identity.id || identity.status !== "active") {
    throw new CommunityStoreError("auth_required", "An active authenticated identity is required.", 401);
  }
  if (action && context.actions && !context.actions.includes(action)) {
    throw new CommunityStoreError("action_not_granted", `The authenticated identity is not granted '${action}'.`, 403);
  }
  return context;
}

/** Derive ownership solely from the authenticated identity; request body names are never consulted. */
export function ownerHandleForIdentity(identity: IdentityView): string {
  const candidate = identity.handle || identity.display_name || identity.id;
  const handle = plainText(candidate, "owner_handle", 120, true, true);
  return handle || identity.id;
}

function challengeKey(input: { problem_id?: string; challenge_id?: string; task_id?: string }): string {
  const value = input.problem_id ?? input.challenge_id ?? input.task_id;
  return identifier(value, "problem_id");
}

function stateCopy(state: CommunityState): CommunityState {
  return clone(state);
}

function storedNumber(value: unknown, fallback: number | null): number | null {
  return typeof value === "number" && Number.isFinite(value) && value >= 0 && value <= Number.MAX_SAFE_INTEGER
    ? Math.floor(value)
    : fallback;
}

function storedMetrics(item: Partial<ChallengeSubmissionRecord>): SolutionPerformanceMetrics {
  const candidate = item.metrics && typeof item.metrics === "object" ? item.metrics as Partial<SolutionPerformanceMetrics> : {};
  const metadata = item.metadata && typeof item.metadata === "object" ? item.metadata : {};
  const cycles = storedNumber(candidate.cycles, storedNumber(item.cycles, null));
  const optLevel = typeof candidate.opt_level === "string"
    ? candidate.opt_level
    : typeof metadata.opt_level === "string" ? metadata.opt_level : null;
  return {
    runtime_ms: storedNumber(candidate.runtime_ms, null),
    memory_kib: storedNumber(candidate.memory_kib, null),
    cycles,
    compile_cycles: storedNumber(candidate.compile_cycles, null),
    tests_passed: storedNumber(candidate.tests_passed, 0) || 0,
    tests_total: storedNumber(candidate.tests_total, 0) || 0,
    engine: typeof candidate.engine === "string" ? candidate.engine : typeof item.engine === "string" ? item.engine : null,
    opt_level: optLevel,
  };
}

function emptyMetrics(): SolutionPerformanceMetrics {
  return {
    runtime_ms: null,
    memory_kib: null,
    cycles: null,
    compile_cycles: null,
    tests_passed: 0,
    tests_total: 0,
    engine: null,
    opt_level: null,
  };
}

export class CommunityStore {
  private readonly now: () => number;
  private readonly state_path: string | null;
  private readonly limits: typeof COMMUNITY_LIMITS;
  private state: CommunityState;

  constructor(options: CommunityStoreOptions = {}) {
    this.now = options.now || (() => Date.now());
    const configuredPath = options.state_path !== undefined
      ? options.state_path
      : options.community_state_path !== undefined
        ? options.community_state_path
        : options.path !== undefined
          ? options.path
          : options.file_path !== undefined
            ? options.file_path
            : options.storage_root !== undefined
              ? options.storage_root === null ? null : join(options.storage_root || ".", "community.json")
              : options.storageRoot !== undefined
                ? options.storageRoot === null ? null : join(options.storageRoot || ".", "community.json")
                : defaultStatePath();
    this.state_path = configuredPath === null ? null : resolve(configuredPath || defaultStatePath());
    this.limits = Object.freeze({ ...COMMUNITY_LIMITS, ...(options.limits || {}) });
    this.state = this.load();
  }

  private emptyState(): CommunityState {
    return {
      schema_version: COMMUNITY_STATE_SCHEMA_VERSION,
      revision: 0,
      updated_at: new Date(this.now()).toISOString(),
      solution_revisions: [],
      submissions: [],
      discussions: [],
      solution_votes: [],
    };
  }

  private load(): CommunityState {
    if (!this.state_path || !existsSync(this.state_path)) return this.emptyState();
    let parsed: unknown;
    try {
      parsed = JSON.parse(readFileSync(this.state_path, "utf8")) as unknown;
    } catch {
      throw new CommunityStoreError("state_invalid", "Community state is not valid JSON.", 500);
    }
    if (!parsed || typeof parsed !== "object") throw new CommunityStoreError("state_invalid", "Community state has an invalid root.", 500);
    const candidate = parsed as Partial<CommunityState>;
    if (candidate.schema_version !== COMMUNITY_STATE_SCHEMA_VERSION || !Array.isArray(candidate.solution_revisions) || !Array.isArray(candidate.submissions) || !Array.isArray(candidate.discussions)) {
      throw new CommunityStoreError("state_schema_invalid", "Community state schema is unsupported.", 500);
    }
    if (typeof candidate.revision !== "number" || !Number.isSafeInteger(candidate.revision) || candidate.revision < 0) throw new CommunityStoreError("state_invalid", "Community state revision is invalid.", 500);
    return {
      schema_version: COMMUNITY_STATE_SCHEMA_VERSION,
      revision: candidate.revision,
      updated_at: typeof candidate.updated_at === "string" ? candidate.updated_at : new Date(this.now()).toISOString(),
      // Older durable state predates the explicit visibility field. Those
      // revisions were benchmark drafts, so migrate them as private rather
      // than accidentally publishing existing participant work.
      solution_revisions: clone(candidate.solution_revisions).map((item) => ({
        ...item,
        visibility: item.visibility === "public" ? "public" : "private",
      })),
      submissions: clone(candidate.submissions).map((item) => ({
        ...item,
        metrics: storedMetrics(item),
      })),
      discussions: clone(candidate.discussions),
      solution_votes: Array.isArray(candidate.solution_votes)
        ? clone(candidate.solution_votes).filter((item) => item && typeof item.solution_id === "string" && typeof item.voter_identity_id === "string")
        : [],
    };
  }

  private persist(): void {
    if (!this.state_path) return;
    const temporaryPath = `${this.state_path}.${process.pid}.${randomBytes(8).toString("hex")}.tmp`;
    mkdirSync(dirname(this.state_path), { recursive: true });
    try {
      writeFileSync(temporaryPath, `${JSON.stringify(this.state)}\n`, { encoding: "utf8", mode: 0o600 });
      renameSync(temporaryPath, this.state_path);
    } catch (error) {
      try { if (existsSync(temporaryPath)) unlinkSync(temporaryPath); } catch { /* Preserve the original persistence failure. */ }
      throw new CommunityStoreError("state_persist_failed", "Community state could not be persisted.", 503);
    }
  }

  private commit(): void {
    const before = stateCopy(this.state);
    this.state.revision += 1;
    this.state.updated_at = new Date(this.now()).toISOString();
    try {
      this.persist();
    } catch (error) {
      this.state = before;
      throw error;
    }
  }

  snapshot(): {
    schema_version: typeof COMMUNITY_STATE_SCHEMA_VERSION;
    revision: number;
    updated_at: string;
    solution_revisions: SavedSolutionRevision[];
    submissions: ChallengeSubmissionRecord[];
    discussions: DiscussionPost[];
    solution_votes: SolutionVoteRecord[];
  } {
    return stateCopy(this.state);
  }

  saveSolutionRevision(actor: CommunityActor, input: SaveSolutionRevisionInput): SavedSolutionRevision {
    const context = requireActor(actor, "artifact");
    const problemId = challengeKey(input);
    const title = plainText(input.title || "Solution", "title", this.limits.solution_title, true, true);
    const code = plainText(input.code, "code", this.limits.solution_code);
    const language = plainText(input.language || "trit", "language", 32);
    const visibility = input.visibility || "private";
    if (visibility !== "private" && visibility !== "public") throw new CommunityStoreError("visibility_invalid", "visibility must be private or public.");
    const metadata = safeMetadata(input.metadata, this.limits.metadata);
    const ownerIdentityId = context.identity.id;
    const ownerHandle = ownerHandleForIdentity(context.identity);
    const solutionId = input.solution_id ? identifier(input.solution_id, "solution_id") : newId("tc:solution:");
    const prior = this.state.solution_revisions.filter((revision) => revision.solution_id === solutionId);
    if (prior.length && prior.some((revision) => revision.owner_identity_id !== ownerIdentityId)) {
      throw new CommunityStoreError("solution_owner_mismatch", "A solution can only be revised by its owner.", 403);
    }
    if (visibility === "public" && !this.state.submissions.some((submission) => (
      submission.problem_id === problemId
      && submission.owner_identity_id === ownerIdentityId
      && submission.outcome === "accepted"
      && submission.code_hash === sha256Text(code)
    ))) {
      throw new CommunityStoreError("solution_not_verified", "A public solution must match a verified accepted submission before it can be posted.", 409);
    }
    const revision: SavedSolutionRevision = {
      schema_version: COMMUNITY_API_SCHEMA_VERSION,
      id: newId("tc:solution-revision:"),
      solution_id: solutionId,
      revision: prior.reduce((max, item) => Math.max(max, item.revision), 0) + 1,
      problem_id: problemId,
      challenge_id: problemId,
      task_id: problemId,
      title,
      code,
      language,
      visibility,
      owner_identity_id: ownerIdentityId,
      owner_handle: ownerHandle,
      metadata,
      created_at: new Date(this.now()).toISOString(),
      updated_at: new Date(this.now()).toISOString(),
    };
    this.state.solution_revisions.push(revision);
    this.commit();
    return clone(revision);
  }

  /** Short alias for route adapters. */
  saveSolution(actor: CommunityActor, input: SaveSolutionRevisionInput): SavedSolutionRevision {
    return this.saveSolutionRevision(actor, input);
  }

  publishSolution(actor: CommunityActor, input: PublishSolutionInput): { solution: SavedSolutionRevision; discussion: DiscussionPost } {
    const explanation = plainText(input.explanation, "explanation", this.limits.discussion_body, true, true);
    const pseudocode = plainText(input.pseudocode, "pseudocode", this.limits.discussion_body, false, true);
    const discussionBody = plainText([
      "Plain-English explanation",
      explanation,
      ...(pseudocode ? ["", "Pseudocode", pseudocode] : []),
    ].join("\n"), "body", this.limits.discussion_body, true, true);
    const solution = this.saveSolutionRevision(actor, { ...input, visibility: "public" });
    const discussion = this.addDiscussion(actor, {
      problem_id: solution.problem_id,
      solution_id: solution.solution_id,
      title: solution.title,
      body: discussionBody,
    });
    return { solution, discussion };
  }

  getSolutionRevision(id: string): SavedSolutionRevision | null {
    const revision = this.state.solution_revisions.find((item) => item.id === id);
    return revision ? clone(revision) : null;
  }

  getSolution(id: string): SavedSolutionRevision | null {
    return this.getSolutionRevision(id);
  }

  listSolutionRevisions(filter: { problem_id?: string; challenge_id?: string; task_id?: string; solution_id?: string; owner_identity_id?: string; limit?: number } = {}): SavedSolutionRevision[] {
    const problemId = filter.problem_id || filter.challenge_id || filter.task_id;
    const limit = Math.min(this.limits.list_limit, Math.max(1, Math.floor(filter.limit || this.limits.list_limit)));
    return this.state.solution_revisions
      .filter((item) => (!problemId || item.problem_id === problemId) && (!filter.solution_id || item.solution_id === filter.solution_id) && (!filter.owner_identity_id || item.owner_identity_id === filter.owner_identity_id))
      .slice(-limit)
      .reverse()
      .map(clone);
  }

  /**
   * Return the latest revision of each explicitly published solution for a
   * challenge. Private benchmark drafts never cross this boundary. A posted
   * solution is marked solved only when its author has an accepted submission
   * for the same source, which also supports submissions created before the
   * UI started attaching a solution id.
   */
  listPublicSolutions(filter: { problem_id?: string; challenge_id?: string; task_id?: string; limit?: number; viewer_identity_id?: string } = {}): PublicSolutionRecord[] {
    const problemId = filter.problem_id || filter.challenge_id || filter.task_id;
    const limit = Math.min(this.limits.list_limit, Math.max(1, Math.floor(filter.limit || this.limits.list_limit)));
    const latestBySolution = new Map<string, SavedSolutionRevision>();
    for (const item of this.state.solution_revisions) {
      if (problemId && item.problem_id !== problemId) continue;
      const current = latestBySolution.get(item.solution_id);
      if (!current || item.revision > current.revision || (item.revision === current.revision && item.updated_at > current.updated_at)) {
        latestBySolution.set(item.solution_id, item);
      }
    }
    const acceptedSubmissions = this.state.submissions.filter((item) => (!problemId || item.problem_id === problemId) && item.outcome === "accepted");
    return [...latestBySolution.values()]
      .filter((item) => item.visibility === "public")
      .map((item) => {
        const accepted = acceptedSubmissions
          .filter((submission) => (
            submission.owner_identity_id === item.owner_identity_id
            && submission.code_hash === sha256Text(item.code)
            && (submission.solution_id === null || submission.solution_id === item.solution_id)
          ))
          .sort((left, right) => {
            const leftRuntime = left.metrics.runtime_ms ?? Number.MAX_SAFE_INTEGER;
            const rightRuntime = right.metrics.runtime_ms ?? Number.MAX_SAFE_INTEGER;
            if (leftRuntime !== rightRuntime) return leftRuntime - rightRuntime;
            const leftCycles = left.metrics.cycles ?? left.cycles ?? Number.MAX_SAFE_INTEGER;
            const rightCycles = right.metrics.cycles ?? right.cycles ?? Number.MAX_SAFE_INTEGER;
            if (leftCycles !== rightCycles) return leftCycles - rightCycles;
            return right.created_at.localeCompare(left.created_at);
          });
        const discussion = this.state.discussions
          .filter((post) => post.solution_id === item.solution_id && post.parent_id === null)
          .sort((left, right) => right.updated_at.localeCompare(left.updated_at))[0];
        const voterIds = new Set(this.state.solution_votes
          .filter((vote) => vote.solution_id === item.solution_id)
          .map((vote) => vote.voter_identity_id));
        return {
          ...clone(item),
          solved: accepted.length > 0,
          upvotes: voterIds.size,
          viewer_has_upvoted: Boolean(filter.viewer_identity_id && voterIds.has(filter.viewer_identity_id)),
          discussion_body: discussion?.body || "",
          metrics: accepted[0]?.metrics ? clone(accepted[0].metrics) : null,
        };
      })
      .sort((left, right) => right.upvotes - left.upvotes || Number(right.solved) - Number(left.solved) || right.updated_at.localeCompare(left.updated_at))
      .slice(0, limit)
      .map((item) => item);
  }

  recordChallengeSubmission(actor: CommunityActor, input: ChallengeSubmissionInput): ChallengeSubmissionRecord {
    const context = requireActor(actor, "test");
    const problemId = challengeKey(input);
    const code = plainText(input.code, "code", this.limits.submission_code);
    const language = plainText(input.language || "trit", "language", 32);
    const engine = input.engine === undefined ? null : plainText(input.engine, "engine", 64, false) || null;
    const metadata = safeMetadata(input.metadata, this.limits.metadata);
    const outcome = input.outcome || (input.accepted === true ? "accepted" : input.accepted === false ? "rejected" : "unknown");
    if (!["accepted", "rejected", "pending", "unknown"].includes(outcome)) throw new CommunityStoreError("outcome_invalid", "outcome is invalid.");
    const cycles = finiteNumber(input.metrics?.cycles ?? input.cycles, "cycles");
    const runtimeMs = finiteNumber(input.metrics?.runtime_ms, "metrics.runtime_ms");
    const memoryKib = finiteNumber(input.metrics?.memory_kib, "metrics.memory_kib");
    const compileCycles = finiteNumber(input.metrics?.compile_cycles, "metrics.compile_cycles");
    const testsPassed = finiteNumber(input.metrics?.tests_passed, "metrics.tests_passed") ?? 0;
    const testsTotal = finiteNumber(input.metrics?.tests_total, "metrics.tests_total") ?? 0;
    if (testsPassed > testsTotal) throw new CommunityStoreError("metrics_invalid", "tests_passed cannot exceed tests_total.");
    const optLevel = input.metrics?.opt_level === undefined || input.metrics?.opt_level === null
      ? typeof metadata.opt_level === "string" ? plainText(metadata.opt_level, "opt_level", 32, false, true) || null : null
      : plainText(input.metrics.opt_level, "opt_level", 32, false, true) || null;
    const record: ChallengeSubmissionRecord = {
      schema_version: COMMUNITY_API_SCHEMA_VERSION,
      id: newId("tc:submission:"),
      problem_id: problemId,
      challenge_id: problemId,
      task_id: problemId,
      solution_id: input.solution_id ? identifier(input.solution_id, "solution_id") : null,
      code,
      code_hash: sha256Text(code),
      language,
      engine,
      owner_identity_id: context.identity.id,
      owner_handle: ownerHandleForIdentity(context.identity),
      metadata,
      outcome,
      cycles,
      metrics: {
        runtime_ms: runtimeMs,
        memory_kib: memoryKib,
        cycles,
        compile_cycles: compileCycles,
        tests_passed: testsPassed,
        tests_total: testsTotal,
        engine,
        opt_level: optLevel,
      },
      created_at: new Date(this.now()).toISOString(),
    };
    this.state.submissions.push(record);
    this.commit();
    return clone(record);
  }

  toggleSolutionVote(actor: CommunityActor, solutionId: string): { solution_id: string; upvoted: boolean; upvotes: number } {
    const context = requireActor(actor, "artifact");
    const normalizedSolutionId = identifier(solutionId, "solution_id");
    const latest = this.state.solution_revisions
      .filter((item) => item.solution_id === normalizedSolutionId)
      .sort((left, right) => right.revision - left.revision || right.updated_at.localeCompare(left.updated_at))[0];
    if (!latest || latest.visibility !== "public") throw new CommunityStoreError("solution_not_found", "The public solution was not found.", 404);
    const existingIndex = this.state.solution_votes.findIndex((vote) => vote.solution_id === normalizedSolutionId && vote.voter_identity_id === context.identity.id);
    let upvoted: boolean;
    if (existingIndex >= 0) {
      this.state.solution_votes.splice(existingIndex, 1);
      upvoted = false;
    } else {
      this.state.solution_votes.push({
        schema_version: COMMUNITY_API_SCHEMA_VERSION,
        id: newId("tc:solution-vote:"),
        solution_id: normalizedSolutionId,
        voter_identity_id: context.identity.id,
        created_at: new Date(this.now()).toISOString(),
      });
      upvoted = true;
    }
    this.commit();
    const upvotes = new Set(this.state.solution_votes.filter((vote) => vote.solution_id === normalizedSolutionId).map((vote) => vote.voter_identity_id)).size;
    return { solution_id: normalizedSolutionId, upvoted, upvotes };
  }

  /** Alias emphasizing that caller-supplied usernames are not accepted. */
  recordAuthenticatedSubmission(actor: CommunityActor, input: ChallengeSubmissionInput): ChallengeSubmissionRecord {
    return this.recordChallengeSubmission(actor, input);
  }

  listChallengeSubmissions(filter: { problem_id?: string; challenge_id?: string; task_id?: string; owner_identity_id?: string; limit?: number } = {}): ChallengeSubmissionRecord[] {
    const problemId = filter.problem_id || filter.challenge_id || filter.task_id;
    const limit = Math.min(this.limits.list_limit, Math.max(1, Math.floor(filter.limit || this.limits.list_limit)));
    return this.state.submissions
      .filter((item) => (!problemId || item.problem_id === problemId) && (!filter.owner_identity_id || item.owner_identity_id === filter.owner_identity_id))
      .slice(-limit)
      .reverse()
      .map(clone);
  }

  listSubmissions(filter: { problem_id?: string; challenge_id?: string; task_id?: string; owner_identity_id?: string; limit?: number } = {}): ChallengeSubmissionRecord[] {
    return this.listChallengeSubmissions(filter);
  }

  addDiscussion(actor: CommunityActor, input: AddDiscussionInput): DiscussionPost {
    const context = requireActor(actor, "artifact");
    const problemId = challengeKey(input);
    const title = input.title === undefined ? null : plainText(input.title, "title", this.limits.discussion_title, false, true) || null;
    const body = plainText(input.body, "body", this.limits.discussion_body, true, true);
    const solutionId = input.solution_id ? identifier(input.solution_id, "solution_id") : null;
    const parentId = input.parent_id ? identifier(input.parent_id, "parent_id") : null;
    const metadata = safeMetadata(input.metadata, this.limits.metadata);
    const timestamp = new Date(this.now()).toISOString();
    const post: DiscussionPost = {
      schema_version: COMMUNITY_API_SCHEMA_VERSION,
      id: newId("tc:discussion:"),
      problem_id: problemId,
      challenge_id: problemId,
      task_id: problemId,
      solution_id: solutionId,
      parent_id: parentId,
      title,
      body,
      author_identity_id: context.identity.id,
      author_handle: ownerHandleForIdentity(context.identity),
      metadata,
      created_at: timestamp,
      updated_at: timestamp,
    };
    this.state.discussions.push(post);
    this.commit();
    return clone(post);
  }

  /** Aliases for route adapters that use resource-oriented names. */
  createDiscussion(actor: CommunityActor, input: AddDiscussionInput): DiscussionPost {
    return this.addDiscussion(actor, input);
  }

  listDiscussions(filter: { problem_id?: string; challenge_id?: string; task_id?: string; solution_id?: string; parent_id?: string; limit?: number } = {}): DiscussionPost[] {
    const problemId = filter.problem_id || filter.challenge_id || filter.task_id;
    const limit = Math.min(this.limits.list_limit, Math.max(1, Math.floor(filter.limit || this.limits.list_limit)));
    return this.state.discussions
      .filter((item) => (!problemId || item.problem_id === problemId) && (!filter.solution_id || item.solution_id === filter.solution_id) && (!filter.parent_id || item.parent_id === filter.parent_id))
      .slice(-limit)
      .reverse()
      .map(clone);
  }

  listDiscussion(filter: { problem_id?: string; challenge_id?: string; task_id?: string; solution_id?: string; parent_id?: string; limit?: number } = {}): DiscussionPost[] {
    return this.listDiscussions(filter);
  }
}

export type CommunityStateStore = CommunityStore;

export function createCommunityStore(options: CommunityStoreOptions = {}): CommunityStore {
  return new CommunityStore(options);
}
