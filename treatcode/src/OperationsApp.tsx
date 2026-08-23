import { FormEvent, useEffect, useMemo, useState } from "react";
import {
  OperationApproval,
  OperationAuditEvent,
  OperationNotification,
  OperationReview,
  OperationTask,
  OperationsOverview,
  RunnerHealth,
  TaskStatus,
  statusLabel,
} from "./operationsModel";
import "./operations.css";
import "./treatcode-theme.css";

type Panel = "overview" | "tasks" | "runners" | "approvals" | "audit" | "recovery";

const PANELS: Array<{ id: Panel; label: string }> = [
  { id: "overview", label: "Overview" },
  { id: "tasks", label: "Tasks" },
  { id: "runners", label: "Runners" },
  { id: "approvals", label: "Approvals" },
  { id: "audit", label: "Audit" },
  { id: "recovery", label: "Recovery" },
];

function readRoute(): { panel: Panel; taskId: string | null } {
  const params = new URLSearchParams(window.location.search);
  const rawPanel = params.get("panel");
  const panel = PANELS.some((candidate) => candidate.id === rawPanel) ? rawPanel as Panel : "overview";
  return { panel, taskId: params.get("task") };
}

async function requestJson<T>(url: string, init?: RequestInit): Promise<T> {
  const response = await fetch(url, { headers: { Accept: "application/json", "Content-Type": "application/json", ...(init?.headers || {}) }, ...init });
  const body = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(body?.error?.message || body?.error?.code || `Request failed (${response.status})`);
  return body as T;
}

function StatusBadge({ status }: { status: TaskStatus | RunnerHealth["status"] | OperationApproval["status"] }): JSX.Element {
  return <span className={`ops-status status-${status}`}>{statusLabel(status as TaskStatus)}</span>;
}

function Metric({ label, value, tone = "neutral" }: { label: string; value: string | number; tone?: string }): JSX.Element {
  return <div className={`ops-metric metric-${tone}`}><span>{label}</span><strong>{value}</strong></div>;
}

function TaskCard({ task, approval, selected, onSelect, onAction }: { task: OperationTask; approval?: OperationApproval; selected: boolean; onSelect: () => void; onAction: (action: string) => void }): JSX.Element {
  const canStart = task.status === "queued" && (!task.approvalId || approval?.status === "approved");
  const canApprove = task.approvalId && approval?.status === "pending" && task.status === "queued";
  return (
    <article className={`ops-card task-card ${selected ? "is-selected" : ""}`} data-task-id={task.id}>
      <div className="ops-card-header">
        <div>
          <button className="ops-link-button" onClick={onSelect} aria-label={`Open ${task.title}`}>{task.title}</button>
          <p className="ops-meta"><span>{task.kind}</span><span>{task.id}</span><span>{task.commit}</span></p>
        </div>
        <StatusBadge status={task.status} />
      </div>
      <div className="task-progress" aria-label={`${task.progress}% complete`}><span style={{ width: `${task.progress}%` }} /></div>
      <div className="ops-card-footer"><span className="ops-muted">{task.progress}% · {task.logs.length} retained log entries</span><div className="ops-actions">
        {canStart && <button className="ops-button small" onClick={() => onAction("start")}>Start</button>}
        {canApprove && <button className="ops-button small primary" onClick={() => onAction("approve")}>Approve</button>}
        {(task.status === "running" || task.status === "orphaned") && <button className="ops-button small danger" onClick={() => onAction("cancel")}>Stop</button>}
        {(task.status === "failed" || task.status === "cancelled" || task.status === "orphaned") && <button className="ops-button small" onClick={() => onAction(task.status === "orphaned" ? "resume" : "retry")}>{task.status === "orphaned" ? "Recover" : "Retry"}</button>}
      </div></div>
    </article>
  );
}

function NotificationList({ notifications, onRead }: { notifications: OperationNotification[]; onRead: (id: string) => void }): JSX.Element {
  return <div className="ops-list notification-list">{notifications.length === 0 ? <p className="ops-empty">No notifications need attention.</p> : notifications.slice(0, 5).map((notification) => (
    <div className={`notification-row ${notification.read ? "is-read" : ""}`} key={notification.id}>
      <div><span className={`notification-dot dot-${notification.kind}`} /><div><a href={notification.link}>{notification.title}</a><p>{notification.body}</p><span className="ops-muted">{new Date(notification.createdAt).toLocaleString()}</span></div></div>
      {!notification.read && <button className="ops-text-button" onClick={() => onRead(notification.id)}>Mark read</button>}
    </div>
  ))}</div>;
}

function ReviewList({ reviews, approvals }: { reviews: OperationReview[]; approvals: OperationApproval[] }): JSX.Element {
  return <div className="ops-list">{reviews.map((review) => {
    const approval = approvals.find((candidate) => candidate.taskId === review.taskId);
    return <div className="review-row" key={review.id}><div><strong>{review.taskId}</strong><p>{review.summary}</p></div><div className="review-state"><span>{review.status.replace(/_/g, " ")}</span>{approval && <StatusBadge status={approval.status} />}</div></div>;
  })}</div>;
}

function RunnerList({ runners }: { runners: RunnerHealth[] }): JSX.Element {
  return <div className="runner-grid">{runners.map((runner) => <article className="runner-card" key={runner.id}>
    <div className="ops-card-header"><div><strong>{runner.name}</strong><p className="ops-meta">{runner.id} · {runner.region}</p></div><StatusBadge status={runner.status} /></div>
    <div className="runner-stats"><span><b>{runner.queueDepth}</b> queued</span><span><b>{runner.usagePercent}%</b> usage</span><span><b>{runner.quotaPercent}%</b> quota</span></div>
    <p className="ops-muted">Heartbeat {new Date(runner.lastHeartbeatAt).toLocaleTimeString()}</p>
    <div className="runner-capabilities">{runner.capabilities.map((capability) => <span key={capability}>{capability}</span>)}</div>
  </article>)}</div>;
}

function AuditTable({ events }: { events: OperationAuditEvent[] }): JSX.Element {
  return <div className="ops-table-wrap"><table className="ops-table"><caption className="sr-only">Recent consequential operations</caption><thead><tr><th>When</th><th>Action</th><th>Actor</th><th>Permission</th><th>Commit</th></tr></thead><tbody>{events.slice(0, 12).map((event) => <tr key={event.id}><td>{new Date(event.at).toLocaleString()}</td><td><strong>{event.action.replace(/_/g, " ")}</strong><small>{event.resourceType} · {event.resourceId}</small></td><td>{event.actor}</td><td>{event.permission}</td><td className="mono">{event.commit}</td></tr>)}</tbody></table></div>;
}

export default function OperationsApp(): JSX.Element {
  const route = readRoute();
  const [panel, setPanel] = useState<Panel>(route.panel);
  const [selectedTaskId, setSelectedTaskId] = useState<string | null>(route.taskId);
  const [overview, setOverview] = useState<OperationsOverview | null>(null);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const [loading, setLoading] = useState(true);
  const [taskTitle, setTaskTitle] = useState("");
  const [taskKind, setTaskKind] = useState("test");
  const [requiresApproval, setRequiresApproval] = useState(false);

  const load = async (): Promise<void> => {
    try {
      const next = await requestJson<OperationsOverview>("/api/operations/overview");
      setOverview(next);
      setError("");
      if (!selectedTaskId && next.tasks[0]) setSelectedTaskId(next.tasks[0].id);
    } catch (loadError) {
      setError(loadError instanceof Error ? loadError.message : "Operations service is unavailable.");
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    void load();
    const poller = window.setInterval(() => void load(), 5000);
    let stream: EventSource | null = null;
    if (typeof EventSource !== "undefined") {
      stream = new EventSource("/api/operations/stream");
      stream.addEventListener("operations", () => void load());
      stream.onerror = () => stream?.close();
    }
    return () => { window.clearInterval(poller); stream?.close(); };
  }, []);

  const selectedTask = useMemo(() => overview?.tasks.find((task) => task.id === selectedTaskId) || null, [overview, selectedTaskId]);

  const navigate = (nextPanel: Panel, taskId = selectedTaskId): void => {
    setPanel(nextPanel);
    if (taskId) setSelectedTaskId(taskId);
    const params = new URLSearchParams();
    if (nextPanel !== "overview") params.set("panel", nextPanel);
    if (taskId) params.set("task", taskId);
    window.history.replaceState({}, "", `${window.location.pathname}${params.toString() ? `?${params}` : ""}`);
  };

  const runAction = async (taskId: string, action: string): Promise<void> => {
    try {
      await requestJson(`/api/operations/tasks/${encodeURIComponent(taskId)}/actions`, { method: "POST", body: JSON.stringify({ action, actor: { id: "operations-console", permission: "operations:write" } }) });
      setNotice(`${action.replace(/_/g, " ")} recorded for ${taskId}.`);
      await load();
    } catch (actionError) {
      setError(actionError instanceof Error ? actionError.message : "The task action failed.");
    }
  };

  const createTask = async (event: FormEvent): Promise<void> => {
    event.preventDefault();
    if (!taskTitle.trim()) return;
    try {
      const task = await requestJson<OperationTask>("/api/operations/tasks", { method: "POST", body: JSON.stringify({ title: taskTitle, kind: taskKind, requiresApproval, actor: { id: "operations-console", permission: "operations:write" } }) });
      setTaskTitle("");
      setRequiresApproval(false);
      setSelectedTaskId(task.id);
      setNotice("Task created and queued.");
      await load();
    } catch (createError) {
      setError(createError instanceof Error ? createError.message : "The task could not be created.");
    }
  };

  const markRead = async (notificationId: string): Promise<void> => {
    try { await requestJson(`/api/operations/notifications/${encodeURIComponent(notificationId)}/read`, { method: "POST", body: JSON.stringify({}) }); await load(); } catch (readError) { setError(readError instanceof Error ? readError.message : "The notification could not be updated."); }
  };

  const runRecovery = async (): Promise<void> => {
    try { const report = await requestJson<{ ok: boolean; immutableArtifactCount: number }>("/api/operations/recovery-exercise", { method: "POST", body: "{}" }); setNotice(`Recovery exercise ${report.ok ? "passed" : "needs attention"}; ${report.immutableArtifactCount} immutable artifact references checked.`); await load(); } catch (recoveryError) { setError(recoveryError instanceof Error ? recoveryError.message : "Recovery exercise failed."); }
  };

  if (loading && !overview) return <div className="operations-shell"><div className="ops-loading" aria-live="polite">Loading operations console…</div></div>;
  if (!overview) return <div className="operations-shell"><div className="ops-error" role="alert">{error || "Operations data is unavailable."}<button className="ops-button" onClick={() => void load()}>Retry</button></div></div>;

  const metrics = overview.metrics;
  return <div className="operations-shell">
    <header className="operations-header"><div className="operations-brand"><a href="/" className="ops-wordmark">TREATCODE</a><span className="ops-divider" /><div><p className="ops-kicker">Operations control plane</p><h1>Operations console</h1></div></div><div className="operations-header-actions"><span className={`connectivity-pill connectivity-${overview.connectivity.mode}`}><i />{overview.connectivity.mode === "online" ? "Live" : "Degraded"}</span><a className="ops-button ghost" href="/practice">Practice</a><a className="ops-button ghost" href="/intelligence">Intelligence</a></div></header>
    <div className="operations-layout">
      <nav className="operations-tabs" aria-label="Operations sections">{PANELS.map((candidate) => <button key={candidate.id} className={panel === candidate.id ? "active" : ""} onClick={() => navigate(candidate.id)}>{candidate.label}{candidate.id === "approvals" && overview.approvals.filter((approval) => approval.status === "pending").length > 0 && <span className="tab-count">{overview.approvals.filter((approval) => approval.status === "pending").length}</span>}</button>)}</nav>
      <main className="operations-main">
        <div className="ops-alerts" aria-live="polite">{error && <div className="ops-banner error" role="alert">{error}<button onClick={() => setError("")} aria-label="Dismiss error">×</button></div>}{notice && <div className="ops-banner success">{notice}<button onClick={() => setNotice("")} aria-label="Dismiss notice">×</button></div>}</div>
        <section className="continuation-banner"><div className="continuation-icon">↻</div><div><strong>Server continuation is active</strong><p>Jobs keep running and retain their logs when every client disconnects. Reconnect within {overview.connectivity.reconnectAfterSeconds} seconds to catch up.</p></div><span className="continuation-status">Event stream connected</span></section>

        {panel === "overview" && <>
          <section className="ops-section-heading"><div><p className="ops-kicker">At a glance</p><h2>What needs attention?</h2></div><button className="ops-button primary" onClick={() => navigate("tasks")}>Open task board</button></section>
          <section className="metric-grid" aria-label="Task metrics"><Metric label="Queued" value={metrics.queued} tone="queued" /><Metric label="Running" value={metrics.running} tone="running" /><Metric label="Failed" value={metrics.failed} tone="failed" /><Metric label="Orphaned" value={metrics.orphaned} tone="orphaned" /><Metric label="Runner usage" value={`${metrics.usagePercent}%`} /><Metric label="Quota used" value={`${metrics.quotaPercent}%`} /></section>
          <div className="ops-dashboard-grid"><section className="ops-panel"><div className="ops-panel-heading"><div><p className="ops-kicker">Work queue</p><h2>Active tasks</h2></div><button className="ops-text-button" onClick={() => navigate("tasks")}>View all →</button></div><div className="task-list">{overview.tasks.slice(0, 4).map((task) => <TaskCard key={task.id} task={task} approval={overview.approvals.find((approval) => approval.id === task.approvalId)} selected={task.id === selectedTaskId} onSelect={() => navigate("tasks", task.id)} onAction={(action) => void runAction(task.id, action)} />)}</div></section><section className="ops-panel"><div className="ops-panel-heading"><div><p className="ops-kicker">Attention stream</p><h2>Notifications</h2></div><span className="ops-muted">{overview.notifications.filter((notification) => !notification.read).length} unread</span></div><NotificationList notifications={overview.notifications} onRead={(id) => void markRead(id)} /></section></div>
          <div className="ops-dashboard-grid"><section className="ops-panel"><div className="ops-panel-heading"><div><p className="ops-kicker">Execution</p><h2>Runner health</h2></div><button className="ops-text-button" onClick={() => navigate("runners")}>Details →</button></div><RunnerList runners={overview.runners} /></section><section className="ops-panel"><div className="ops-panel-heading"><div><p className="ops-kicker">Review lane</p><h2>Approvals & reviews</h2></div><button className="ops-text-button" onClick={() => navigate("approvals")}>Review →</button></div><ReviewList reviews={overview.reviews} approvals={overview.approvals} /></section></div>
        </>}

        {panel === "tasks" && <section className="ops-panel full-panel"><div className="ops-section-heading"><div><p className="ops-kicker">Task and workspace control</p><h2>Task board</h2><p className="ops-muted">Start, stop, recover, or approve work without opening a desktop IDE.</p></div></div><form className="create-task-form" onSubmit={(event) => void createTask(event)}><label>Task title<input value={taskTitle} onChange={(event) => setTaskTitle(event.target.value)} placeholder="e.g. Verify release image" /></label><label>Kind<select value={taskKind} onChange={(event) => setTaskKind(event.target.value)}><option value="test">Test</option><option value="build">Build</option><option value="review">Review</option><option value="workspace">Workspace</option></select></label><label className="checkbox-label"><input type="checkbox" checked={requiresApproval} onChange={(event) => setRequiresApproval(event.target.checked)} /> Requires approval</label><button className="ops-button primary" type="submit">Create task</button></form><div className="task-board">{overview.tasks.map((task) => <TaskCard key={task.id} task={task} approval={overview.approvals.find((approval) => approval.id === task.approvalId)} selected={task.id === selectedTaskId} onSelect={() => setSelectedTaskId(task.id)} onAction={(action) => void runAction(task.id, action)} />)}</div>{selectedTask && <section className="task-detail"><div className="ops-panel-heading"><div><p className="ops-kicker">Selected task</p><h3>{selectedTask.title}</h3></div><StatusBadge status={selectedTask.status} /></div><dl className="detail-grid"><div><dt>Task link</dt><dd><a href={selectedTask.links.self}>{selectedTask.links.self}</a></dd></div><div><dt>Workspace</dt><dd>{selectedTask.workspaceId}</dd></div><div><dt>Commit</dt><dd className="mono">{selectedTask.commit}</dd></div><div><dt>Continuation</dt><dd>cursor {selectedTask.continuation.cursor} · logs retained</dd></div></dl><div className="log-view" aria-label="Retained task log">{selectedTask.logs.map((log) => <div key={log.sequence}><span>{log.sequence}</span><b className={`log-${log.level}`}>{log.level}</b><p>{log.message}</p></div>)}</div></section>}</section>}

        {panel === "runners" && <section className="ops-panel full-panel"><div className="ops-section-heading"><div><p className="ops-kicker">Capacity and health</p><h2>Runner fleet</h2><p className="ops-muted">Queue depth, usage, quota, heartbeat, and capabilities for isolated workers.</p></div></div><RunnerList runners={overview.runners} /></section>}
        {panel === "approvals" && <section className="ops-panel full-panel"><div className="ops-section-heading"><div><p className="ops-kicker">Permission boundary</p><h2>Approvals and reviews</h2><p className="ops-muted">Every approval is tied to a task, permission, commit, expiry, and audit event.</p></div></div><div className="approval-list">{overview.approvals.map((approval) => <article className="approval-card" key={approval.id}><div><StatusBadge status={approval.status} /><h3>{approval.taskId}</h3><p>Requested by {approval.requestedBy} · {approval.permission}</p><p className="ops-muted">Commit {approval.commit} · expires {new Date(approval.expiresAt).toLocaleString()}</p></div>{approval.status === "pending" && <button className="ops-button primary" onClick={() => void runAction(approval.taskId, "approve")}>Approve</button>}</article>)}</div><ReviewList reviews={overview.reviews} approvals={overview.approvals} /></section>}
        {panel === "audit" && <section className="ops-panel full-panel"><div className="ops-section-heading"><div><p className="ops-kicker">Accountability</p><h2>Audit history</h2><p className="ops-muted">Who performed each consequential operation, when, under which permission, and against which commit.</p></div></div><AuditTable events={overview.audit} /></section>}
        {panel === "recovery" && <section className="ops-panel full-panel"><div className="ops-section-heading"><div><p className="ops-kicker">Backup and restore</p><h2>Recovery readiness</h2><p className="ops-muted">Restore into a clean environment and compare database state with immutable artifact references.</p></div><button className="ops-button primary" onClick={() => void runRecovery()}>Run recovery exercise</button></div><div className="recovery-grid"><Metric label="RPO" value={`${overview.policies.recovery.rpoMinutes} min`} /><Metric label="RTO" value={`${overview.policies.recovery.rtoMinutes} min`} /><Metric label="Backup cadence" value={`${overview.policies.recovery.backupFrequencyMinutes} min`} /><Metric label="Log retention" value={`${overview.policies.retention.taskLogsDays} days`} /></div><div className="synthetic-list">{overview.policies.synthetics.map((synthetic) => <div key={synthetic.id}><span className="synthetic-dot" /><div><strong>{synthetic.name}</strong><p>{synthetic.objective}</p></div><span className="ops-muted">every {synthetic.intervalMinutes}m</span></div>)}</div></section>}
      </main>
    </div>
    <footer className="operations-footer"><span>Operations API v1</span><span>Last event {new Date(overview.connectivity.lastEventAt).toLocaleTimeString()}</span><a href="/api/operations/health">Health</a><a href="/api/operations/audit">Audit API</a></footer>
  </div>;
}
