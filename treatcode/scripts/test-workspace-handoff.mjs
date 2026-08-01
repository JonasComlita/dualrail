import assert from "node:assert/strict";
import { api, assertApiOk, currentCommit, login, repoRoot, startWorkspaceServer, writeEvidence } from "./workspace-test-lib.mjs";

const server = await startWorkspaceServer();
try {
  const ownerToken = await login(server, "tc:identity:demo-human", "local-human-key");
  const collaboratorToken = await login(server, "tc:identity:demo-collaborator", "local-collaborator-key");
  const commit = currentCommit();
  const workspace = assertApiOk(await api(server, "/api/workspaces/v1/workspaces", { method: "POST", token: ownerToken, body: { repository: repoRoot, commit, name: "P08 handoff fixture" } }), "create handoff workspace");
  const base = `/api/workspaces/v1/workspaces/${encodeURIComponent(workspace.id)}`;
  const handoff = assertApiOk(await api(server, `${base}/handoff`, { method: "POST", token: ownerToken, body: { to_actor_id: "tc:identity:demo-collaborator", scopes: ["workspace:read", "workspace:edit", "workspace:context"] } }), "handoff workspace");
  assert.equal(handoff.handoff.actor_id, "tc:identity:demo-collaborator");
  assert.deepEqual(handoff.handoff.scopes, ["workspace:read", "workspace:edit", "workspace:context"]);

  const inspected = assertApiOk(await api(server, base, { token: collaboratorToken }), "collaborator inspect");
  assert.ok(inspected.collaborators.some((item) => item.actor_id === "tc:identity:demo-collaborator"));
  const edited = assertApiOk(await api(server, `${base}/files`, { method: "POST", token: collaboratorToken, body: { path: "p08-handoff.txt", content: "collaborator state\n", expected_version: 0 } }), "collaborator edit");
  assert.equal(edited.path, "p08-handoff.txt");

  const forbiddenDestroy = await api(server, `${base}/destroy`, { method: "POST", token: collaboratorToken });
  assert.equal(forbiddenDestroy.response.status, 403);
  assert.equal(forbiddenDestroy.payload.error.code, "workspace_access_denied");
  const stillActive = assertApiOk(await api(server, base, { token: ownerToken }), "owner sees active workspace");
  assert.equal(stillActive.status, "active");

  const destroyed = assertApiOk(await api(server, `${base}/destroy`, { method: "POST", token: ownerToken }), "owner destroys workspace");
  assert.equal(destroyed.status, "destroyed");
  const collaboratorRevoked = await api(server, base, { token: collaboratorToken });
  assert.equal(collaboratorRevoked.response.status, 410);
  const audit = assertApiOk(await api(server, `${base}/audit`, { token: ownerToken }), "read handoff audit");
  assert.ok(audit.some((event) => event.event_type === "workspace.handoff" && event.details.to_actor_id === "tc:identity:demo-collaborator"));
  assert.ok(audit.some((event) => event.event_type === "workspace.action_denied"));
  assert.ok(audit.some((event) => event.event_type === "workspace.destroyed" && event.details.access_revoked === true));

  writeEvidence("handoff-destruction-audit.json", {
    schema: "treatcode.workspace.handoff-destruction-audit.v1",
    workspace_id: workspace.id,
    commit,
    handoff: { actor_id: handoff.handoff.actor_id, scopes: handoff.handoff.scopes },
    denied_destroy_status: forbiddenDestroy.response.status,
    collaborator_access_revoked: collaboratorRevoked.response.status === 410,
    mutable_storage_removed: audit.find((event) => event.event_type === "workspace.destroyed")?.details.mutable_storage_removed === true,
    audit_events: audit.length,
  });
  console.log(JSON.stringify({ ok: true, workspace_id: workspace.id, denied_destroy: forbiddenDestroy.response.status, audit_events: audit.length }));
} finally {
  await server.close();
}

