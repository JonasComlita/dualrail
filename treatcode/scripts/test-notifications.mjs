import { api, assert, jsonBody, startServer, writeEvidence } from "./operations-test-utils.mjs";

const checks = [];
const errors = [];
let server;

try {
  server = await startServer();
  const created = await api(server, "/api/operations/tasks", { method: "POST", ...jsonBody({ title: "Notification approval fixture", kind: "review", requiresApproval: true, commit: "abcdef1234567890" }) });
  assert(created.response.status === 201 && created.body.approvalId, "approval fixture was not created");
  const notifications = await api(server, "/api/operations/notifications?unread=true");
  const notification = notifications.body.data.find((candidate) => candidate.taskId === created.body.id);
  assert(notification, "approval notification is missing");
  assert(notification.link === `/operations?task=${encodeURIComponent(created.body.id)}`, "notification does not contain a stable task link");
  assert(notification.containsSecrets === false, "notification declares a secret payload");
  const { containsSecrets, ...notificationPayload } = notification;
  assert(!/secret|token|password|credential|private[_-]?key|authorization/i.test(JSON.stringify(notificationPayload)), "notification payload contains a secret-like value");
  checks.push("completion and attention notifications contain stable task links and no secrets");

  const blockedStart = await api(server, `/api/operations/tasks/${created.body.id}/actions`, { method: "POST", ...jsonBody({ action: "start" }) });
  assert(blockedStart.response.status === 409 && blockedStart.body.error.code === "APPROVAL_REQUIRED", "approval boundary did not block an unapproved start");
  const approved = await api(server, `/api/operations/tasks/${created.body.id}/actions`, { method: "POST", ...jsonBody({ action: "approve", actor: { id: "ops-reviewer", permission: "operations:approve", commit: "abcdef1234567890" } }) });
  assert(approved.response.ok, "approval action failed");
  const started = await api(server, `/api/operations/tasks/${created.body.id}/actions`, { method: "POST", ...jsonBody({ action: "start" }) });
  assert(started.response.ok && started.body.status === "running", "approved task could not start");
  checks.push("notifications and task actions preserve the explicit approval boundary");

  const preferences = await api(server, "/api/operations/notifications/preferences", { method: "PATCH", ...jsonBody({ completion: false, channels: ["in_app", "web_push"] }) });
  assert(preferences.response.ok && preferences.body.completion === false && preferences.body.channels.includes("web_push"), "notification preferences were not configurable");
  checks.push("operators can configure completion notifications and allowed delivery channels");

  const audit = await api(server, "/api/operations/audit?limit=100");
  const approvalAudit = audit.body.data.find((event) => event.action === "approval_granted" && event.resourceId === created.body.approvalId);
  assert(approvalAudit && approvalAudit.permission === "operations:approve" && approvalAudit.commit === "abcdef1234567890", "approval audit record lacks actor permission or commit");
  checks.push("approval audit history identifies actor, permission, and commit");
} catch (error) {
  errors.push(String(error?.message || error));
} finally {
  if (server) await server.stop();
}

const report = { schema: "treatcode.notifications.v1", ok: errors.length === 0, checks, errors };
writeEvidence("notifications.json", report);
console.log(`P12 notifications: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
