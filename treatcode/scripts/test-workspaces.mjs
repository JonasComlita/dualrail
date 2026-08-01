import assert from "node:assert/strict";
import {
  api,
  assertApiOk,
  currentCommit,
  login,
  startWorkspaceServer,
  writeEvidence,
  repoRoot,
} from "./workspace-test-lib.mjs";

const server = await startWorkspaceServer();
try {
  const capabilities = assertApiOk(await api(server, "/api/workspaces/v1/capabilities"), "workspace capabilities");
  assert.ok(capabilities.supported_operations.includes("handoff"));
  assert.ok(capabilities.transferable_scopes.includes("workspace:edit"));
  const contract = await api(server, "/api/workspaces/v1/openapi.json");
  assert.equal(contract.response.status, 200);
  assert.equal(contract.payload.openapi, "3.0.3");
  const unauthenticated = await api(server, "/api/workspaces/v1/workspaces", { method: "POST", body: { repository: repoRoot, commit: currentCommit() } });
  assert.equal(unauthenticated.response.status, 401);
  const token = await login(server);
  const commit = currentCommit();
  const created = await api(server, "/api/workspaces/v1/workspaces", {
    method: "POST",
    token,
    body: { repository: repoRoot, commit, name: "P08 lifecycle fixture", image: "test/treatcode:workspace", toolchain: { name: "trit-test-toolchain", version: "fixture" } },
  });
  const workspace = assertApiOk(created, "create workspace");
  assert.equal(created.response.status, 201);
  assert.equal(workspace.base_commit, commit);
  assert.equal(workspace.commit, commit);
  assert.equal(workspace.mode, "isolated");
  assert.equal(workspace.image, "test/treatcode:workspace");
  assert.equal(workspace.toolchain.version, "fixture");

  const workspaceUrl = `/api/workspaces/v1/workspaces/${encodeURIComponent(workspace.id)}`;
  const inspected = assertApiOk(await api(server, workspaceUrl, { token }), "inspect workspace");
  assert.equal(inspected.id, workspace.id);

  const beforeSource = assertApiOk(await api(server, "/api/workspaces/v1/workspaces", { token }), "list workspaces");
  assert.ok(beforeSource.some((item) => item.id === workspace.id));
  const aliasList = assertApiOk(await api(server, "/api/v1/workspaces", { token }), "list workspace API alias");
  assert.ok(aliasList.some((item) => item.id === workspace.id));

  const fileName = "build/p08-workspace-fixture.txt";
  const content = `P08 isolated state ${Date.now()}\n`;
  const write = assertApiOk(await api(server, `${workspaceUrl}/files`, { method: "POST", token, body: { path: fileName, content, expected_version: 0 } }), "write workspace file");
  assert.equal(write.path, fileName);
  const read = assertApiOk(await api(server, `${workspaceUrl}/files?path=${encodeURIComponent(fileName)}`, { token }), "read workspace file");
  assert.equal(read.content, content);

  const snapshot = assertApiOk(await api(server, `${workspaceUrl}/snapshots`, { method: "POST", token, body: { label: "before disconnect" } }), "create snapshot");
  assert.equal(snapshot.tree_hash, write.tree_hash);
  const disconnected = assertApiOk(await api(server, `${workspaceUrl}/disconnect`, { method: "POST", token, body: { checkpoint: true, label: "disconnect checkpoint" } }), "disconnect workspace");
  assert.equal(disconnected.workspace.status, "suspended");
  const resumed = assertApiOk(await api(server, `${workspaceUrl}/resume`, { method: "POST", token }), "resume workspace");
  assert.equal(resumed.status, "active");
  const recoveredRead = assertApiOk(await api(server, `${workspaceUrl}/files?path=${encodeURIComponent(fileName)}`, { token }), "read resumed file");
  assert.equal(recoveredRead.content, content);

  const task = assertApiOk(await api(server, `${workspaceUrl}/tasks`, { method: "POST", token, body: { title: "Bounded P08 task", scope: "Verify workspace persistence", acceptance: ["The same file is present after resume."], priority: "high", context_scopes: ["docs/README.md"] } }), "create workspace task");
  const context = assertApiOk(await api(server, `${workspaceUrl}/tasks/${encodeURIComponent(task.id)}/context-package`, { method: "POST", token, body: { scopes: ["docs/README.md"], limits: { max_files: 4, max_bytes: 16384 } } }), "create context package");
  assert.equal(context.source_commit, commit);
  assert.ok(context.content_hash);
  const approvedTask = assertApiOk(await api(server, `${workspaceUrl}/tasks/${encodeURIComponent(task.id)}/approve`, { method: "POST", token }), "approve workspace task");
  assert.equal(approvedTask.status, "active");

  const doctor = assertApiOk(await api(server, `${workspaceUrl}/checks`, { method: "POST", token, body: { kind: "doctor" } }), "run isolated doctor");
  const smoke = assertApiOk(await api(server, `${workspaceUrl}/checks`, { method: "POST", token, body: { kind: "smoke" } }), "run isolated smoke preflight");
  assert.equal(doctor.authoritative_checkout_unchanged, true);
  assert.equal(smoke.authoritative_checkout_unchanged, true);

  const traversal = await api(server, `${workspaceUrl}/files`, { method: "POST", token, body: { path: "../outside.txt", content: "must reject" } });
  assert.equal(traversal.response.status, 400);
  assert.equal(traversal.payload.error.code, "invalid_workspace_path");

  const exported = assertApiOk(await api(server, `${workspaceUrl}/export`, { method: "POST", token }), "export workspace");
  assert.equal(exported.format, "treatcode.workspace.export.v1");
  assert.ok(exported.tree_hash);

  const destroyed = assertApiOk(await api(server, `${workspaceUrl}/destroy`, { method: "POST", token }), "destroy workspace");
  assert.equal(destroyed.status, "destroyed");
  const revoked = await api(server, workspaceUrl, { token });
  assert.equal(revoked.response.status, 410);
  assert.equal(revoked.payload.error.code, "workspace_destroyed");
  const audit = assertApiOk(await api(server, `${workspaceUrl}/audit`, { token }), "read destruction audit");
  assert.ok(audit.some((event) => event.event_type === "workspace.destroyed" && event.details.mutable_storage_removed === true));

  writeEvidence("workspace-report.json", {
    schema: "treatcode.workspace.lifecycle.report.v1",
    workspace_id: workspace.id,
    requested_commit: commit,
    created_status: workspace.status,
    resumed_status: resumed.status,
    context_package_hash: context.content_hash,
    checks: { doctor: doctor.ok, smoke_preflight: smoke.ok, authoritative_checkout_unchanged: doctor.authoritative_checkout_unchanged && smoke.authoritative_checkout_unchanged },
    destroyed_status: destroyed.status,
    audit_events: audit.length,
  });
  console.log(JSON.stringify({ ok: true, workspace_id: workspace.id, commit, audit_events: audit.length }));
} finally {
  await server.close();
}
