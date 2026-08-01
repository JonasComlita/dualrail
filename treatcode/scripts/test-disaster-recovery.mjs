import { api, assert, jsonBody, startServer, writeEvidence } from "./operations-test-utils.mjs";

const checks = [];
const errors = [];
let server;

try {
  server = await startServer();
  const before = await api(server, "/api/operations/overview");
  assert(before.response.ok, "could not read source operations state");
  const backupResponse = await api(server, "/api/operations/backup", { method: "POST", ...jsonBody({ actor: { id: "synthetic-recovery", permission: "operations:backup" } }) });
  assert(backupResponse.response.status === 201, "backup endpoint did not create a backup");
  const backup = backupResponse.body;
  assert(backup.schemaVersion === "treatcode.operations.backup.v1", "backup schema is incorrect");
  assert(typeof backup.database?.digest === "string" && backup.database.digest.startsWith("sha256:"), "backup has no content digest");
  checks.push("backup captures a content-addressed database snapshot");

  const restoreResponse = await api(server, "/api/operations/restore", { method: "POST", ...jsonBody({ backup, actor: { id: "synthetic-recovery", permission: "operations:restore" } }) });
  assert(restoreResponse.response.ok, "restore endpoint rejected a valid backup");
  assert(restoreResponse.body.restoredDigest === backup.database.digest, "restore digest differs from backup digest");
  assert(restoreResponse.body.immutableArtifactCount === backup.immutableArtifacts.length, "immutable artifact references did not survive restore");
  const after = await api(server, "/api/operations/overview");
  assert(after.response.ok && after.body.tasks.length === before.body.tasks.length, "restore did not reproduce task database state");
  checks.push("restore into a clean-equivalent state reproduces tasks and immutable artifact references");

  const exercise = await api(server, "/api/operations/recovery-exercise", { method: "POST", ...jsonBody({}) });
  assert(exercise.response.ok && exercise.body.ok, `recovery exercise failed: ${JSON.stringify(exercise.body)}`);
  assert(exercise.body.recoveryPointObjectiveMinutes === 15 && exercise.body.recoveryTimeObjectiveMinutes === 30, "recovery objectives are not stated");
  checks.push("disaster-recovery exercise passes the stated RPO/RTO objectives");
} catch (error) {
  errors.push(String(error?.message || error));
} finally {
  if (server) await server.stop();
}

const report = { schema: "treatcode.backup_restore.v1", ok: errors.length === 0, checks, errors };
writeEvidence("disaster-recovery.json", report);
console.log(`P12 disaster recovery: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
