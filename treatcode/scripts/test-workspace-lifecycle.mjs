import assert from "node:assert/strict";
import { api, assertApiOk, currentCommit, login, repoRoot, startWorkspaceServer, writeEvidence } from "./workspace-test-lib.mjs";

const server = await startWorkspaceServer();
try {
  const token = await login(server);
  const commit = currentCommit();
  const workspace = assertApiOk(await api(server, "/api/workspaces/v1/workspaces", { method: "POST", token, body: { repository: repoRoot, commit, name: "P08 resume checksum" } }), "create lifecycle workspace");
  const base = `/api/workspaces/v1/workspaces/${encodeURIComponent(workspace.id)}`;
  const original = "snapshot A\n";
  const changed = "snapshot B\n";
  assertApiOk(await api(server, `${base}/files`, { method: "POST", token, body: { path: "p08-resume.txt", content: original, expected_version: 0 } }), "write original state");
  const snapshotA = assertApiOk(await api(server, `${base}/snapshots`, { method: "POST", token, body: { label: "state A" } }), "snapshot A");
  assertApiOk(await api(server, `${base}/files`, { method: "POST", token, body: { path: "p08-resume.txt", content: changed, expected_version: 1 } }), "write changed state");
  const snapshotB = assertApiOk(await api(server, `${base}/snapshots`, { method: "POST", token, body: { label: "state B" } }), "snapshot B");
  assert.notEqual(snapshotA.tree_hash, snapshotB.tree_hash);
  assertApiOk(await api(server, `${base}/snapshots/${encodeURIComponent(snapshotA.snapshot_id)}/restore`, { method: "POST", token }), "restore snapshot A");
  const afterRestore = assertApiOk(await api(server, `${base}/files?path=p08-resume.txt`, { token }), "read restored state");
  assert.equal(afterRestore.content, original);
  const restoredWorkspace = assertApiOk(await api(server, base, { token }), "inspect restored workspace");
  assert.equal(restoredWorkspace.last_tree_hash, snapshotA.tree_hash);
  assertApiOk(await api(server, `${base}/disconnect`, { method: "POST", token }), "disconnect for resume");
  assertApiOk(await api(server, `${base}/resume`, { method: "POST", token }), "resume checksum workspace");
  const afterResume = assertApiOk(await api(server, `${base}/files?path=p08-resume.txt`, { token }), "read resumed state");
  assert.equal(afterResume.content, original);
  const audit = assertApiOk(await api(server, `${base}/audit`, { token }), "read lifecycle audit");
  assert.ok(audit.some((event) => event.event_type === "workspace.snapshot_restored"));
  assert.ok(audit.some((event) => event.event_type === "workspace.resumed"));

  writeEvidence("snapshot-resume.json", {
    schema: "treatcode.workspace.snapshot-resume.v1",
    workspace_id: workspace.id,
    commit,
    snapshot_a: { id: snapshotA.snapshot_id, tree_hash: snapshotA.tree_hash },
    snapshot_b: { id: snapshotB.snapshot_id, tree_hash: snapshotB.tree_hash },
    restored_tree_hash: restoredWorkspace.last_tree_hash,
    content_recovered: afterResume.content === original,
    audit_events: audit.length,
  });
  console.log(JSON.stringify({ ok: true, workspace_id: workspace.id, snapshot_a: snapshotA.tree_hash, restored: restoredWorkspace.last_tree_hash }));
  assertApiOk(await api(server, `${base}/destroy`, { method: "POST", token }), "destroy lifecycle workspace");
} finally {
  await server.close();
}

