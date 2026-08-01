import { useEffect, useMemo, useState } from "react";
import type { WorkspaceRecord, WorkspaceSnapshot } from "./workspaceApi";

const API_ROOT = "/api/workspaces/v1";

interface ApiResult<T> {
  data?: T;
  error?: { code?: string; message?: string; details?: Record<string, unknown> };
}

async function callApi<T>(token: string, path: string, init: RequestInit = {}): Promise<ApiResult<T>> {
  const headers = new Headers(init.headers);
  headers.set("Accept", "application/json");
  headers.set("Content-Type", "application/json");
  if (token.trim()) headers.set("Authorization", `Bearer ${token.trim()}`);
  const response = await fetch(`${API_ROOT}${path}`, { ...init, headers });
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) return { error: payload.error || { code: `http_${response.status}`, message: "The workspace request failed." } };
  return payload as ApiResult<T>;
}

function shortCommit(value: string): string {
  return value && value !== "unknown" ? value.slice(0, 12) : "unresolved";
}

function prettyStatus(status: string): string {
  return status.replace(/_/g, " ");
}

export default function WorkspaceApp() {
  const [token, setToken] = useState("");
  const [identityId, setIdentityId] = useState("tc:identity:demo-human");
  const [accessKey, setAccessKey] = useState("local-human-key");
  const [repository, setRepository] = useState("");
  const [commit, setCommit] = useState("HEAD");
  const [name, setName] = useState("Trit workspace");
  const [workspaces, setWorkspaces] = useState<WorkspaceRecord[]>([]);
  const [selectedId, setSelectedId] = useState("");
  const [selected, setSelected] = useState<WorkspaceRecord | null>(null);
  const [filePath, setFilePath] = useState("README.md");
  const [fileContent, setFileContent] = useState("");
  const [snapshots, setSnapshots] = useState<WorkspaceSnapshot[]>([]);
  const [handoffTarget, setHandoffTarget] = useState("agent.demo");
  const [contextPath, setContextPath] = useState("docs/README.md");
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState("Ready for an authorized workspace action.");

  const selectedSnapshot = useMemo(() => snapshots[snapshots.length - 1], [snapshots]);

  const signIn = async () => {
    setBusy(true);
    const response = await fetch("/api/auth/v1/login", { method: "POST", headers: { Accept: "application/json", "Content-Type": "application/json" }, body: JSON.stringify({ identity_id: identityId, access_key: accessKey }) });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || !payload.data?.credential?.token) setMessage(`${payload.error?.code || "login_failed"}: ${payload.error?.reason || "The identity could not sign in."}`);
    else {
      setToken(payload.data.credential.token);
      setMessage(`Signed in as ${payload.data.identity?.display_name || identityId}.`);
    }
    setBusy(false);
  };

  const refresh = async (id = selectedId) => {
    setBusy(true);
    const list = await callApi<WorkspaceRecord[]>(token, "/workspaces");
    if (list.error) {
      setMessage(`${list.error.code || "error"}: ${list.error.message || "Unable to list workspaces."}`);
      setBusy(false);
      return;
    }
    const records = list.data || [];
    setWorkspaces(records);
    const nextId = id && records.some((record) => record.id === id) ? id : records[0]?.id || "";
    setSelectedId(nextId);
    if (nextId) {
      const detail = await callApi<WorkspaceRecord>(token, `/workspaces/${encodeURIComponent(nextId)}`);
      if (!detail.error && detail.data) setSelected(detail.data);
      const history = await callApi<WorkspaceSnapshot[]>(token, `/workspaces/${encodeURIComponent(nextId)}/snapshots`);
      if (!history.error) setSnapshots(history.data || []);
    } else {
      setSelected(null);
      setSnapshots([]);
    }
    setBusy(false);
  };

  useEffect(() => {
    void refresh("");
    // Token changes intentionally refresh the visible scope.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [token]);

  const createWorkspace = async (event: React.FormEvent) => {
    event.preventDefault();
    setBusy(true);
    const result = await callApi<WorkspaceRecord>(token, "/workspaces", {
      method: "POST",
      body: JSON.stringify({ repository: repository.trim() || undefined, commit: commit.trim() || "HEAD", name: name.trim() || "Trit workspace", mode: "isolated" }),
    });
    if (result.error || !result.data) setMessage(`${result.error?.code || "create_failed"}: ${result.error?.message || "The workspace could not be created."}`);
    else {
      setMessage(`Created ${result.data.id} at ${shortCommit(result.data.commit)}.`);
      await refresh(result.data.id);
    }
    setBusy(false);
  };

  const action = async (operation: string, path: string, body?: unknown) => {
    if (!selected) return;
    setBusy(true);
    const result = await callApi<WorkspaceRecord | { workspace: WorkspaceRecord } | WorkspaceSnapshot>(token, `/workspaces/${encodeURIComponent(selected.id)}${path}`, {
      method: "POST",
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    if (result.error) setMessage(`${result.error.code || "action_failed"}: ${result.error.message || "The action failed."}`);
    else {
      setMessage(`${operation} completed.`);
      await refresh(selected.id);
    }
    setBusy(false);
  };

  const openFile = async () => {
    if (!selected || !filePath.trim()) return;
    const result = await callApi<{ content: string }>(token, `/workspaces/${encodeURIComponent(selected.id)}/files?path=${encodeURIComponent(filePath.trim())}`);
    if (result.error) setMessage(`${result.error.code || "file_failed"}: ${result.error.message || "The file could not be loaded."}`);
    else setFileContent(result.data?.content || "");
  };

  const saveFile = async () => {
    if (!selected) return;
    setBusy(true);
    const result = await callApi(token, `/workspaces/${encodeURIComponent(selected.id)}/files`, { method: "POST", body: JSON.stringify({ path: filePath.trim(), content: fileContent, expected_version: selected.file_version }) });
    if (result.error) setMessage(`${result.error.code || "save_failed"}: ${result.error.message || "The file could not be saved."}`);
    else {
      setMessage(`Saved ${filePath.trim()} in the isolated workspace.`);
      await refresh(selected.id);
    }
    setBusy(false);
  };

  const createHandoff = async () => {
    await action("Handoff", "/handoff", { to_actor_id: handoffTarget, scopes: ["workspace:read", "workspace:edit", "workspace:context"] });
  };

  const createContext = async () => {
    if (!selected) return;
    setBusy(true);
    const result = await callApi<{ content_hash: string; files: Array<{ path: string }> }>(token, `/workspaces/${encodeURIComponent(selected.id)}/context-package`, { method: "POST", body: JSON.stringify({ scopes: [contextPath.trim()], limits: { max_files: 20, max_bytes: 32768 } }) });
    setMessage(result.error ? `${result.error.code || "context_failed"}: ${result.error.message || "The context package failed."}` : `Context package ready: ${result.data?.content_hash?.slice(0, 16)} (${result.data?.files?.length || 0} files).`);
    setBusy(false);
  };

  return (
    <div className="workspace-app">
      <header className="workspace-header">
        <a className="workspace-wordmark" href="/">Treat<span>Code</span></a>
        <nav aria-label="Workspace navigation">
          <a href="/stack">Stack Explorer</a>
          <a href="/learn">Learn</a>
          <a className="active" href="/workspaces">Workspaces</a>
        </nav>
      </header>
      <main className="workspace-main">
        <section className="workspace-hero">
          <div>
            <span className="workspace-eyebrow">P08 · remote development</span>
            <h1>Work on an exact commit, with the state still yours.</h1>
            <p>Provision an isolated checkout, keep its snapshots across disconnects, and hand off only the scopes a collaborator needs.</p>
          </div>
          <div className="workspace-auth-card">
            <label htmlFor="workspace-identity">Identity</label>
            <select id="workspace-identity" value={identityId} onChange={(event) => setIdentityId(event.target.value)}><option value="tc:identity:demo-human">Demo human</option><option value="tc:identity:demo-collaborator">Demo collaborator</option></select>
            <label htmlFor="workspace-access-key">Access key</label>
            <input id="workspace-access-key" value={accessKey} onChange={(event) => setAccessKey(event.target.value)} autoComplete="off" />
            <button className="workspace-primary" onClick={() => void signIn()} disabled={busy}>Sign in and load scope</button>
            <small>Issued credentials are short-lived; they are sent only as a bearer token to the versioned API.</small>
          </div>
        </section>

        <div className="workspace-grid">
          <section className="workspace-panel">
            <div className="workspace-panel-heading"><div><span className="workspace-eyebrow">Create</span><h2>New isolated workspace</h2></div><span className="workspace-badge">commit-pinned</span></div>
            <form onSubmit={createWorkspace} className="workspace-form">
              <label>Name<input value={name} onChange={(event) => setName(event.target.value)} /></label>
              <label>Repository mirror or path<input value={repository} onChange={(event) => setRepository(event.target.value)} placeholder="Configured repository mirror" /></label>
              <label>Commit<input value={commit} onChange={(event) => setCommit(event.target.value)} placeholder="HEAD or a full commit" /></label>
              <button className="workspace-primary" disabled={busy} type="submit">{busy ? "Working…" : "Create workspace"}</button>
            </form>
            <p className="workspace-note">Provisioning uses a Git archive into mutable storage outside the authoritative checkout.</p>
          </section>

          <section className="workspace-panel">
            <div className="workspace-panel-heading"><div><span className="workspace-eyebrow">Your scope</span><h2>Workspaces</h2></div><button className="workspace-quiet" onClick={() => void refresh()} disabled={busy}>Refresh</button></div>
            {workspaces.length ? <div className="workspace-list">{workspaces.map((workspace) => <button key={workspace.id} className={`workspace-list-item ${workspace.id === selectedId ? "selected" : ""}`} onClick={() => void refresh(workspace.id)}><span><strong>{workspace.name}</strong><small>{shortCommit(workspace.commit)} · {workspace.image}</small></span><em>{prettyStatus(workspace.status)}</em></button>)}</div> : <div className="workspace-empty">No accessible workspaces yet.</div>}
          </section>
        </div>

        {selected ? <section className="workspace-panel workspace-detail">
          <div className="workspace-detail-heading"><div><span className="workspace-eyebrow">Workspace detail</span><h2>{selected.name}</h2><p>{selected.repository} · <code>{selected.commit}</code></p></div><span className={`workspace-status status-${selected.status}`}>{prettyStatus(selected.status)}</span></div>
          <div className="workspace-facts"><span><b>Image</b>{selected.image}</span><span><b>Toolchain</b>{selected.toolchain.name} {selected.toolchain.version}</span><span><b>File version</b>{selected.file_version}</span><span><b>Tree checksum</b>{selected.last_tree_hash?.slice(0, 16) || "—"}</span></div>
          <div className="workspace-actions"><button onClick={() => void action("Resume", "/resume")} disabled={busy}>Resume</button><button onClick={() => void action("Disconnect", "/disconnect", { checkpoint: true })} disabled={busy}>Disconnect + checkpoint</button><button onClick={() => void action("Snapshot", "/snapshots", { label: "Manual checkpoint" })} disabled={busy}>Snapshot</button><button onClick={() => void action("Export", "/export")} disabled={busy}>Export manifest</button><button className="workspace-danger" onClick={() => void action("Destroy", "/destroy")} disabled={busy}>Destroy</button></div>

          <div className="workspace-editor-grid">
            <div className="workspace-editor">
              <div className="workspace-panel-heading"><div><span className="workspace-eyebrow">Browser editor</span><h3>Mutable file</h3></div><button className="workspace-quiet" onClick={() => void openFile()}>Open</button></div>
              <div className="workspace-file-row"><input value={filePath} onChange={(event) => setFilePath(event.target.value)} aria-label="Workspace file path" /><button onClick={() => void saveFile()} disabled={busy}>Save</button></div>
              <textarea value={fileContent} onChange={(event) => setFileContent(event.target.value)} aria-label="Workspace file editor" spellCheck={false} />
            </div>
            <div className="workspace-side-tools">
              <div><span className="workspace-eyebrow">Terminal gate</span><h3>Safe checks</h3><p>Doctor and smoke preflight run inside this workspace and report whether the authoritative checkout stayed unchanged.</p><button onClick={() => void action("Run doctor", "/checks", { kind: "doctor" })} disabled={busy}>Run doctor</button><button onClick={() => void action("Run smoke", "/checks", { kind: "smoke" })} disabled={busy}>Run smoke preflight</button></div>
              <div><span className="workspace-eyebrow">Task approval</span><h3>Mobile-safe control</h3><p>Approve the scoped task without opening the editor. The API records the human action.</p>{selected.tasks[0] ? <button onClick={() => void action("Approve task", `/tasks/${encodeURIComponent(selected.tasks[0].id)}/approve`)} disabled={busy}>Approve task</button> : <span className="workspace-note">Create a task to enable approval.</span>}</div>
              <div><span className="workspace-eyebrow">Bounded context</span><h3>Inject a scope</h3><div className="workspace-file-row"><input value={contextPath} onChange={(event) => setContextPath(event.target.value)} aria-label="Context path" /><button onClick={() => void createContext()} disabled={busy}>Package</button></div></div>
              <div><span className="workspace-eyebrow">Handoff</span><h3>Collaborate safely</h3><div className="workspace-file-row"><select value={handoffTarget} onChange={(event) => setHandoffTarget(event.target.value)} aria-label="Handoff recipient"><option value="tc:identity:demo-collaborator">Demo collaborator</option><option value="tc:identity:demo-service">Demo service</option></select><button onClick={() => void createHandoff()} disabled={busy}>Handoff</button></div></div>
            </div>
          </div>
          <div className="workspace-snapshot-strip"><strong>{snapshots.length} snapshots</strong>{selectedSnapshot ? <span>Latest {selectedSnapshot.label} · {selectedSnapshot.tree_hash.slice(0, 16)}</span> : <span>Disconnect or checkpoint to make recovery evidence.</span>}</div>
        </section> : null}

        <p className="workspace-live-message" role="status" aria-live="polite">{message}</p>
      </main>
    </div>
  );
}
