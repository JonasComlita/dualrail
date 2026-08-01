import { api, assert, jsonBody, sleep, startServer, writeEvidence } from "./operations-test-utils.mjs";

const checks = [];
const errors = [];
let server;

try {
  server = await startServer();
  const created = await api(server, "/api/operations/tasks", { method: "POST", ...jsonBody({ title: "Disconnect and resume fixture", kind: "test", commit: "abcdef1234567890" }) });
  assert(created.response.status === 201, "could not create continuation fixture");
  const taskId = created.body.id;
  const started = await api(server, `/api/operations/tasks/${taskId}/actions`, { method: "POST", ...jsonBody({ action: "start" }) });
  assert(started.response.ok && started.body.status === "running", "continuation fixture did not start");
  const connected = await api(server, "/api/operations/sessions", { method: "POST", ...jsonBody({ clientId: "mobile-client-1", taskId }) });
  assert(connected.response.ok && connected.body.task.connection.connectedClients === 1, "client did not connect to task");
  const cursorBeforeDisconnect = connected.body.task.continuation.cursor;
  const disconnected = await api(server, "/api/operations/sessions/mobile-client-1", { method: "DELETE", ...jsonBody({}) });
  assert(disconnected.response.ok && disconnected.body.task.connection.connectedClients === 0, "disconnect did not remove the client");
  checks.push("client disconnect is recorded while the task remains server-owned");

  await sleep(1250);
  const continued = await api(server, `/api/operations/tasks/${taskId}`);
  assert(continued.response.ok && continued.body.status === "running", "task did not continue while clients were disconnected");
  assert(continued.body.continuation.cursor > cursorBeforeDisconnect, "task log cursor did not advance while disconnected");
  assert(continued.body.continuation.logsRetained === true, "task logs were not retained");
  checks.push("server continuation advanced the task and retained logs with zero connected clients");

  const resumed = await api(server, "/api/operations/sessions/mobile-client-1/resume", { method: "POST", ...jsonBody({ taskId }) });
  assert(resumed.response.ok && resumed.body.resumed === true, "resume endpoint did not acknowledge the client");
  assert(resumed.body.task.id === taskId && resumed.body.task.connection.connectedClients === 1, "resume did not return the same task state");
  const events = await api(server, "/api/operations/events?since=0");
  assert(events.body.data.some((event) => event.type === "client_disconnected"), "disconnect event is missing");
  assert(events.body.data.some((event) => event.type === "client_resumed"), "resume event is missing");
  checks.push("resume returns the same task and emits auditable disconnect/resume events");
} catch (error) {
  errors.push(String(error?.message || error));
} finally {
  if (server) await server.stop();
}

const report = { schema: "treatcode.disconnect_resume.v1", ok: errors.length === 0, checks, errors };
writeEvidence("disconnect-resume.json", report);
console.log(`P12 disconnect/resume: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
