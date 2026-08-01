export const OPERATIONS_SCHEMA_VERSION = "treatcode.operations.v1" as const;

export type TaskStatus = "queued" | "running" | "succeeded" | "failed" | "cancelled" | "orphaned";
export type TaskKind = "build" | "test" | "review" | "workspace";
export type RunnerStatus = "healthy" | "degraded" | "offline";
export type ApprovalStatus = "pending" | "approved" | "rejected" | "expired";

export interface TaskLogEntry {
  sequence: number;
  at: string;
  level: "info" | "warn" | "error";
  message: string;
}

export interface ArtifactReference {
  id: string;
  kind: "log" | "trace" | "diagnostic" | "result";
  sha256: string;
  immutable: true;
  uri: string;
}

export interface OperationTask {
  id: string;
  title: string;
  kind: TaskKind;
  status: TaskStatus;
  owner: string;
  commit: string;
  workspaceId: string;
  runnerId: string | null;
  createdAt: string;
  updatedAt: string;
  progress: number;
  logs: TaskLogEntry[];
  connection: {
    connectedClients: number;
    lastClientAt: string | null;
  };
  continuation: {
    mode: "server";
    cursor: number;
    logsRetained: true;
    clientsMayDisconnect: true;
  };
  artifactRefs: ArtifactReference[];
  approvalId: string | null;
  links: {
    self: string;
    review: string;
  };
}

export interface OperationWorkspace {
  id: string;
  name: string;
  repository: string;
  commit: string;
  image: string;
  toolchain: string;
  status: "ready" | "busy" | "stopped";
  lastSnapshotAt: string;
}

export interface RunnerHealth {
  id: string;
  name: string;
  status: RunnerStatus;
  region: string;
  queueDepth: number;
  usagePercent: number;
  quotaPercent: number;
  lastHeartbeatAt: string;
  capabilities: string[];
}

export interface OperationApproval {
  id: string;
  taskId: string;
  status: ApprovalStatus;
  requestedBy: string;
  requestedAt: string;
  permission: string;
  commit: string;
  expiresAt: string;
}

export interface OperationReview {
  id: string;
  taskId: string;
  status: "pending" | "in_review" | "approved" | "changes_requested";
  reviewer: string | null;
  summary: string;
  updatedAt: string;
}

export interface OperationNotification {
  id: string;
  kind: "completion" | "attention" | "approval" | "recovery";
  taskId: string | null;
  title: string;
  body: string;
  link: string;
  createdAt: string;
  read: boolean;
  channels: Array<"in_app" | "email" | "web_push">;
  containsSecrets: false;
}

export interface OperationAuditEvent {
  id: string;
  at: string;
  actor: string;
  action: string;
  resourceType: "task" | "workspace" | "runner" | "approval" | "notification" | "backup" | "session";
  resourceId: string;
  permission: string;
  commit: string;
  metadata: Record<string, string | number | boolean | null>;
}

export interface OperationEvent {
  sequence: number;
  at: string;
  type: string;
  taskId: string | null;
  payload: Record<string, string | number | boolean | null>;
}

export interface OperationNotificationPreferences {
  completion: boolean;
  attention: boolean;
  approval: boolean;
  recovery: boolean;
  channels: Array<"in_app" | "email" | "web_push">;
}

export interface OperationPolicies {
  retention: {
    taskLogsDays: number;
    auditDays: number;
    mutableWorkspaceDays: number;
    immutableArtifacts: true;
  };
  recovery: {
    rpoMinutes: number;
    rtoMinutes: number;
    backupFrequencyMinutes: number;
    restoreVerification: string;
  };
  synthetics: Array<{
    id: string;
    name: string;
    intervalMinutes: number;
    objective: string;
  }>;
}

export interface OperationsOverview {
  schemaVersion: typeof OPERATIONS_SCHEMA_VERSION;
  generatedAt: string;
  connectivity: {
    mode: "online" | "degraded";
    lastEventAt: string;
    serverContinuation: true;
    reconnectAfterSeconds: number;
  };
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
  metrics: {
    queued: number;
    running: number;
    failed: number;
    cancelled: number;
    orphaned: number;
    usagePercent: number;
    quotaPercent: number;
  };
}

export const DEFAULT_OPERATION_POLICIES: OperationPolicies = {
  retention: {
    taskLogsDays: 30,
    auditDays: 365,
    mutableWorkspaceDays: 14,
    immutableArtifacts: true,
  },
  recovery: {
    rpoMinutes: 15,
    rtoMinutes: 30,
    backupFrequencyMinutes: 15,
    restoreVerification: "Restore into a clean store and compare database and immutable artifact digests.",
  },
  synthetics: [
    { id: "ops-api", name: "Operations API health", intervalMinutes: 5, objective: "Health response stays available and reports runner freshness." },
    { id: "ops-task", name: "Task continuation", intervalMinutes: 10, objective: "A disconnected task continues and retains its log cursor." },
    { id: "ops-recovery", name: "Backup restore", intervalMinutes: 60, objective: "A clean restore reproduces task state and immutable artifact references." },
  ],
};

export function stableTaskHref(taskId: string): string {
  return `/operations?task=${encodeURIComponent(taskId)}`;
}

export function statusLabel(status: TaskStatus): string {
  return status.charAt(0).toUpperCase() + status.slice(1);
}

export function isTerminalTaskStatus(status: TaskStatus): boolean {
  return status === "succeeded" || status === "failed" || status === "cancelled";
}

export function containsSecretLikeKey(value: unknown): boolean {
  if (Array.isArray(value)) return value.some(containsSecretLikeKey);
  if (!value || typeof value !== "object") return false;
  return Object.entries(value).some(([key, nested]) => {
    if (/secret|token|password|credential|private[_-]?key|authorization/i.test(key)) return true;
    return containsSecretLikeKey(nested);
  });
}
