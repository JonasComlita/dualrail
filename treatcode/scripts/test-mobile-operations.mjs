import fs from "node:fs";
import path from "node:path";
import { api, appRoot, assert, buildIfNeeded, jsonBody, startServer, writeEvidence } from "./operations-test-utils.mjs";

const checks = [];
const errors = [];
let server;

try {
  buildIfNeeded();
  const html = fs.readFileSync(path.join(appRoot, "dist", "operations", "index.html"), "utf8");
  const css = fs.readFileSync(path.join(appRoot, "src", "operations.css"), "utf8");
  assert(/name="viewport"/.test(html), "operations route has no viewport metadata");
  assert(html.includes("Stay in control when the connection drops"), "operations route has no static mobile entry content");
  assert(css.includes("@media (max-width: 390px)"), "operations CSS has no 390px responsive target");
  assert(css.includes("overflow-x: hidden"), "operations shell does not contain horizontal overflow");
  checks.push("operations route has a mobile viewport, static entry content, and a 390px layout target");

  server = await startServer();
  const health = await api(server, "/api/operations/health");
  assert(health.response.ok, `operations health failed: ${JSON.stringify(health.body)}`);
  assert(health.body.service?.continuation === true, "health does not advertise server continuation");
  checks.push("operations health exposes continuation, event stream, and backup/restore service capabilities");

  const overview = await api(server, "/api/operations/overview");
  assert(overview.response.ok && overview.body.tasks.length >= 4, "overview has no task board data");
  assert(overview.body.workspaces.length > 0 && overview.body.runners.length > 0, "overview is missing workspace or runner data");
  assert(overview.body.approvals.length > 0 && overview.body.reviews.length > 0, "overview is missing approval or review data");
  checks.push("task, workspace, runner, approval, review, notification, and audit views have data");

  const created = await api(server, "/api/operations/tasks", { method: "POST", ...jsonBody({ title: "Mobile stop control", kind: "test", commit: "abcdef1234567890" }) });
  assert(created.response.status === 201, "mobile task creation did not return 201");
  const started = await api(server, `/api/operations/tasks/${created.body.id}/actions`, { method: "POST", ...jsonBody({ action: "start" }) });
  assert(started.response.ok && started.body.status === "running", "mobile start control did not start a task");
  const stopped = await api(server, `/api/operations/tasks/${created.body.id}/actions`, { method: "POST", ...jsonBody({ action: "stop" }) });
  assert(stopped.response.ok && stopped.body.status === "cancelled", "mobile stop control did not cancel a task");
  assert(stopped.body.links.self === `/operations?task=${encodeURIComponent(created.body.id)}`, "task link is not stable");
  checks.push("a mobile client can create, start, stop, and retain a task link");
} catch (error) {
  errors.push(String(error?.message || error));
} finally {
  if (server) await server.stop();
}

const report = {
  schema: "treatcode.mobile_operations_e2e.v1",
  ok: errors.length === 0,
  viewport: { widthCssPixels: 390, heightCssPixels: 844 },
  route: "/operations",
  checks,
  screenshotManifest: [{ route: "/operations", viewport: "390x844", captureTarget: "mobile operations dashboard" }],
  errors,
};
writeEvidence("mobile-operations.json", report);
console.log(`P12 mobile operations: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
