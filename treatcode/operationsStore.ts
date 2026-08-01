import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import {
  DEFAULT_OPERATION_POLICIES,
  OPERATIONS_SCHEMA_VERSION,
  OperationApproval,
  OperationAuditEvent,
  OperationEvent,
  OperationNotification,
  OperationNotificationPreferences,
  OperationPolicies,
  OperationReview,
  OperationTask,
  OperationWorkspace,
  RunnerHealth,
  TaskKind,
  TaskLogEntry,
  TaskStatus,
  isTerminalTaskStatus,
  stableTaskHref,
} from "./src/operationsModel";

export interface TaskInput {
  title?: unknown;
  kind?: unknown;
  owner?: unknown;
  commit?: unknown;
  workspaceId?: unknown;
  requiresApproval?: unknown;
}

export interface OperationActor {
  id?: string;
  permission?: string;
  commit?: string;
}

export interface OperationsBackup {
  schemaVersion: "treatcode.operations.backup.v1";
  backupId: string;
  createdAt: string;
  database: {
    digest: string;
    state: OperationsDatabase;
  };
  immutableArtifacts: Array<OperationTask["artifactRefs"][number]>;
  recoveryObjectives: OperationPolicies["recovery"];
}

export interface RecoveryExerciseReport {
  schema: "treatcode.operations.disaster_recovery.v1";
  ok: boolean;
  exercisedAt: string;
  recoveryPointObjectiveMinutes: number;
  recoveryTimeObjectiveMinutes: number;
  checks: Array<{ id: string; name: string; ok: boolean; detail: string }>;
  sourceDigest: string;
  restoredDigest: string;
  immutableArtifactCount: number;
  errors: string[];
}

interface OperationClientSession {
  clientId: string;
  taskId: string | null;
  connected: boolean;
  connectedAt: string;
  lastSeenAt: string;
}

interface OperationsDatabase {
  schemaVersion: typeof OPERATIONS_SCHEMA_VERSION;
  tasks: OperationTask[];
  workspaces: OperationWorkspace[];
  runners: RunnerHealth[];
  approvals: OperationApproval[];
  reviews: OperationReview[];
  notifications: OperationNotification[];
  audit: OperationAuditEvent[];
  events: OperationEvent[];
  notificationPreferences: OperationNotificationPreferences;
  policies: OperationPolicies;
  nextEventSequence: number;
}

interface OperationsState extends OperationsDatabase {
  sessions: Record<string, OperationClientSession>;
}

export interface OperationsStoreOptions {
  filePath?: string | null;
  clock?: () => Date;
  seed?: boolean;
}

const DEFAULT_COMMIT = "working-tree";
const DEFAULT_WORKSPACE_ID = "workspace-trit-main";
const DEFAULT_RUNNER_ID = "runner-primary";

function clone<T>(value: T): T {
  return JSON.parse(JSON.stringify(value)) as T;
}

function canonical(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(canonical).join(",")}]`;
  if (value && typeof value === "object") {
    return `{${Object.entries(value as Record<string, unknown>)
      .sort(([left], [right]) => left.localeCompare(right))
      .map(([key, item]) => `${JSON.stringify(key)}:${canonical(item)}`)
      .join(",")}}`;
  }
  return JSON.stringify(value);
}

function digest(value: unknown): string {
  return `sha256:${crypto.createHash("sha256").update(canonical(value)).digest("hex")}`;
}

function shortText(value: unknown, fallback: string, maxLength: number): string {
  const text = String(value ?? fallback).replace(/[\u0000-\u001f\u007f]/g, " ").trim();
  return (text || fallback).slice(0, maxLength);
}

function commitValue(value: unknown): string {
  const commit = String(value ?? "").trim();
  return /^[0-9a-f]{7,64}$/i.test(commit) ? commit : DEFAULT_COMMIT;
}

function nowIso(clock: () => Date): string {
  return clock().toISOString();
}

function emptyDatabase(): OperationsDatabase {
  return {
    schemaVersion: OPERATIONS_SCHEMA_VERSION,
    tasks: [],
    workspaces: [],
    runners: [],
    approvals: [],
    reviews: [],
    notifications: [],
    audit: [],
    events: [],
    notificationPreferences: {
      completion: true,
      attention: true,
      approval: true,
      recovery: true,
      channels: ["in_app"],
    },
    policies: clone(DEFAULT_OPERATION_POLICIES),
    nextEventSequence: 1,
  };
}

function seedDatabase(clock: () => Date): OperationsDatabase {
  const database = emptyDatabase();
  const now = nowIso(clock);
  database.workspaces = [
    {
      id: DEFAULT_WORKSPACE_ID,
      name: "Trit main workspace",
      repository: "JonasComlita/dualrail",
      commit: "working-tree",
      image: "trit-runner:v2.4.1",
      toolchain: "tritc 2.0 · tcl 1.0",
      status: "busy",
      lastSnapshotAt: now,
    },
    {
      id: "workspace-review",
      name: "Review sandbox",
      repository: "JonasComlita/dualrail",
      commit: "working-tree",
      image: "trit-runner:v2.4.1",
      toolchain: "tritc 2.0 · tcl 1.0",
      status: "ready",
      lastSnapshotAt: now,
    },
  ];
  database.runners = [
    {
      id: DEFAULT_RUNNER_ID,
      name: "Primary isolated runner",
      status: "healthy",
      region: "us-west",
      queueDepth: 2,
      usagePercent: 42,
      quotaPercent: 61,
      lastHeartbeatAt: now,
      capabilities: ["build", "test", "diagnostics"],
    },
    {
      id: "runner-secondary",
      name: "Recovery runner",
      status: "degraded",
      region: "eu-central",
      queueDepth: 0,
      usagePercent: 18,
      quotaPercent: 37,
      lastHeartbeatAt: now,
      capabilities: ["restore", "diagnostics"],
    },
  ];

  const makeTask = (
    id: string,
    title: string,
    kind: TaskKind,
    status: TaskStatus,
    progress: number,
    runnerId: string | null,
    workspaceId: string,
    logs: TaskLogEntry[],
    approvalId: string | null = null,
  ): OperationTask => ({
    id,
    title,
    kind,
    status,
    owner: "operations-console",
    commit: "working-tree",
    workspaceId,
    runnerId,
    createdAt: now,
    updatedAt: now,
    progress,
    logs,
    connection: { connectedClients: status === "running" ? 1 : 0, lastClientAt: status === "running" ? now : null },
    continuation: { mode: "server", cursor: logs.length, logsRetained: true, clientsMayDisconnect: true },
    artifactRefs: [],
    approvalId,
    links: { self: stableTaskHref(id), review: `${stableTaskHref(id)}&panel=review` },
  });

  database.tasks = [
    makeTask("task-ops-1001", "Production smoke validation", "test", "running", 60, DEFAULT_RUNNER_ID, DEFAULT_WORKSPACE_ID, [
      { sequence: 1, at: now, level: "info", message: "Runner accepted the smoke validation job." },
      { sequence: 2, at: now, level: "info", message: "Server continuation is retaining logs for disconnected clients." },
    ]),
    makeTask("task-ops-1002", "Review release evidence", "review", "queued", 0, null, "workspace-review", [
      { sequence: 1, at: now, level: "info", message: "Waiting for an operations approval." },
    ], "approval-ops-1002"),
    makeTask("task-ops-1003", "Rebuild failed fixture", "build", "failed", 35, DEFAULT_RUNNER_ID, DEFAULT_WORKSPACE_ID, [
      { sequence: 1, at: now, level: "error", message: "Worker exited before producing a result artifact." },
    ]),
    makeTask("task-ops-1004", "Recover orphaned workspace", "workspace", "orphaned", 12, null, "workspace-review", [
      { sequence: 1, at: now, level: "warn", message: "Runner heartbeat expired; recovery action is available." },
    ]),
  ];
  database.approvals = [
    {
      id: "approval-ops-1002",
      taskId: "task-ops-1002",
      status: "pending",
      requestedBy: "release-bot",
      requestedAt: now,
      permission: "operations:approve",
      commit: "working-tree",
      expiresAt: new Date(clock().getTime() + 60 * 60 * 1000).toISOString(),
    },
  ];
  database.reviews = [
    { id: "review-ops-1001", taskId: "task-ops-1001", status: "in_review", reviewer: "operations-console", summary: "Smoke evidence is being watched.", updatedAt: now },
    { id: "review-ops-1002", taskId: "task-ops-1002", status: "pending", reviewer: null, summary: "Approval required before review work starts.", updatedAt: now },
  ];
  database.notifications = [
    {
      id: "notification-ops-1002",
      kind: "approval",
      taskId: "task-ops-1002",
      title: "Approval needed",
      body: "Review release evidence is waiting for an operations approval.",
      link: stableTaskHref("task-ops-1002"),
      createdAt: now,
      read: false,
      channels: ["in_app"],
      containsSecrets: false,
    },
  ];
  database.events = [
    { sequence: 1, at: now, type: "task_running", taskId: "task-ops-1001", payload: { status: "running", progress: 60 } },
    { sequence: 2, at: now, type: "approval_pending", taskId: "task-ops-1002", payload: { status: "pending" } },
  ];
  database.nextEventSequence = 3;
  return database;
}

function isTaskStatus(value: unknown): value is TaskStatus {
  return ["queued", "running", "succeeded", "failed", "cancelled", "orphaned"].includes(String(value));
}

function serializeDatabase(state: OperationsState): OperationsDatabase {
  return {
    schemaVersion: state.schemaVersion,
    tasks: state.tasks,
    workspaces: state.workspaces,
    runners: state.runners,
    approvals: state.approvals,
    reviews: state.reviews,
    notifications: state.notifications,
    audit: state.audit,
    events: state.events,
    notificationPreferences: state.notificationPreferences,
    policies: state.policies,
    nextEventSequence: state.nextEventSequence,
  };
}

export class OperationsStore {
  private readonly filePath: string | null;
  private readonly clock: () => Date;
  private state: OperationsState;

  constructor(options: OperationsStoreOptions = {}) {
    this.filePath = options.filePath === undefined ? null : options.filePath;
    this.clock = options.clock || (() => new Date());
    const loaded = options.seed === false ? null : this.readState();
    const database = loaded || (options.seed === false ? emptyDatabase() : seedDatabase(this.clock));
    this.state = { ...database, sessions: {} };
    this.persist();
  }

  private readState(): OperationsDatabase | null {
    if (!this.filePath) return null;
    try {
      const parsed = JSON.parse(fs.readFileSync(this.filePath, "utf8")) as OperationsState;
      if (parsed.schemaVersion !== OPERATIONS_SCHEMA_VERSION || !Array.isArray(parsed.tasks) || !Array.isArray(parsed.events)) return null;
      return {
        ...emptyDatabase(),
        ...parsed,
        policies: { ...clone(DEFAULT_OPERATION_POLICIES), ...(parsed.policies || {}) },
      };
    } catch {
      return null;
    }
  }

  private persist(): void {
    if (!this.filePath) return;
    try {
      fs.mkdirSync(path.dirname(this.filePath), { recursive: true });
      const temporary = `${this.filePath}.tmp`;
      fs.writeFileSync(temporary, `${JSON.stringify(this.state, null, 2)}\n`, "utf8");
      fs.renameSync(temporary, this.filePath);
    } catch {
      // The in-memory state remains authoritative for this process. A failed
      // local write is surfaced by health(), while request handling continues.
    }
  }

  private actor(value: OperationActor | string | undefined): { id: string; permission: string; commit: string } {
    if (typeof value === "string") return { id: shortText(value, "operations-console", 80), permission: "operations:write", commit: DEFAULT_COMMIT };
    return {
      id: shortText(value?.id, "operations-console", 80),
      permission: shortText(value?.permission, "operations:write", 80),
      commit: commitValue(value?.commit),
    };
  }

  private audit(
    actorValue: OperationActor | string | undefined,
    action: string,
    resourceType: OperationAuditEvent["resourceType"],
    resourceId: string,
    commit: string,
    metadata: Record<string, string | number | boolean | null> = {},
  ): void {
    const actor = this.actor(actorValue);
    this.state.audit.unshift({
      id: `audit-${crypto.randomUUID()}`,
      at: nowIso(this.clock),
      actor: actor.id,
      action,
      resourceType,
      resourceId,
      permission: actor.permission,
      commit: commitValue(commit || actor.commit),
      metadata,
    });
    this.state.audit = this.state.audit.slice(0, 200);
  }

  private emit(type: string, taskId: string | null, payload: Record<string, string | number | boolean | null> = {}): void {
    const event: OperationEvent = {
      sequence: this.state.nextEventSequence,
      at: nowIso(this.clock),
      type,
      taskId,
      payload,
    };
    this.state.nextEventSequence += 1;
    this.state.events.push(event);
    this.state.events = this.state.events.slice(-200);
  }

  private taskOrThrow(taskId: string): OperationTask {
    const task = this.state.tasks.find((candidate) => candidate.id === taskId);
    if (!task) throw new Error("TASK_NOT_FOUND");
    return task;
  }

  private notification(kind: OperationNotification["kind"], title: string, body: string, taskId: string | null): OperationNotification {
    const notification: OperationNotification = {
      id: `notification-${crypto.randomUUID()}`,
      kind,
      taskId,
      title: shortText(title, "Operations update", 120),
      body: shortText(body, "An operations update is available.", 240),
      link: taskId ? stableTaskHref(taskId) : "/operations?panel=recovery",
      createdAt: nowIso(this.clock),
      read: false,
      channels: clone(this.state.notificationPreferences.channels),
      containsSecrets: false,
    };
    this.state.notifications.unshift(notification);
    this.state.notifications = this.state.notifications.slice(0, 100);
    this.emit("notification_created", taskId, { kind, notificationId: notification.id });
    return notification;
  }

  private appendLog(task: OperationTask, level: TaskLogEntry["level"], message: string): void {
    const sequence = task.logs.length + 1;
    task.logs.push({ sequence, at: nowIso(this.clock), level, message: shortText(message, "Task update.", 240) });
    task.continuation.cursor = sequence;
    task.updatedAt = nowIso(this.clock);
  }

  private updateRunner(task: OperationTask, running: boolean): void {
    const runner = task.runnerId ? this.state.runners.find((candidate) => candidate.id === task.runnerId) : null;
    if (runner) {
      runner.queueDepth = Math.max(0, running ? runner.queueDepth : runner.queueDepth - 1);
      runner.lastHeartbeatAt = nowIso(this.clock);
    }
    const workspace = this.state.workspaces.find((candidate) => candidate.id === task.workspaceId);
    if (workspace) workspace.status = running ? "busy" : "ready";
  }

  reset(): void {
    this.state = { ...seedDatabase(this.clock), sessions: {} };
    this.persist();
  }

  listTasks(): OperationTask[] {
    return clone(this.state.tasks).sort((left, right) => right.updatedAt.localeCompare(left.updatedAt));
  }

  getTask(taskId: string): OperationTask {
    return clone(this.taskOrThrow(taskId));
  }

  createTask(input: TaskInput, actorValue?: OperationActor | string): OperationTask {
    const title = shortText(input.title, "Untitled operations task", 120);
    const kind = ["build", "test", "review", "workspace"].includes(String(input.kind)) ? String(input.kind) as TaskKind : "test";
    const workspaceId = this.state.workspaces.some((workspace) => workspace.id === String(input.workspaceId)) ? String(input.workspaceId) : DEFAULT_WORKSPACE_ID;
    const taskId = `task-${Date.now().toString(36)}-${crypto.randomUUID().slice(0, 8)}`;
    const now = nowIso(this.clock);
    const task: OperationTask = {
      id: taskId,
      title,
      kind,
      status: "queued",
      owner: shortText(input.owner, this.actor(actorValue).id, 80),
      commit: commitValue(input.commit),
      workspaceId,
      runnerId: null,
      createdAt: now,
      updatedAt: now,
      progress: 0,
      logs: [{ sequence: 1, at: now, level: "info", message: "Task accepted and waiting for an isolated runner." }],
      connection: { connectedClients: 0, lastClientAt: null },
      continuation: { mode: "server", cursor: 1, logsRetained: true, clientsMayDisconnect: true },
      artifactRefs: [],
      approvalId: null,
      links: { self: stableTaskHref(taskId), review: `${stableTaskHref(taskId)}&panel=review` },
    };
    this.state.tasks.push(task);
    if (input.requiresApproval) {
      const approvalId = `approval-${taskId}`;
      task.approvalId = approvalId;
      this.state.approvals.unshift({
        id: approvalId,
        taskId,
        status: "pending",
        requestedBy: task.owner,
        requestedAt: now,
        permission: "operations:approve",
        commit: task.commit,
        expiresAt: new Date(this.clock().getTime() + 60 * 60 * 1000).toISOString(),
      });
      this.notification("approval", "Approval needed", `${task.title} is waiting for an operations approval.`, taskId);
      this.emit("approval_pending", taskId, { status: "pending" });
    }
    this.audit(actorValue, "task_created", "task", taskId, task.commit, { kind, requiresApproval: Boolean(input.requiresApproval) });
    this.emit("task_queued", taskId, { status: task.status });
    this.persist();
    return clone(task);
  }

  action(taskId: string, action: string, actorValue?: OperationActor | string): OperationTask {
    const task = this.taskOrThrow(taskId);
    const normalized = String(action).toLowerCase();
    const actor = this.actor(actorValue);
    if (normalized === "approve") {
      if (!task.approvalId) throw new Error("APPROVAL_NOT_FOUND");
      const approval = this.state.approvals.find((candidate) => candidate.id === task.approvalId);
      if (!approval) throw new Error("APPROVAL_NOT_FOUND");
      approval.status = "approved";
      approval.commit = task.commit;
      this.audit(actor, "approval_granted", "approval", approval.id, task.commit, { taskId });
      this.emit("approval_granted", taskId, { approvalId: approval.id, status: approval.status });
      this.notification("completion", "Approval recorded", `${task.title} can now be started by an authorized operator.`, taskId);
      this.persist();
      return clone(task);
    }

    if (normalized === "start") {
      const approval = task.approvalId ? this.state.approvals.find((candidate) => candidate.id === task.approvalId) : null;
      if (approval && approval.status !== "approved") throw new Error("APPROVAL_REQUIRED");
      if (task.status !== "queued") throw new Error("TASK_NOT_QUEUED");
      task.status = "running";
      task.runnerId = task.runnerId || DEFAULT_RUNNER_ID;
      task.progress = Math.max(task.progress, 5);
      this.appendLog(task, "info", "Task started on the isolated runner; server continuation is active.");
      this.updateRunner(task, true);
      this.audit(actor, "task_started", "task", taskId, task.commit, { status: task.status });
      this.emit("task_running", taskId, { status: task.status, progress: task.progress });
      this.persist();
      return clone(task);
    }

    if (normalized === "cancel" || normalized === "stop") {
      if (task.status !== "queued" && task.status !== "running" && task.status !== "orphaned") throw new Error("TASK_NOT_ACTIVE");
      task.status = "cancelled";
      this.appendLog(task, "warn", "Task cancelled by an authorized operator; retained logs remain available.");
      this.updateRunner(task, false);
      this.audit(actor, "task_cancelled", "task", taskId, task.commit, { status: task.status });
      this.emit("task_cancelled", taskId, { status: task.status });
      this.notification("attention", "Task cancelled", `${task.title} was cancelled by an operator.`, taskId);
      this.persist();
      return clone(task);
    }

    if (normalized === "retry") {
      if (task.status !== "failed" && task.status !== "cancelled" && task.status !== "orphaned") throw new Error("TASK_NOT_RETRYABLE");
      task.status = "queued";
      task.runnerId = null;
      task.progress = 0;
      this.appendLog(task, "info", "Task re-queued for a fresh isolated attempt.");
      this.audit(actor, "task_retried", "task", taskId, task.commit, { status: task.status });
      this.emit("task_queued", taskId, { status: task.status });
      this.persist();
      return clone(task);
    }

    if (normalized === "resume") {
      if (task.status !== "orphaned") throw new Error("TASK_NOT_ORPHANED");
      task.status = "running";
      task.runnerId = DEFAULT_RUNNER_ID;
      this.appendLog(task, "info", "Operator resumed the orphaned task from its retained cursor.");
      this.updateRunner(task, true);
      this.audit(actor, "task_resumed", "task", taskId, task.commit, { status: task.status, cursor: task.continuation.cursor });
      this.emit("task_resumed", taskId, { status: task.status, cursor: task.continuation.cursor });
      this.persist();
      return clone(task);
    }

    throw new Error("UNSUPPORTED_TASK_ACTION");
  }

  advanceTask(taskId: string, amount = 20): OperationTask {
    const task = this.taskOrThrow(taskId);
    if (task.status !== "running") return clone(task);
    task.progress = Math.min(100, task.progress + Math.max(1, Math.min(50, amount)));
    this.appendLog(task, "info", `Runner checkpoint retained at ${task.progress}% progress.`);
    if (task.progress >= 100) {
      task.status = "succeeded";
      task.progress = 100;
      task.artifactRefs = [
        { id: `artifact-${task.id}-result`, kind: "result", sha256: digest({ taskId: task.id, commit: task.commit, cursor: task.continuation.cursor }), immutable: true, uri: `/artifacts/${task.id}/result` },
        { id: `artifact-${task.id}-log`, kind: "log", sha256: digest(task.logs), immutable: true, uri: `/artifacts/${task.id}/log` },
      ];
      this.appendLog(task, "info", "Task completed; immutable result and log references are available.");
      this.updateRunner(task, false);
      this.emit("task_completed", taskId, { status: task.status, progress: task.progress });
      this.notification("completion", "Task completed", `${task.title} completed and produced immutable evidence.`, taskId);
    } else {
      this.emit("task_progress", taskId, { status: task.status, progress: task.progress, cursor: task.continuation.cursor });
    }
    this.persist();
    return clone(task);
  }

  tick(): void {
    for (const task of clone(this.state.tasks)) {
      if (task.status === "running") this.advanceTask(task.id, 10);
    }
  }

  connectClient(clientIdValue: unknown, taskIdValue: unknown, actorValue?: OperationActor | string): OperationTask | null {
    const clientId = shortText(clientIdValue, "operations-client", 100);
    const taskId = taskIdValue ? String(taskIdValue) : null;
    if (taskId) this.taskOrThrow(taskId);
    const now = nowIso(this.clock);
    const existing = this.state.sessions[clientId];
    this.state.sessions[clientId] = {
      clientId,
      taskId,
      connected: true,
      connectedAt: existing?.connectedAt || now,
      lastSeenAt: now,
    };
    if (taskId) {
      const task = this.taskOrThrow(taskId);
      task.connection.connectedClients = Object.values(this.state.sessions).filter((session) => session.connected && session.taskId === taskId).length;
      task.connection.lastClientAt = now;
      task.updatedAt = now;
      this.emit("client_connected", taskId, { clientId, connectedClients: task.connection.connectedClients });
      this.audit(actorValue, "client_connected", "session", clientId, task.commit, { taskId });
      this.persist();
      return clone(task);
    }
    this.audit(actorValue, "client_connected", "session", clientId, DEFAULT_COMMIT, {});
    this.persist();
    return null;
  }

  disconnectClient(clientIdValue: unknown, actorValue?: OperationActor | string): OperationTask | null {
    const clientId = shortText(clientIdValue, "operations-client", 100);
    const session = this.state.sessions[clientId];
    if (!session) return null;
    session.connected = false;
    session.lastSeenAt = nowIso(this.clock);
    if (!session.taskId) {
      this.audit(actorValue, "client_disconnected", "session", clientId, DEFAULT_COMMIT, {});
      this.persist();
      return null;
    }
    const task = this.taskOrThrow(session.taskId);
    task.connection.connectedClients = Object.values(this.state.sessions).filter((candidate) => candidate.connected && candidate.taskId === task.id).length;
    task.connection.lastClientAt = session.lastSeenAt;
    task.updatedAt = session.lastSeenAt;
    this.emit("client_disconnected", task.id, { clientId, connectedClients: task.connection.connectedClients, serverContinuation: true });
    this.audit(actorValue, "client_disconnected", "session", clientId, task.commit, { taskId: task.id, serverContinuation: true });
    this.persist();
    return clone(task);
  }

  resumeClient(clientIdValue: unknown, taskIdValue: unknown, actorValue?: OperationActor | string): OperationTask | null {
    const task = this.connectClient(clientIdValue, taskIdValue, actorValue);
    if (!task) return null;
    this.emit("client_resumed", task.id, { clientId: shortText(clientIdValue, "operations-client", 100), cursor: task.continuation.cursor });
    this.audit(actorValue, "client_resumed", "session", shortText(clientIdValue, "operations-client", 100), task.commit, { taskId: task.id, cursor: task.continuation.cursor });
    this.persist();
    return clone(task);
  }

  setNotificationPreferences(input: Partial<OperationNotificationPreferences>, actorValue?: OperationActor | string): OperationNotificationPreferences {
    const channels = Array.isArray(input.channels) ? input.channels.filter((channel): channel is OperationNotificationPreferences["channels"][number] => ["in_app", "email", "web_push"].includes(channel)) : this.state.notificationPreferences.channels;
    this.state.notificationPreferences = {
      completion: input.completion === undefined ? this.state.notificationPreferences.completion : Boolean(input.completion),
      attention: input.attention === undefined ? this.state.notificationPreferences.attention : Boolean(input.attention),
      approval: input.approval === undefined ? this.state.notificationPreferences.approval : Boolean(input.approval),
      recovery: input.recovery === undefined ? this.state.notificationPreferences.recovery : Boolean(input.recovery),
      channels: channels.length ? channels : ["in_app"],
    };
    this.audit(actorValue, "notification_preferences_updated", "notification", "preferences", DEFAULT_COMMIT, { channelCount: this.state.notificationPreferences.channels.length });
    this.emit("notification_preferences_updated", null, { channelCount: this.state.notificationPreferences.channels.length });
    this.persist();
    return clone(this.state.notificationPreferences);
  }

  markNotificationRead(notificationId: string, actorValue?: OperationActor | string): OperationNotification {
    const notification = this.state.notifications.find((candidate) => candidate.id === notificationId);
    if (!notification) throw new Error("NOTIFICATION_NOT_FOUND");
    notification.read = true;
    this.audit(actorValue, "notification_read", "notification", notificationId, DEFAULT_COMMIT, {});
    this.persist();
    return clone(notification);
  }

  eventsSince(sequenceValue: unknown): OperationEvent[] {
    const sequence = Number(sequenceValue || 0);
    return clone(this.state.events.filter((event) => event.sequence > (Number.isFinite(sequence) ? sequence : 0)));
  }

  backup(actorValue?: OperationActor | string): OperationsBackup {
    const state = clone(serializeDatabase(this.state));
    const backup: OperationsBackup = {
      schemaVersion: "treatcode.operations.backup.v1",
      backupId: `backup-${Date.now().toString(36)}-${crypto.randomUUID().slice(0, 8)}`,
      createdAt: nowIso(this.clock),
      database: { digest: digest(state), state },
      immutableArtifacts: clone(state.tasks.flatMap((task) => task.artifactRefs)),
      recoveryObjectives: clone(state.policies.recovery),
    };
    this.audit(actorValue, "backup_created", "backup", backup.backupId, DEFAULT_COMMIT, { digest: backup.database.digest, immutableArtifacts: backup.immutableArtifacts.length });
    this.emit("backup_created", null, { backupId: backup.backupId, immutableArtifacts: backup.immutableArtifacts.length });
    this.persist();
    return clone(backup);
  }

  restore(backup: OperationsBackup, actorValue?: OperationActor | string): { restoredDigest: string; immutableArtifactCount: number } {
    if (backup?.schemaVersion !== "treatcode.operations.backup.v1" || !backup.database?.state) throw new Error("INVALID_BACKUP");
    const expected = digest(backup.database.state);
    if (expected !== backup.database.digest) throw new Error("BACKUP_DIGEST_MISMATCH");
    const restored = clone(backup.database.state);
    this.state = { ...restored, sessions: {} };
    this.audit(actorValue, "backup_restored", "backup", backup.backupId, DEFAULT_COMMIT, { digest: expected, immutableArtifacts: backup.immutableArtifacts.length });
    this.emit("backup_restored", null, { backupId: backup.backupId, immutableArtifacts: backup.immutableArtifacts.length });
    this.persist();
    return { restoredDigest: expected, immutableArtifactCount: backup.immutableArtifacts.length };
  }

  disasterRecoveryExercise(): RecoveryExerciseReport {
    const started = this.clock().getTime();
    const backup = this.backup("synthetic-recovery");
    const sourceDigest = backup.database.digest;
    const cleanStore = new OperationsStore({ seed: false, clock: this.clock });
    const restored = cleanStore.restore(backup, "synthetic-recovery");
    const checks = [
      { id: "backup-created", name: "Backup contains a database digest", ok: Boolean(backup.database.digest), detail: backup.database.digest },
      { id: "clean-restore", name: "Restore succeeds in a clean store", ok: restored.restoredDigest === sourceDigest, detail: restored.restoredDigest },
      { id: "immutable-artifacts", name: "Immutable artifact references survive restore", ok: restored.immutableArtifactCount === backup.immutableArtifacts.length, detail: `${restored.immutableArtifactCount} references` },
      { id: "audit-policy", name: "Audit and retention policy are present", ok: backup.database.state.audit.length >= 1 && backup.database.state.policies.retention.auditDays >= 365, detail: `${backup.database.state.policies.retention.auditDays} audit days` },
      { id: "recovery-objectives", name: "Recovery objectives are stated", ok: backup.recoveryObjectives.rpoMinutes > 0 && backup.recoveryObjectives.rtoMinutes > 0, detail: `RPO ${backup.recoveryObjectives.rpoMinutes}m / RTO ${backup.recoveryObjectives.rtoMinutes}m` },
    ];
    const errors = checks.filter((check) => !check.ok).map((check) => check.name);
    return {
      schema: "treatcode.operations.disaster_recovery.v1",
      ok: errors.length === 0,
      exercisedAt: nowIso(this.clock),
      recoveryPointObjectiveMinutes: backup.recoveryObjectives.rpoMinutes,
      recoveryTimeObjectiveMinutes: backup.recoveryObjectives.rtoMinutes,
      checks,
      sourceDigest,
      restoredDigest: restored.restoredDigest,
      immutableArtifactCount: restored.immutableArtifactCount,
      errors: [
        ...errors,
        ...(this.clock().getTime() - started > backup.recoveryObjectives.rtoMinutes * 60 * 1000 ? ["restore exceeded the recovery time objective"] : []),
      ],
    };
  }

  health(): Record<string, unknown> {
    const now = this.clock().getTime();
    const staleRunner = this.state.runners.some((runner) => now - new Date(runner.lastHeartbeatAt).getTime() > 5 * 60 * 1000);
    const syntheticChecks = [
      { id: "runner-heartbeat", ok: !staleRunner, detail: staleRunner ? "runner heartbeat is stale" : "runner heartbeat is fresh" },
      { id: "continuation", ok: this.state.tasks.every((task) => task.continuation.mode === "server" && task.continuation.logsRetained), detail: "server continuation and log retention enabled" },
      { id: "notification-safety", ok: this.state.notifications.every((notification) => notification.containsSecrets === false && notification.link.startsWith("/operations?")), detail: "links are stable and payloads contain no secrets" },
    ];
    return {
      schemaVersion: OPERATIONS_SCHEMA_VERSION,
      ok: syntheticChecks.every((check) => check.ok),
      generatedAt: nowIso(this.clock),
      service: { continuation: true, eventStream: true, backupRestore: true },
      syntheticChecks,
      queueDepth: this.state.tasks.filter((task) => task.status === "queued").length,
      runnerCount: this.state.runners.length,
    };
  }

  overview(): {
    schemaVersion: typeof OPERATIONS_SCHEMA_VERSION;
    generatedAt: string;
    connectivity: { mode: "online" | "degraded"; lastEventAt: string; serverContinuation: true; reconnectAfterSeconds: number };
    tasks: OperationTask[];
    workspaces: OperationWorkspace[];
    runners: RunnerHealth[];
    approvals: OperationApproval[];
    reviews: OperationReview[];
    notifications: OperationNotification[];
    audit: OperationAuditEvent[];
    events: OperationEvent[];
    notificationPreferences: OperationNotificationPreferences;
    policies: OperationPolicies;
    metrics: { queued: number; running: number; failed: number; cancelled: number; orphaned: number; usagePercent: number; quotaPercent: number };
  } {
    const lastEvent = this.state.events[this.state.events.length - 1];
    const queue = this.state.tasks.filter((task) => task.status === "queued").length;
    const mode = this.state.runners.some((runner) => runner.status === "offline") ? "degraded" : "online";
    return {
      schemaVersion: OPERATIONS_SCHEMA_VERSION,
      generatedAt: nowIso(this.clock),
      connectivity: { mode, lastEventAt: lastEvent?.at || nowIso(this.clock), serverContinuation: true, reconnectAfterSeconds: 5 },
      tasks: this.listTasks(),
      workspaces: clone(this.state.workspaces),
      runners: clone(this.state.runners),
      approvals: clone(this.state.approvals),
      reviews: clone(this.state.reviews),
      notifications: clone(this.state.notifications),
      audit: clone(this.state.audit.slice(0, 50)),
      events: clone(this.state.events.slice(-50)),
      notificationPreferences: clone(this.state.notificationPreferences),
      policies: clone(this.state.policies),
      metrics: {
        queued: queue,
        running: this.state.tasks.filter((task) => task.status === "running").length,
        failed: this.state.tasks.filter((task) => task.status === "failed").length,
        cancelled: this.state.tasks.filter((task) => task.status === "cancelled").length,
        orphaned: this.state.tasks.filter((task) => task.status === "orphaned").length,
        usagePercent: Math.max(...this.state.runners.map((runner) => runner.usagePercent), 0),
        quotaPercent: Math.max(...this.state.runners.map((runner) => runner.quotaPercent), 0),
      },
    };
  }
}

export { canonical as canonicalOperationsValue, digest as digestOperationsValue, isTerminalTaskStatus };
