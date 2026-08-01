import { actor, assert, loadContributionModule, sha256, writeReport } from "./p11-test-lib.mjs";

const checks = [];
const errors = [];

try {
  const module = await loadContributionModule();
  const service = new module.ContributionService();
  const agent = actor(module);
  const content = new TextEncoder().encode("// P11 resumable contribution\n" + "fn contribution_marker() -> t40 { return 0; }\n".repeat(5000));
  const expectedHash = sha256(content);
  const session = service.createUploadSession(agent, {
    fileName: "contributions/addition.trit",
    contentType: "text/x-trit",
    totalBytes: content.byteLength,
    expectedHash,
    license: "MIT",
    taskId: agent.taskId,
    projectId: agent.projectId,
    provenance: { author: "p11-agent", source: "original" },
  });
  assert(session.status === "open" && session.quarantine === true, "upload session did not start in quarantine");
  checks.push("upload sessions require provenance, license, task, size, and expected content hash");

  const split = Math.min(150 * 1024, content.byteLength - 1);
  const first = service.putUploadChunk(agent, session.id, 0, content.slice(0, split));
  assert(first.contiguousBytes === split && first.receivedBytes === split, "first resumable range was not recorded");
  const resumed = service.getUploadSession(agent, session.id);
  assert(resumed.receivedRanges.length === 1 && resumed.contentHash === undefined, "interrupted upload exposed an incorrect content hash");
  service.putUploadChunk(agent, session.id, split, content.slice(split));
  const accepted = service.finalizeUpload(agent, session.id);
  assert(accepted.status === "accepted" && accepted.contentHash === expectedHash, "resumed upload changed the assembled content hash");
  checks.push("interrupted uploads resume by byte range and preserve the resulting SHA-256 hash");

  const workspace = service.createWorkspace(agent, { repository: "https://github.com/JonasComlita/dualrail", baseCommit: "d168bc8", taskId: agent.taskId });
  service.materializeUpload(agent, workspace.id, session.id);
  const validation = service.validateWorkspace(agent, workspace.id);
  assert(validation.passed && validation.inputHashes.includes(expectedHash), "accepted upload did not pass isolated workspace validation");
  const inspected = service.getWorkspace(agent, workspace.id);
  assert(inspected.authoritativeSourceTouched === false && inspected.files.length === 1, "materialization touched authoritative source or lost the file");
  checks.push("accepted uploads materialize only into an isolated workspace and require a second validation pass");

  writeReport("upload-tests.json", "treatcode.p11.upload-tests.v1", checks, errors, {
    content_hash: expectedHash,
    upload_id: session.id,
    workspace_id: workspace.id,
    validation_artifact_hash: validation.artifactHash,
  });
} catch (error) {
  errors.push(String(error?.stack || error));
  writeReport("upload-tests.json", "treatcode.p11.upload-tests.v1", checks, errors);
  process.exitCode = 1;
}
