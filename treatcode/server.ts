import express, { Request, Response } from "express";
import cors from "cors";
import fs from "fs";
import path from "path";
import {
  PUBLIC_API_SCHEMA_VERSION,
  PublicRecord,
  PublicSnapshot,
  collectionFor,
  normalizeSearchMode,
  searchSnapshot,
} from "./src/publicApi";
import {
  WORKSPACE_API_SCHEMA_VERSION,
  WorkspaceServiceError,
  workspaceService,
  type WorkspaceActor,
} from "./src/workspaceService";
import type { WorkspaceScope } from "./src/workspaceApi";
import {
  EXECUTION_REQUEST_SCHEMA,
  SecureExecutionQueue,
  type ExecutionResult,
  type RunnerEngine,
} from "./src/runner/secure-runner";
import {
  CONTRIBUTION_ACTIONS,
  ContributionAction,
  ContributionActor,
  ContributionError,
  ContributionService,
  InMemoryGitHubApp,
} from "./src/contributionService";
import { OperationActor, OperationsBackup, OperationsStore, TaskInput } from "./operationsStore";
import {
  AUTH_API_SCHEMA_VERSION,
  DEFAULT_PROJECT_ID,
  PERMISSION_ACTIONS,
  AuthStore,
  actionCatalog,
  bearerToken,
  createDefaultAuthStore,
  type AuthorizationDenial,
  type PermissionAction,
} from "./src/auth";

export const app = express();
const PORT = process.env.PORT || 3000;

app.use(cors());
app.use(express.json({ limit: "8mb" }));

// In production, Vite builds static files to 'dist'. Serve them.
const distPath = path.join(__dirname, "dist");
if (fs.existsSync(distPath)) {
  // Avoid a redirect-only response for the public entry points. This keeps
  // direct requests useful to crawlers and clients with JavaScript disabled.
  app.get("/", (_req: Request, res: Response) => res.sendFile(path.join(distPath, "index.html")));
  app.get("/stack", (_req: Request, res: Response) => res.sendFile(path.join(distPath, "stack", "index.html")));
  app.get("/learn", (_req: Request, res: Response) => res.sendFile(path.join(distPath, "learn", "index.html")));
  app.get("/operations", (_req: Request, res: Response) => res.sendFile(path.join(distPath, "operations", "index.html")));
  app.use(express.static(distPath));
}

// The public platform surface is intentionally read-only and snapshot-backed.
// It is registered before the legacy challenge endpoints so public reading
// routes never need to load the compiler, editor, or runner state.
const publicSnapshotCandidates = [
  path.join(__dirname, "public", "api", "v1", "snapshot.json"),
  path.join(process.cwd(), "public", "api", "v1", "snapshot.json"),
  path.join(process.cwd(), "treatcode", "public", "api", "v1", "snapshot.json"),
];

function loadPublicSnapshot(): PublicSnapshot {
  for (const candidate of publicSnapshotCandidates) {
    try {
      const parsed = JSON.parse(fs.readFileSync(candidate, "utf8")) as PublicSnapshot;
      if (parsed.schema_version === "treatcode.public.snapshot.v1" && parsed.snapshot?.commit) return parsed;
    } catch {
      // Try the next source location; the static route still has a useful fallback.
    }
  }
  return {
    schema_version: "treatcode.public.snapshot.v1",
    snapshot: {
      id: "tc:snapshot:unavailable",
      repository: "https://github.com/JonasComlita/dualrail",
      commit: "unknown",
      generated_at: new Date(0).toISOString(),
      source: "snapshot unavailable",
    },
    projects: [], stack_nodes: [], components: [], capabilities: [], contracts: [], decisions: [], sources: [], symbols: [], tests: [], benchmarks: [], runs: [], releases: [], gaps: [], relations: [], statistics: {},
  };
}

const publicSnapshot = loadPublicSnapshot();
const publicResources = new Set([
  "projects", "stack-nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps",
]);

function publicEnvelope<T>(data: T, resource: string, prefix: string, meta: Record<string, unknown> = {}) {
  return {
    schema_version: PUBLIC_API_SCHEMA_VERSION,
    snapshot: publicSnapshot.snapshot,
    data,
    meta: { resource, ...meta },
    links: { self: `${prefix}/${resource}` },
  };
}

function publicError(res: Response, status: number, code: string, message: string, prefix: string) {
  res.status(status).json({
    schema_version: PUBLIC_API_SCHEMA_VERSION,
    snapshot: publicSnapshot.snapshot,
    error: { code, message },
    links: { api: prefix },
  });
}

function publicEntityById(id: string): PublicRecord | null {
  for (const resource of ["projects", "stack_nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps"] as const) {
    const record = publicSnapshot[resource].find((candidate) => candidate.id === id);
    if (record) return record;
  }
  return null;
}

function registerPublicApi(prefix: string) {
  app.get(prefix, (_req: Request, res: Response) => {
    const counts = Object.fromEntries([...publicResources].map((resource) => [resource, collectionFor(publicSnapshot, resource)?.length || 0]));
    res.json(publicEnvelope({ api_schema: PUBLIC_API_SCHEMA_VERSION, snapshot_schema: publicSnapshot.schema_version, resources: counts }, "", prefix, { read_only: true }));
  });

  app.get(`${prefix}/snapshot.json`, (_req: Request, res: Response) => {
    res.setHeader("Cache-Control", "public, max-age=60");
    res.json(publicSnapshot);
  });

  app.get(`${prefix}/openapi.json`, (_req: Request, res: Response) => {
    const contractCandidates = [
      path.join(__dirname, "public", "api", "v1", "openapi.json"),
      path.join(process.cwd(), "public", "api", "v1", "openapi.json"),
      path.join(process.cwd(), "treatcode", "public", "api", "v1", "openapi.json"),
    ];
    for (const candidate of contractCandidates) {
      try {
        const contract = JSON.parse(fs.readFileSync(candidate, "utf8"));
        res.json({ ...contract, "x-treatcode-schema-version": PUBLIC_API_SCHEMA_VERSION, "x-treatcode-source-snapshot": publicSnapshot.snapshot });
        return;
      } catch {
        // Keep looking for the generated contract.
      }
    }
    publicError(res, 404, "contract_unavailable", "The generated OpenAPI contract is unavailable.", prefix);
  });

  app.get(`${prefix}/search`, (req: Request, res: Response) => {
    const query = String(req.query.q || "").trim();
    if (!query) {
      publicError(res, 400, "query_required", "Search requires a non-empty q parameter.", prefix);
      return;
    }
    const mode = normalizeSearchMode(String(req.query.mode || "semantic"));
    const requestedLimit = Number.parseInt(String(req.query.limit || "25"), 10);
    const limit = Number.isFinite(requestedLimit) ? Math.max(1, Math.min(100, requestedLimit)) : 25;
    const results = searchSnapshot(publicSnapshot, query, mode).slice(0, limit);
    res.json(publicEnvelope(results, "search", prefix, { count: results.length, query, mode, limit }));
  });

  app.get(`${prefix}/:resource/:id`, (req: Request, res: Response) => {
    const resource = String(req.params.resource);
    if (!publicResources.has(resource)) {
      publicError(res, 404, "resource_not_found", `Unknown public resource: ${resource}.`, prefix);
      return;
    }
    const collection = collectionFor(publicSnapshot, resource);
    const record = collection?.find((candidate) => candidate.id === req.params.id) || publicEntityById(req.params.id);
    if (!record) {
      publicError(res, 404, "entity_not_found", `No ${resource} entity exists for id ${req.params.id}.`, prefix);
      return;
    }
    res.json(publicEnvelope(record, resource, prefix));
  });

  app.get(`${prefix}/:resource`, (req: Request, res: Response, next) => {
    const resource = String(req.params.resource);
    if (resource.endsWith(".json")) {
      next();
      return;
    }
    if (!publicResources.has(resource)) {
      publicError(res, 404, "resource_not_found", `Unknown public resource: ${resource}.`, prefix);
      return;
    }
    const collection = collectionFor(publicSnapshot, resource) || [];
    const requestedLimit = Number.parseInt(String(req.query.limit || "25"), 10);
    const requestedOffset = Number.parseInt(String(req.query.offset || "0"), 10);
    const limit = Number.isFinite(requestedLimit) ? Math.max(1, Math.min(100, requestedLimit)) : 25;
    const offset = Number.isFinite(requestedOffset) ? Math.max(0, requestedOffset) : 0;
    const data = collection.slice(offset, offset + limit);
    res.json(publicEnvelope(data, resource, prefix, { count: data.length, total: collection.length, offset, limit }));
  });
}

// Register the workspace alias before the generic public `/api/v1/:resource`
// matcher so the versioned workspace collection cannot be mistaken for a
// read-only public resource.
registerWorkspaceApi("/api/workspaces/v1");
registerWorkspaceApi("/api/v1/workspaces", "/api/v1/workspaces");
registerPublicApi("/api/public/v1");
registerPublicApi("/api/v1");

// P10 benchmark evidence is read-only and comes from the repository's
// versioned manifest/reference artifact. Candidate execution remains owned by
// the isolated runner surface; this endpoint never executes submitted code.
function loadP10Artifact(relativePath: string): Record<string, unknown> | null {
  const candidates = [
    path.join(process.cwd(), relativePath),
    path.join(process.cwd(), "..", relativePath),
    path.join(__dirname, relativePath),
    path.join(__dirname, "..", relativePath),
  ];
  for (const candidate of candidates) {
    try {
      return JSON.parse(fs.readFileSync(candidate, "utf8")) as Record<string, unknown>;
    } catch {
      // Try the next workspace layout used by Bun, node, and the built server.
    }
  }
  return null;
}

app.get("/api/benchmarks/p10", (_req: Request, res: Response) => {
  const manifest = loadP10Artifact("BENCHMARK_MANIFEST.json");
  const reference = loadP10Artifact(path.join("benchmarks", "reference", "p10-reference.v1.json"));
  if (!manifest || !reference) {
    res.status(503).json({ schema: "treatcode.p10_benchmark_api.v1", error: "P10 benchmark evidence is unavailable." });
    return;
  }
  res.setHeader("Cache-Control", "public, max-age=60");
  res.json({
    schema: "treatcode.p10_benchmark_api.v1",
    manifest,
    reference,
    provenance: {
      manifest: "BENCHMARK_MANIFEST.json",
      protocol: "BENCHMARK_PROTOCOL_SCHEMA.json",
      reference: "benchmarks/reference/p10-reference.v1.json",
      read_only: true,
    },
  });
});

// P07 identity and authorization are shared by the UI-facing action routes and
// the versioned auth API. Credentials are opaque, short-lived, and stored only
// as hashes; the raw value is returned exactly once from login or task issuance.
export const authStore: AuthStore = createDefaultAuthStore({
  audit_path: process.env.TREATCODE_AUTH_AUDIT_PATH || path.resolve(__dirname, "..", "build", "treatcode-auth", "audit.jsonl"),
});

function authToken(req: Request): string | null {
  return bearerToken(req.header("authorization"));
}

function requestProject(req: Request, body?: Record<string, unknown>): string {
  return String(req.header("x-treatcode-project") || body?.project_id || DEFAULT_PROJECT_ID);
}

function requestTask(req: Request, body?: Record<string, unknown>): string | null {
  const value = req.header("x-treatcode-task") || body?.task_id;
  return value ? String(value) : null;
}

function requestNonce(req: Request): string | null {
  return req.header("x-action-nonce") || req.header("idempotency-key") || null;
}

function authErrorResponse(res: Response, denial: AuthorizationDenial): void {
  res.status(denial.http_status).json({
    schema_version: AUTH_API_SCHEMA_VERSION,
    error: {
      code: denial.code,
      reason: denial.reason,
      action: denial.action,
      project_id: denial.project_id,
      task_id: denial.task_id,
      audit_event_id: denial.audit_event_id,
      retryable: denial.retryable,
    },
    meta: { policy_version: "treatcode.authz.policy.v1" },
  });
}

function authDataResponse(res: Response, data: unknown, status = 200): void {
  res.status(status).json({
    schema_version: AUTH_API_SCHEMA_VERSION,
    data,
    meta: { policy_version: "treatcode.authz.policy.v1" },
  });
}

function requireAction(
  req: Request,
  res: Response,
  action: PermissionAction,
  options: { project_id?: string; task_id?: string | null; require_nonce?: boolean } = {},
) {
  const decision = authStore.authorize({
    token: authToken(req),
    action,
    project_id: options.project_id || requestProject(req, req.body),
    task_id: options.task_id === undefined ? requestTask(req, req.body) : options.task_id,
    request_nonce: requestNonce(req),
    resource: req.path,
    require_nonce: options.require_nonce,
  });
  if (!decision.allowed) {
    authErrorResponse(res, decision.denial);
    return null;
  }
  return decision;
}

app.get("/api/auth/v1", (_req: Request, res: Response) => {
  authDataResponse(res, {
    api_schema: AUTH_API_SCHEMA_VERSION,
    policy_version: "treatcode.authz.policy.v1",
    actions: actionCatalog(),
    identity_kinds: ["human", "collaborator", "service", "agent"],
    links: {
      login: "/api/auth/v1/login",
      capabilities: "/api/auth/v1/capabilities",
      openapi: "/api/auth/v1/openapi.json",
      audit: "/api/auth/v1/audit",
    },
  });
});

app.get("/api/auth/v1/openapi.json", (_req: Request, res: Response) => {
  const candidates = [
    path.resolve(__dirname, "..", "docs", "11_TreatCode_Platform", "schemas", "auth_api.v1.openapi.json"),
    path.resolve(process.cwd(), "docs", "11_TreatCode_Platform", "schemas", "auth_api.v1.openapi.json"),
  ];
  for (const candidate of candidates) {
    try {
      res.json(JSON.parse(fs.readFileSync(candidate, "utf8")));
      return;
    } catch {
      // Continue to the next source location.
    }
  }
  res.status(404).json({ schema_version: AUTH_API_SCHEMA_VERSION, error: { code: "contract_unavailable", reason: "The auth API contract is unavailable." } });
});

app.post("/api/auth/v1/login", (req: Request, res: Response) => {
  const identityId = String(req.body?.identity_id || "");
  const accessKey = typeof req.body?.access_key === "string" ? req.body.access_key : "";
  const result = authStore.login(identityId, accessKey);
  if (!result.ok) {
    authErrorResponse(res, result.denial);
    return;
  }
  res.setHeader("Cache-Control", "no-store");
  authDataResponse(res, { identity: result.identity, credential: result.credential });
});

app.get("/api/auth/v1/session", (req: Request, res: Response) => {
  const decision = requireAction(req, res, "read", { require_nonce: false });
  if (!decision) return;
  authDataResponse(res, { identity: decision.actor, credential: decision.credential });
});

app.get("/api/auth/v1/capabilities", (req: Request, res: Response) => {
  const result = authStore.capabilities({ token: authToken(req), project_id: requestProject(req), task_id: requestTask(req) });
  if (!result.ok) {
    authErrorResponse(res, result.denial);
    return;
  }
  authDataResponse(res, result);
});

app.post("/api/auth/v1/check", (req: Request, res: Response) => {
  const rawAction = String(req.body?.action || "");
  if (!(PERMISSION_ACTIONS as readonly string[]).includes(rawAction)) {
    res.status(400).json({ schema_version: AUTH_API_SCHEMA_VERSION, error: { code: "invalid_scope", reason: "action must be one of the published independent permission actions." } });
    return;
  }
  const decision = requireAction(req, res, rawAction as PermissionAction, {
    project_id: requestProject(req, req.body),
    task_id: requestTask(req, req.body),
    require_nonce: false,
  });
  if (!decision) return;
  authDataResponse(res, { allowed: true, action: rawAction, identity: decision.actor, credential: decision.credential });
});

app.post("/api/auth/v1/tasks", (req: Request, res: Response) => {
  const result = authStore.issueTaskCredential({
    requester_token: authToken(req),
    target_identity_id: String(req.body?.target_identity_id || ""),
    project_id: requestProject(req, req.body),
    task_id: requestTask(req, req.body) || "",
    actions: Array.isArray(req.body?.actions) ? req.body.actions : [],
    ttl_seconds: Number(req.body?.ttl_seconds),
    request_nonce: requestNonce(req),
  });
  if (!result.ok) {
    authErrorResponse(res, result.denial);
    return;
  }
  res.setHeader("Cache-Control", "no-store");
  authDataResponse(res, { identity: result.identity, credential: result.credential }, 201);
});

app.post("/api/auth/v1/credentials/:credentialId/revoke", (req: Request, res: Response) => {
  const result = authStore.revokeCredential({
    requester_token: authToken(req),
    credential_id: String(req.params.credentialId),
    project_id: requestProject(req, req.body),
    task_id: requestTask(req, req.body),
    request_nonce: requestNonce(req),
  });
  if (!result.ok) {
    authErrorResponse(res, result.denial);
    return;
  }
  authDataResponse(res, { credential: result.credential });
});

app.get("/api/auth/v1/audit", (req: Request, res: Response) => {
  const result = authStore.auditEvents({ token: authToken(req), project_id: requestProject(req), task_id: requestTask(req) });
  if (!result.ok) {
    authErrorResponse(res, result.denial);
    return;
  }
  authDataResponse(res, { events: result.events });
});

// P08 workspace routes are versioned and isolated from the public snapshot
// routes above. The service provisions from `git archive`, so creating or
// editing a workspace never writes to the authoritative checkout.
function workspaceRequestId(req: Request): string {
  const supplied = String(req.header("x-request-id") || "").trim();
  return supplied && supplied.length <= 120 ? supplied : `tc:req:${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

function workspaceJson<T>(req: Request, data: T, meta: Record<string, unknown> = {}, links: Record<string, string> = {}) {
  return {
    schema_version: WORKSPACE_API_SCHEMA_VERSION,
    request_id: workspaceRequestId(req),
    data,
    meta,
    links,
  };
}

function workspaceErrorResponse(req: Request, res: Response, error: unknown, workspaceId?: string): void {
  const serviceFailure = error instanceof WorkspaceServiceError
    ? error
    : new WorkspaceServiceError({ status: 500, code: "workspace_internal_error", message: "The workspace operation could not be completed." });
  if (workspaceId && serviceFailure.status >= 400) {
    try {
      workspaceService.recordDenied(workspaceId, undefined, serviceFailure.code, { status: serviceFailure.status });
    } catch {
      // A failed audit write must not replace the structured API error.
    }
  }
  res.status(serviceFailure.status).json({
    schema_version: WORKSPACE_API_SCHEMA_VERSION,
    request_id: workspaceRequestId(req),
    error: {
      code: serviceFailure.code,
      message: serviceFailure.message,
      ...(Object.keys(serviceFailure.details).length ? { details: serviceFailure.details } : {}),
    },
  });
}

function workspacePermissionAction(scope: WorkspaceScope): PermissionAction {
  if (scope === "workspace:read" || scope === "workspace:context" || scope === "task:read") return "read";
  if (scope === "workspace:edit") return "edit";
  if (scope === "workspace:test") return "test";
  if (scope === "task:create") return "workspace";
  return "workspace";
}

function workspaceActorFromDecision(decision: ReturnType<AuthStore["authorize"]>): WorkspaceActor {
  if (!decision.allowed || !decision.actor || !decision.credential) {
    throw new Error("workspace authorization did not return an actor");
  }
  const actionScopes: Record<PermissionAction, WorkspaceScope[]> = {
    read: ["workspace:read", "workspace:context", "task:read"],
    workspace: ["workspace:create", "workspace:handoff", "workspace:destroy", "task:create"],
    edit: ["workspace:edit"],
    test: ["workspace:test"],
    benchmark: ["workspace:test"],
    artifact: ["workspace:context"],
    branch: ["workspace:edit"],
    commit: ["workspace:edit"],
    draft_pr: ["workspace:edit"],
    push: ["workspace:edit"],
    review: ["workspace:read"],
    merge: ["workspace:destroy"],
  };
  const scopes = [...new Set(decision.credential.actions.flatMap((action) => actionScopes[action] || []))];
  return {
    actor_id: decision.actor.id,
    kind: decision.actor.kind,
    scopes,
  };
}

function workspaceAuthorization(req: Request, res: Response, scope: WorkspaceScope, workspaceId?: string): { actor: WorkspaceActor; record?: ReturnType<typeof workspaceService.get> } | null {
  const decision = authStore.authorize({
    token: authToken(req),
    action: workspacePermissionAction(scope),
    project_id: requestProject(req, req.body),
    task_id: requestTask(req, req.body),
    request_nonce: requestNonce(req),
    resource: req.path,
    require_nonce: false,
  });
  if (!decision.allowed) {
    res.status(decision.denial.http_status).json({
      schema_version: WORKSPACE_API_SCHEMA_VERSION,
      request_id: workspaceRequestId(req),
      error: {
        code: decision.denial.code,
        message: decision.denial.reason,
        action: decision.denial.action,
        audit_event_id: decision.denial.audit_event_id,
        retryable: decision.denial.retryable,
      },
    });
    return null;
  }
  try {
    const actor = workspaceActorFromDecision(decision);
    if (workspaceId) {
      const record = workspaceService.get(workspaceId);
      workspaceService.requireAccess(record, actor, scope);
      return { actor, record };
    }
    return { actor };
  } catch (error) {
    if (workspaceId) {
      try { workspaceService.recordDenied(workspaceId, decision.actor?.id, error instanceof WorkspaceServiceError ? error.code : "authorization_failed", { required_scope: scope }); } catch { /* Best-effort audit. */ }
    }
    workspaceErrorResponse(req, res, error, workspaceId);
    return null;
  }
}

function workspaceAccess(req: Request, res: Response, workspaceId: string, scope: WorkspaceScope): { actor: WorkspaceActor; record: ReturnType<typeof workspaceService.get> } | null {
  return workspaceAuthorization(req, res, scope, workspaceId) as { actor: WorkspaceActor; record: ReturnType<typeof workspaceService.get> } | null;
}

function registerWorkspaceApi(prefix: string, collectionPrefix = `${prefix}/workspaces`): void {
  if (prefix !== collectionPrefix) {
    app.get(prefix, (req: Request, res: Response) => {
      res.json(workspaceJson(req, workspaceService.capabilities(), { public: false }));
    });
  }

  app.get(`${prefix}/capabilities`, (req: Request, res: Response) => {
    res.json(workspaceJson(req, workspaceService.capabilities(), { public: false }));
  });

  app.get(`${prefix}/openapi.json`, (_req: Request, res: Response) => {
    const candidates = [
      path.resolve(__dirname, "..", "docs", "11_TreatCode_Platform", "schemas", "workspace_api.v1.openapi.json"),
      path.resolve(process.cwd(), "docs", "11_TreatCode_Platform", "schemas", "workspace_api.v1.openapi.json"),
    ];
    for (const candidate of candidates) {
      try {
        res.json(JSON.parse(fs.readFileSync(candidate, "utf8")));
        return;
      } catch {
        // Continue to the next source location.
      }
    }
    res.status(404).json({ schema_version: WORKSPACE_API_SCHEMA_VERSION, error: { code: "contract_unavailable", message: "The workspace API contract is unavailable." } });
  });

  app.get(collectionPrefix, (req: Request, res: Response) => {
    const authorization = workspaceAuthorization(req, res, "workspace:read");
    if (!authorization) return;
    try {
      const workspaces = workspaceService.list(authorization.actor);
      res.json(workspaceJson(req, workspaces, { count: workspaces.length }, { self: collectionPrefix }));
    } catch (error) {
      workspaceErrorResponse(req, res, error);
    }
  });

  app.post(collectionPrefix, (req: Request, res: Response) => {
    const authorization = workspaceAuthorization(req, res, "workspace:create");
    if (!authorization) return;
    try {
      const workspace = workspaceService.createWorkspace((req.body || {}) as Record<string, unknown>, authorization.actor);
      res.status(201).json(workspaceJson(req, workspace, { operation: "create" }, { self: `${collectionPrefix}/${encodeURIComponent(workspace.id)}` }));
    } catch (error) {
      workspaceErrorResponse(req, res, error);
    }
  });

  app.get(`${collectionPrefix}/:workspaceId`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:read");
    if (!access) return;
    res.json(workspaceJson(req, workspaceService.publicRecord(access.record), { operation: "inspect" }, { self: `${collectionPrefix}/${encodeURIComponent(access.record.id)}` }));
  });

  app.get(`${collectionPrefix}/:workspaceId/files`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:read");
    if (!access) return;
    try {
      const requestedPath = String(req.query.path || "").trim();
      const data = requestedPath ? workspaceService.readFile(access.record, requestedPath) : workspaceService.listFiles(access.record);
      res.json(workspaceJson(req, data, { operation: requestedPath ? "read-file" : "list-files" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/files`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:edit");
    if (!access) return;
    try {
      const body = (req.body || {}) as Record<string, unknown>;
      const relativePath = String(body.path || "");
      const content = typeof body.content === "string" ? body.content : "";
      const result = workspaceService.updateFile(access.record, access.actor, relativePath, content, typeof body.expected_version === "number" ? body.expected_version : undefined);
      res.json(workspaceJson(req, result, { operation: "write-file" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.get(`${collectionPrefix}/:workspaceId/snapshots`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:read");
    if (!access) return;
    res.json(workspaceJson(req, access.record.snapshots, { count: access.record.snapshots.length }));
  });

  app.post(`${collectionPrefix}/:workspaceId/snapshots`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:edit");
    if (!access) return;
    try {
      const snapshot = workspaceService.createSnapshot(access.record, access.actor, (req.body || {}).label);
      res.status(201).json(workspaceJson(req, snapshot, { operation: "snapshot" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/snapshots/:snapshotId/restore`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:edit");
    if (!access) return;
    try {
      const snapshot = workspaceService.restoreSnapshot(access.record, access.actor, req.params.snapshotId);
      res.json(workspaceJson(req, snapshot, { operation: "restore" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/disconnect`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:read");
    if (!access) return;
    try {
      const body = (req.body || {}) as Record<string, unknown>;
      const checkpoint = body.checkpoint ? workspaceService.createSnapshot(access.record, access.actor, body.label || "Disconnect checkpoint") : null;
      const workspace = workspaceService.disconnect(access.record, access.actor);
      res.json(workspaceJson(req, { workspace, checkpoint }, { operation: "disconnect", persistent_state: true }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/resume`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:read");
    if (!access) return;
    try {
      const workspace = workspaceService.resume(access.record, access.actor);
      res.json(workspaceJson(req, workspace, { operation: "resume", recovered: true }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.get(`${collectionPrefix}/:workspaceId/export`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:read");
    if (!access) return;
    try {
      res.json(workspaceJson(req, workspaceService.exportWorkspace(access.record, access.actor), { operation: "export" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/export`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:read");
    if (!access) return;
    try {
      res.json(workspaceJson(req, workspaceService.exportWorkspace(access.record, access.actor), { operation: "export" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/checks`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:test");
    if (!access) return;
    try {
      const body = (req.body || {}) as Record<string, unknown>;
      res.json(workspaceJson(req, workspaceService.runCheck(access.record, access.actor, String(body.kind || "doctor")), { operation: "check" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/handoff`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:handoff");
    if (!access) return;
    try {
      const body = (req.body || {}) as Record<string, unknown>;
      res.json(workspaceJson(req, workspaceService.handoff(access.record, access.actor, String(body.to_actor_id || body.recipient_id || body.target_actor_id || ""), body.scopes), { operation: "handoff" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.delete(`${collectionPrefix}/:workspaceId/collaborators/:actorId`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:handoff");
    if (!access) return;
    try {
      res.json(workspaceJson(req, workspaceService.revokeCollaborator(access.record, access.actor, req.params.actorId), { operation: "handoff-revoke" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.get(`${collectionPrefix}/:workspaceId/audit`, (req: Request, res: Response) => {
    const authorization = workspaceAuthorization(req, res, "workspace:read");
    if (!authorization) return;
    try {
      const record = workspaceService.get(req.params.workspaceId);
      workspaceService.requireAuditAccess(record, authorization.actor);
      const audit = workspaceService.auditForWorkspace(record.id);
      res.json(workspaceJson(req, audit, { count: audit.length }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, req.params.workspaceId);
    }
  });

  app.get(`${collectionPrefix}/:workspaceId/tasks`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "task:read");
    if (!access) return;
    res.json(workspaceJson(req, access.record.tasks, { count: access.record.tasks.length }));
  });

  app.post(`${collectionPrefix}/:workspaceId/tasks`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "task:create");
    if (!access) return;
    try {
      res.status(201).json(workspaceJson(req, workspaceService.createTask(access.record, access.actor, (req.body || {}) as Record<string, unknown>), { operation: "task-create" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.get(`${collectionPrefix}/:workspaceId/tasks/:taskId`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "task:read");
    if (!access) return;
    try {
      res.json(workspaceJson(req, workspaceService.task(access.record, req.params.taskId), { operation: "task-inspect" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/tasks/:taskId/approve`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "task:create");
    if (!access) return;
    try {
      res.json(workspaceJson(req, workspaceService.approveTask(access.record, access.actor, req.params.taskId), { operation: "task-approve" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/tasks/:taskId/context-package`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:context");
    if (!access) return;
    try {
      workspaceService.task(access.record, req.params.taskId);
      const body = (req.body || {}) as Record<string, unknown>;
      res.json(workspaceJson(req, workspaceService.contextPackage(access.record, access.actor, body.scopes || body.scope, req.params.taskId, body.limits as Record<string, unknown> | undefined), { operation: "context-package" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/context-package`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:context");
    if (!access) return;
    try {
      const body = (req.body || {}) as Record<string, unknown>;
      res.json(workspaceJson(req, workspaceService.contextPackage(access.record, access.actor, body.scopes || body.scope, typeof body.task_id === "string" ? body.task_id : undefined, body.limits as Record<string, unknown> | undefined), { operation: "context-package" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/destroy`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:destroy");
    if (!access) return;
    try {
      res.json(workspaceJson(req, workspaceService.destroy(access.record, access.actor), { operation: "destroy", access_revoked: true }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.delete(`${collectionPrefix}/:workspaceId`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:destroy");
    if (!access) return;
    try {
      res.json(workspaceJson(req, workspaceService.destroy(access.record, access.actor), { operation: "destroy", access_revoked: true }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });
}


// Operations is a private, stateful control plane separate from the public snapshot API.
const operationsStatePath = process.env.TREATCODE_OPERATIONS_STATE || null;
const operationsStore = new OperationsStore({ filePath: operationsStatePath });

function operationsActor(req: Request): OperationActor {
  const bodyActor = req.body && typeof req.body.actor === "object" && req.body.actor ? req.body.actor : {};
  return {
    id: String(bodyActor.id || req.header("x-treatcode-actor") || "operations-console"),
    permission: String(bodyActor.permission || req.header("x-treatcode-permission") || "operations:write"),
    commit: String(bodyActor.commit || req.header("x-treatcode-commit") || "working-tree"),
  };
}

function operationsError(res: Response, error: unknown): void {
  const code = String((error as Error)?.message || "OPERATIONS_ERROR");
  const status = code === "TASK_NOT_FOUND" || code === "NOTIFICATION_NOT_FOUND" ? 404 : code === "APPROVAL_REQUIRED" ? 409 : 400;
  res.status(status).json({ schemaVersion: "treatcode.operations.v1", error: { code, message: code.replace(/_/g, " ").toLowerCase() } });
}

function registerOperationsApi(): void {
  app.get("/api/operations", (_req: Request, res: Response) => {
    res.json({ schemaVersion: "treatcode.operations.v1", service: "operations", links: { overview: "/api/operations/overview", events: "/api/operations/events", health: "/api/operations/health" } });
  });

  app.get("/api/operations/overview", (_req: Request, res: Response) => {
    res.json(operationsStore.overview());
  });

  app.get("/api/operations/health", (_req: Request, res: Response) => {
    const health = operationsStore.health();
    res.status(health.ok ? 200 : 503).json(health);
  });

  app.get("/api/operations/policies", (_req: Request, res: Response) => {
    res.json(operationsStore.overview().policies);
  });

  app.get("/api/operations/tasks", (req: Request, res: Response) => {
    const status = String(req.query.status || "").trim();
    const tasks = operationsStore.listTasks().filter((task) => !status || task.status === status);
    res.json({ schemaVersion: "treatcode.operations.v1", data: tasks, meta: { count: tasks.length } });
  });

  app.post("/api/operations/tasks", (req: Request, res: Response) => {
    try {
      const task = operationsStore.createTask((req.body || {}) as TaskInput, operationsActor(req));
      res.status(201).json(task);
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.get("/api/operations/tasks/:taskId", (req: Request, res: Response) => {
    try {
      res.json(operationsStore.getTask(req.params.taskId));
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.post("/api/operations/tasks/:taskId/actions", (req: Request, res: Response) => {
    try {
      const action = String(req.body?.action || "").trim();
      if (!action) {
        res.status(400).json({ schemaVersion: "treatcode.operations.v1", error: { code: "ACTION_REQUIRED", message: "an action is required" } });
        return;
      }
      res.json(operationsStore.action(req.params.taskId, action, operationsActor(req)));
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.post("/api/operations/sessions", (req: Request, res: Response) => {
    try {
      const task = operationsStore.connectClient(req.body?.clientId, req.body?.taskId, operationsActor(req));
      res.json({ connected: true, task });
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.delete("/api/operations/sessions/:clientId", (req: Request, res: Response) => {
    try {
      res.json({ connected: false, task: operationsStore.disconnectClient(req.params.clientId, operationsActor(req)) });
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.post("/api/operations/sessions/:clientId/resume", (req: Request, res: Response) => {
    try {
      res.json({ connected: true, resumed: true, task: operationsStore.resumeClient(req.params.clientId, req.body?.taskId, operationsActor(req)) });
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.get("/api/operations/events", (req: Request, res: Response) => {
    res.json({ schemaVersion: "treatcode.operations.v1", data: operationsStore.eventsSince(req.query.since), meta: { since: Number(req.query.since || 0) || 0 } });
  });

  app.get("/api/operations/stream", (req: Request, res: Response) => {
    res.status(200).set({ "Content-Type": "text/event-stream", "Cache-Control": "no-cache", Connection: "keep-alive" });
    let sequence = Number(req.query.since || 0) || 0;
    const writeEvents = () => {
      const events = operationsStore.eventsSince(sequence);
      for (const event of events) {
        sequence = event.sequence;
        res.write(`event: operations\\ndata: ${JSON.stringify(event)}\\n\\n`);
      }
      res.write(`event: heartbeat\\ndata: ${JSON.stringify({ at: new Date().toISOString(), sequence })}\\n\\n`);
    };
    writeEvents();
    const timer = setInterval(writeEvents, 5000);
    req.on("close", () => clearInterval(timer));
  });

  app.get("/api/operations/notifications", (req: Request, res: Response) => {
    const notifications = operationsStore.overview().notifications.filter((notification) => req.query.unread === "true" ? !notification.read : true);
    res.json({ schemaVersion: "treatcode.operations.v1", data: notifications, meta: { count: notifications.length } });
  });

  app.post("/api/operations/notifications/:notificationId/read", (req: Request, res: Response) => {
    try {
      res.json(operationsStore.markNotificationRead(req.params.notificationId, operationsActor(req)));
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.patch("/api/operations/notifications/preferences", (req: Request, res: Response) => {
    try {
      res.json(operationsStore.setNotificationPreferences(req.body || {}, operationsActor(req)));
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.get("/api/operations/audit", (req: Request, res: Response) => {
    const limit = Math.max(1, Math.min(100, Number(req.query.limit || 50) || 50));
    res.json({ schemaVersion: "treatcode.operations.v1", data: operationsStore.overview().audit.slice(0, limit), meta: { count: limit } });
  });

  app.post("/api/operations/backup", (req: Request, res: Response) => {
    try {
      res.status(201).json(operationsStore.backup(operationsActor(req)));
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.post("/api/operations/restore", (req: Request, res: Response) => {
    try {
      res.json(operationsStore.restore((req.body?.backup || req.body) as OperationsBackup, operationsActor(req)));
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.post("/api/operations/recovery-exercise", (_req: Request, res: Response) => {
    const report = operationsStore.disasterRecoveryExercise();
    res.status(report.ok ? 200 : 503).json(report);
  });
}

registerOperationsApi();
const operationsTicker = setInterval(() => operationsStore.tick(), 1000);
operationsTicker.unref?.();

const executionQueue = new SecureExecutionQueue({
  artifactRoot: process.env.TREATCODE_RUNNER_ARTIFACT_ROOT || path.resolve(process.cwd(), "..", "build", "treatcode-runner-evidence"),
  repositoryRoot: path.resolve(__dirname, ".."),
  workerPath: path.join(__dirname, "src", "runner", "worker.ts"),
  concurrency: 2,
  maxRetries: 1,
});

// Contribution intake is deliberately separate from the public snapshot API.
// Upload bytes stay in the service's quarantine/workspace boundary and the
// GitHub adapter receives only validated file metadata and immutable evidence.
const contributionGitHub = new InMemoryGitHubApp();
const contributionService = new ContributionService({ github: contributionGitHub });

function contributionActor(req: Request): ContributionActor {
  const rawScopes = String(req.header("x-treatcode-scopes") || "").split(",").map((scope) => scope.trim()).filter(Boolean);
  const scopes = rawScopes.filter((scope): scope is ContributionAction => (CONTRIBUTION_ACTIONS as readonly string[]).includes(scope));
  const actorType = String(req.header("x-treatcode-actor-type") || "agent");
  return {
    actorId: String(req.header("x-treatcode-actor") || "anonymous"),
    actorType: actorType === "human" || actorType === "service" ? actorType : "agent",
    scopes,
    taskId: req.header("x-treatcode-task") || undefined,
    projectId: req.header("x-treatcode-project") || undefined,
    tokenId: req.header("x-treatcode-token") || undefined,
    expiresAt: req.header("x-treatcode-expires-at") || undefined,
  };
}

function contributionErrorResponse(res: Response, error: unknown): void {
  if (error instanceof ContributionError) {
    res.status(error.status).json({ schema_version: "treatcode.contribution.v1", error: { code: error.code, message: error.message, details: error.details } });
    return;
  }
  res.status(500).json({ schema_version: "treatcode.contribution.v1", error: { code: "CONTRIBUTION_INTERNAL_ERROR", message: "Contribution request failed." } });
}

function contributionHandler(handler: (req: Request, res: Response) => void | Promise<void>) {
  return (req: Request, res: Response) => {
    Promise.resolve(handler(req, res)).catch((error) => contributionErrorResponse(res, error));
  };
}

app.get("/api/contributions/capabilities", (_req: Request, res: Response) => {
  res.json(contributionService.capabilities());
});

app.post("/api/contributions/uploads", contributionHandler((req, res) => {
  const session = contributionService.createUploadSession(contributionActor(req), req.body || {});
  res.status(201).json(session);
}));

app.get("/api/contributions/uploads/:uploadId", contributionHandler((req, res) => {
  res.json(contributionService.getUploadSession(contributionActor(req), req.params.uploadId));
}));

app.put("/api/contributions/uploads/:uploadId/chunks", contributionHandler((req, res) => {
  const encoded = String(req.body?.dataBase64 || "");
  if (!/^[A-Za-z0-9+/]*={0,2}$/.test(encoded) || encoded.length === 0) throw new ContributionError("INVALID_CHUNK", "Chunk dataBase64 is required.", 422);
  const bytes = Buffer.from(encoded, "base64");
  const session = contributionService.putUploadChunk(contributionActor(req), req.params.uploadId, Number(req.body?.offset), bytes);
  res.json(session);
}));

app.post("/api/contributions/uploads/:uploadId/finalize", contributionHandler((req, res) => {
  res.json(contributionService.finalizeUpload(contributionActor(req), req.params.uploadId));
}));

app.post("/api/contributions/workspaces", contributionHandler((req, res) => {
  const workspace = contributionService.createWorkspace(contributionActor(req), req.body || {});
  res.status(201).json(workspace);
}));

app.get("/api/contributions/workspaces/:workspaceId", contributionHandler((req, res) => {
  res.json(contributionService.getWorkspace(contributionActor(req), req.params.workspaceId));
}));

app.post("/api/contributions/workspaces/:workspaceId/uploads/:uploadId", contributionHandler((req, res) => {
  res.json(contributionService.materializeUpload(contributionActor(req), req.params.workspaceId, req.params.uploadId, req.body?.targetPath));
}));

app.post("/api/contributions/workspaces/:workspaceId/validate", contributionHandler((req, res) => {
  res.json(contributionService.validateWorkspace(contributionActor(req), req.params.workspaceId));
}));

app.post("/api/contributions/workspaces/:workspaceId/evidence", contributionHandler((req, res) => {
  res.status(201).json(contributionService.attachEvidence(contributionActor(req), req.params.workspaceId, req.body || {}));
}));

app.post("/api/contributions/workspaces/:workspaceId/approval", contributionHandler((req, res) => {
  res.status(201).json(contributionService.recordHumanApproval(contributionActor(req), req.params.workspaceId, req.body || {}));
}));

app.post("/api/contributions/workspaces/:workspaceId/draft-pr", contributionHandler(async (req, res) => {
  res.status(201).json(await contributionService.submitDraftPullRequest(contributionActor(req), req.params.workspaceId, req.body || {}));
}));

// In-memory databases
interface LeaderboardEntry {
  id: number;
  problemId: string;
  name: string;
  engine: "native" | "bootstrap";
  cycles: number;
  date: string;
}

// Leaderboard state is populated only by accepted submissions in this server
// process.  Keeping the initial collection empty prevents demo records from
// being presented as authoritative public performance statistics.
const leaderboard: LeaderboardEntry[] = [];

interface TestCase {
  arg: any;
  expectedOutput?: string;
  expectedR13?: number;
}

interface ChallengeTestCase {
  arg: any;
  expected_output?: string;
  expected_r13?: number;
}

interface Challenge {
  id: string;
  title: string;
  lifecycle: "published" | "draft" | "retired";
  difficulty: "easy" | "medium" | "hard";
  category: string;
  tags: string[];
  facets: Record<string, string[]>;
  description: string;
  signature: string;
  template: string;
  stats: { solved: boolean; submissions: number; acceptance: number; points: number };
  execution: {
    mode: "verified" | "compile-only";
    runner: string;
    limits: { time_ms: number; memory_kib: number; max_cycles: number; max_output_bytes: number };
    correctness?: {
      kind: string;
      test_cases: ChallengeTestCase[];
      wrapper: { kind: "call" | "string-buffer"; template: string };
    };
    cost_probe?: Record<string, unknown>;
  };
  source_refs: Array<{ path: string; symbol: string }>;
}

interface GeneratedChallengeData {
  schema: string;
  manifest_schema: string;
  manifest_version: number;
  source_of_truth: string;
  facet_dimensions: Record<string, string[]>;
  challenges: Challenge[];
}

function loadChallenges(): Challenge[] {
  const candidates = [
    path.join(__dirname, "src", "generated", "challenges.server.json"),
    path.join(process.cwd(), "src", "generated", "challenges.server.json"),
    path.join(process.cwd(), "treatcode", "src", "generated", "challenges.server.json"),
  ];
  for (const candidate of candidates) {
    try {
      const data = JSON.parse(fs.readFileSync(candidate, "utf8")) as GeneratedChallengeData;
      if (data.schema === "treatcode.challenge_data.v1" && Array.isArray(data.challenges)) return data.challenges;
    } catch {
      // Try the next generated-data location.
    }
  }
  throw new Error("Generated challenge data is unavailable; run npm run generate:challenges.");
}

const challenges = loadChallenges();
const challengeById = new Map(challenges.map((challenge) => [challenge.id, challenge]));

function challengeTestCases(challenge: Challenge): TestCase[] {
  return (challenge.execution.correctness?.test_cases || []).map((testCase) => ({
    arg: testCase.arg,
    expectedOutput: testCase.expected_output,
    expectedR13: testCase.expected_r13,
  }));
}

function renderChallengeWrapper(challenge: Challenge, code: string, arg: any): string {
  const wrapper = challenge.execution.correctness?.wrapper;
  if (!wrapper) throw new Error(`Challenge ${challenge.id} has no correctness wrapper.`);
  const values: Record<string, string> = {
    "${code}": code,
    "${arg}": String(arg),
  };
  if (wrapper.kind === "string-buffer") {
    const input = String(arg);
    values["${arg_length}"] = String(input.length);
    values["${arg_stores}"] = Array.from(input)
      .map((character, index) => `store(p + ${index}, ${character.charCodeAt(0)});`)
      .join("\n                ");
  }
  let rendered = wrapper.template;
  for (const [token, value] of Object.entries(values)) rendered = rendered.split(token).join(value);
  return rendered;
}

function clientChallenge(challenge: Challenge) {
  return {
    id: challenge.id,
    title: challenge.title,
    lifecycle: challenge.lifecycle,
    difficulty: challenge.difficulty,
    category: challenge.category,
    tags: challenge.tags,
    facets: challenge.facets,
    description: challenge.description,
    signature: challenge.signature,
    template: challenge.template,
    stats: challenge.stats,
    ...challenge.stats,
    execution: {
      mode: challenge.execution.mode,
      runner: challenge.execution.runner,
      limits: challenge.execution.limits,
    },
  };
}

async function compileAndRunTrit(
  code: string,
  engine: RunnerEngine,
  optLevel = "-O2",
): Promise<ExecutionResult> {
  const normalizedOptLevel = /^-O[0-3]$/.test(optLevel) ? optLevel as "-O0" | "-O1" | "-O2" | "-O3" : "-O2";
  return executionQueue.submit({
    schema: EXECUTION_REQUEST_SCHEMA,
    kind: "trit.compile-and-run",
    code,
    engine,
    optLevel: normalizedOptLevel,
    sourceCommit: publicSnapshot.snapshot.commit || "working-tree",
  }).result;
}

// REST Routes
function normalizeRunnerEngine(value: unknown): RunnerEngine | null {
  return value === "native" || value === "bootstrap" ? value : null;
}

app.get("/api/problems", (req: Request, res: Response) => {
  // Retired entries remain in the manifest for auditability but never enter
  // the active challenge experience. Correctness fixtures stay server-only.
  res.json(challenges.filter((challenge) => challenge.lifecycle !== "retired").map(clientChallenge));
});

app.get("/api/leaderboard", (req: Request, res: Response) => {
  res.json(leaderboard);
});

app.get("/api/runs/:runId", async (req: Request, res: Response) => {
  const decision = requireAction(req, res, "read", { require_nonce: false });
  if (!decision) return;
  const runId = String(req.params.runId || "");
  const record = await executionQueue.getRecord(runId);
  if (record) {
    res.json(record);
    return;
  }
  const status = executionQueue.status(runId);
  if (!status) {
    res.status(404).json({ error: "Run not found" });
    return;
  }
  res.status(202).json(status);
});

app.post("/api/run", async (req: Request, res: Response) => {
  const decision = requireAction(req, res, "test");
  if (!decision) return;
  const { problemId, code, engine, optLevel } = req.body;
  const runnerEngine = normalizeRunnerEngine(engine);
  if (!runnerEngine) {
    return res.status(400).json({ error: "Runner engine must be native or bootstrap" });
  }
  const challenge = challengeById.get(String(problemId));
  if (!challenge) {
    return res.status(404).json({ error: "Problem not found" });
  }
  if (challenge.lifecycle !== "published") {
    return res.status(409).json({
      success: false,
      code: "challenge_not_published",
      lifecycle: challenge.lifecycle,
      error: "This challenge is not available for verified execution.",
    });
  }

  // Get first test case for manual run
  const testCases = challengeTestCases(challenge);
  if (testCases.length === 0) {
    return res.status(500).json({ success: false, error: "Published challenge has no correctness contract" });
  }
  const testCase = testCases[0];
  const wrappedCode = renderChallengeWrapper(challenge, code, testCase.arg);

  const result = await compileAndRunTrit(wrappedCode, runnerEngine, optLevel);
  res.json(result);
});

app.post("/api/submit", async (req: Request, res: Response) => {
  const editDecision = requireAction(req, res, "edit");
  if (!editDecision) return;
  const testDecision = requireAction(req, res, "test", { require_nonce: false });
  if (!testDecision) return;
  const { problemId, code, engine, username, optLevel } = req.body;
  const runnerEngine = normalizeRunnerEngine(engine);
  if (!runnerEngine) {
    return res.status(400).json({ error: "Runner engine must be native or bootstrap" });
  }
  const challenge = challengeById.get(String(problemId));
  if (!challenge) {
    return res.status(404).json({ error: "Problem not found" });
  }

  if (challenge.lifecycle !== "published") {
    return res.status(409).json({
      success: false,
      code: "challenge_not_published",
      lifecycle: challenge.lifecycle,
      error: "Draft and retired challenges cannot be submitted for verified completion.",
    });
  }

  if (!username || username.trim() === "") {
    return res.status(400).json({ error: "Username is required for submission" });
  }

  const testCases = challengeTestCases(challenge);
  if (testCases.length === 0) {
    return res.status(500).json({ success: false, error: "Published challenge has no correctness contract" });
  }

  const results = [];
  let allPassed = true;
  let totalCycles = 0;

  for (let i = 0; i < testCases.length; i++) {
    const tc = testCases[i];
    const wrappedCode = renderChallengeWrapper(challenge, code, tc.arg);
    const runResult = await compileAndRunTrit(wrappedCode, runnerEngine, optLevel);

    if (!runResult.success) {
      results.push({
        testCase: i + 1,
        passed: false,
        error: runResult.error,
        compilerOutput: runResult.compilerOutput
      });
      allPassed = false;
      continue;
    }

    let passed = false;
    if (tc.expectedOutput !== undefined) {
      const actual = (runResult.consoleOutput || "").replace(/\r\n/g, "\n").trim();
      const expected = tc.expectedOutput.replace(/\r\n/g, "\n").trim();
      passed = actual === expected;
    } else if (tc.expectedR13 !== undefined) {
      passed = runResult.r13 === tc.expectedR13;
    }

    results.push({
      testCase: i + 1,
      passed,
      cycles: runResult.cycles,
      consoleOutput: runResult.consoleOutput,
      r13: runResult.r13,
      registers: runResult.registers
    });

    if (!passed) allPassed = false;
    totalCycles += runResult.cycles || 0;
  }

  // If all tests passed, insert into leaderboard
  if (allPassed) {
    const avgCycles = Math.round(totalCycles / testCases.length);
    const newEntry: LeaderboardEntry = {
      id: leaderboard.length + 1,
      problemId,
      name: username.substring(0, 20),
      engine: runnerEngine,
      cycles: avgCycles,
      date: new Date().toISOString().split("T")[0]
    };
    leaderboard.push(newEntry);
    leaderboard.sort((a, b) => a.cycles - b.cycles);
  }

  res.json({
    success: allPassed,
    results
  });
});

// Fallback to serving the built frontend app for all other routes
app.get("*", (req: Request, res: Response) => {
  const indexHtmlPath = path.join(distPath, "index.html");
  if (fs.existsSync(indexHtmlPath)) {
    res.sendFile(indexHtmlPath);
  } else {
    res.status(404).send("API endpoint or frontend assets not found");
  }
});

if (process.env.TREATCODE_NO_LISTEN !== "1") {
  const server = app.listen(PORT, () => {
    const address = server.address();
    const actualPort = typeof address === "object" && address ? address.port : PORT;
    console.log(`TreatCode Server running at http://localhost:${actualPort}`);
  });
}
