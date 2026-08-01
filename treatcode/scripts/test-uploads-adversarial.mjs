import { actor, assert, buildZip, expectFailure, loadContributionModule, sha256, writeReport } from "./p11-test-lib.mjs";

const checks = [];
const errors = [];

try {
  const module = await loadContributionModule();
  const service = new module.ContributionService();
  const agent = actor(module);
  const provenance = { author: "adversarial-fixture", source: "original" };
  const create = (fileName, bytes, overrides = {}) => service.createUploadSession(agent, {
    fileName,
    totalBytes: bytes.byteLength,
    license: "MIT",
    provenance,
    taskId: agent.taskId,
    projectId: agent.projectId,
    ...overrides,
  });

  await expectFailure(() => create("../escape.trit", new TextEncoder().encode("safe")), "PATH_TRAVERSAL");
  await expectFailure(() => create("encoded/%2e%2e/escape.trit", new TextEncoder().encode("safe")), "PATH_TRAVERSAL");
  checks.push("plain and URL-encoded path traversal is rejected before a session is created");

  await expectFailure(() => create("payload.exe", new Uint8Array([1, 2, 3])), "FORBIDDEN_TYPE");
  await expectFailure(() => create("payload.trit", new Uint8Array(9 * 1024 * 1024)), "SIZE_LIMIT");
  await expectFailure(() => service.createUploadSession(agent, {
    fileName: "missing.trit",
    totalBytes: 4,
    license: "MIT",
    taskId: agent.taskId,
  }), "PROVENANCE_REQUIRED");
  await expectFailure(() => create("license.trit", new TextEncoder().encode("safe"), { license: "GPL-3.0" }), "LICENSE_UNAUTHORIZED");
  checks.push("forbidden types, oversized content, missing provenance, and unauthorized licenses are rejected");

  const unsafe = new Uint8Array([0x4d, 0x5a, 0x90, 0x00]);
  const unsafeSession = create("payload.trit", unsafe);
  service.putUploadChunk(agent, unsafeSession.id, 0, unsafe);
  await expectFailure(() => service.finalizeUpload(agent, unsafeSession.id), "UNSAFE_CONTENT");
  assert(service.getUploadSession(agent, unsafeSession.id).status === "rejected", "unsafe content did not close the upload in rejected state");
  checks.push("executable magic and binary content are rejected in quarantine");

  const traversalArchive = buildZip([{ name: "../escape.trit", data: "fn escape() -> t40 { return 0; }", method: 0 }]);
  const archiveSession = create("bundle.zip", traversalArchive);
  service.putUploadChunk(agent, archiveSession.id, 0, traversalArchive);
  await expectFailure(() => service.finalizeUpload(agent, archiveSession.id), "PATH_TRAVERSAL");

  const bombArchive = buildZip([{ name: "zeros.trit", data: Buffer.alloc(4 * 1024 * 1024), method: 8 }]);
  const bombSession = create("bomb.zip", bombArchive);
  service.putUploadChunk(agent, bombSession.id, 0, bombArchive);
  await expectFailure(() => service.finalizeUpload(agent, bombSession.id), "ARCHIVE_BOMB");
  checks.push("archive path traversal and high-expansion archive bombs are rejected");

  const overlap = create("overlap.trit", new TextEncoder().encode("abcd"));
  service.putUploadChunk(agent, overlap.id, 0, new TextEncoder().encode("ab"));
  await expectFailure(() => service.putUploadChunk(agent, overlap.id, 1, new TextEncoder().encode("bc")), "CHUNK_OVERLAP");
  checks.push("overlapping resumable ranges are rejected instead of silently rewriting bytes");

  const hashMismatch = create("hash.trit", new TextEncoder().encode("abcd"), { expectedHash: sha256(new TextEncoder().encode("wxyz")) });
  const hashContent = new TextEncoder().encode("abcd");
  service.putUploadChunk(agent, hashMismatch.id, 0, hashContent);
  await expectFailure(() => service.finalizeUpload(agent, hashMismatch.id), "CONTENT_HASH_MISMATCH");
  checks.push("declared content hashes are checked after all chunks are assembled");

  writeReport("upload-adversarial.json", "treatcode.p11.upload-adversarial.v1", checks, errors);
} catch (error) {
  errors.push(String(error?.stack || error));
  writeReport("upload-adversarial.json", "treatcode.p11.upload-adversarial.v1", checks, errors);
  process.exitCode = 1;
}
