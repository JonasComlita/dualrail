export const WORKSPACE_API_SCHEMA_VERSION = "treatcode.workspace.api.v1" as const;

export type WorkspaceStatus = "active" | "suspended" | "destroyed" | "lost";
export type WorkspaceMode = "isolated";
export type WorkspaceActorKind = "human" | "collaborator" | "agent" | "service";

export type WorkspaceScope =
  | "workspace:create"
  | "workspace:read"
  | "workspace:edit"
  | "workspace:test"
  | "workspace:context"
  | "workspace:handoff"
  | "workspace:destroy"
  | "task:create"
  | "task:read";

export interface WorkspaceToolchain {
  name: string;
  version: string;
  image?: string;
}

export interface WorkspaceCollaborator {
  actor_id: string;
  kind: WorkspaceActorKind;
  scopes: WorkspaceScope[];
  granted_at: string;
  granted_by: string;
}

export interface WorkspaceSnapshot {
  snapshot_id: string;
  workspace_id: string;
  sequence: number;
  label: string;
  tree_hash: string;
  file_count: number;
  byte_count: number;
  created_at: string;
  created_by: string;
}

export interface WorkspaceTask {
  id: string;
  entity_type: "task";
  title: string;
  scope: string;
  acceptance: string[];
  priority: "low" | "normal" | "high" | "critical";
  assigned_to?: string;
  workspace_id: string;
  context_scopes: string[];
  status: "queued" | "active" | "complete" | "cancelled";
  created_at: string;
  updated_at: string;
  created_by: string;
}

export interface WorkspaceRecord {
  id: string;
  entity_type: "workspace";
  name: string;
  repository: string;
  requested_commit: string;
  base_commit: string;
  commit: string;
  mode: WorkspaceMode;
  image: string;
  toolchain: WorkspaceToolchain;
  status: WorkspaceStatus;
  owner_actor_id: string;
  owner_kind: WorkspaceActorKind;
  collaborators: WorkspaceCollaborator[];
  snapshots: WorkspaceSnapshot[];
  tasks: WorkspaceTask[];
  file_version: number;
  last_tree_hash: string | null;
  created_at: string;
  updated_at: string;
  last_connected_at: string;
  last_disconnected_at?: string;
  destroyed_at?: string;
  retention_policy: string;
}

export interface WorkspaceApiEnvelope<T> {
  schema_version: typeof WORKSPACE_API_SCHEMA_VERSION;
  request_id: string;
  data: T;
  meta?: Record<string, unknown>;
  links?: Record<string, string>;
}

export interface WorkspaceContextFile {
  path: string;
  sha256: string;
  bytes: number;
  content: string;
}

export interface WorkspaceContextPackage {
  package_id: string;
  workspace_id: string;
  task_id?: string;
  source_commit: string;
  requested_scopes: string[];
  files: WorkspaceContextFile[];
  total_bytes: number;
  max_files: number;
  max_bytes: number;
  content_hash: string;
  created_at: string;
  created_by: string;
}
