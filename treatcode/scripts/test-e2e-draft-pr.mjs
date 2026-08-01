import { actor, assert, expectFailure, loadContributionModule, writeReport } from "./p11-test-lib.mjs";

const checks = [];
const errors = [];

try {
  const module = await loadContributionModule();
  const github = new module.InMemoryGitHubApp();
  const service = new module.ContributionService({ github });
  const agent = actor(module, { actorId: "agent-p11-submit", actorType: "agent" });
  const content = new TextEncoder().encode("fn p11_contribution() -> t40 { return 0; }\n");
  const upload = service.createUploadSession(agent, {
    fileName: "ulib/p11_contribution.trit",
    totalBytes: content.byteLength,
    license: "MIT",
    taskId: agent.taskId,
    projectId: agent.projectId,
    provenance: { author: "agent-p11-submit", source: "original" },
  });
  service.putUploadChunk(agent, upload.id, 0, content);
  service.finalizeUpload(agent, upload.id);
  const workspace = service.createWorkspace(agent, { repository: "https://github.com/JonasComlita/dualrail", baseCommit: "d168bc8", taskId: agent.taskId });
  service.materializeUpload(agent, workspace.id, upload.id);
  const validation = service.validateWorkspace(agent, workspace.id);
  const correctness = service.attachEvidence(agent, workspace.id, {
    kind: "correctness",
    label: "isolated correctness fixture",
    payload: JSON.stringify({ passed: true, fixture: "p11-draft-pr" }),
    sourceCommit: "d168bc8",
    runnerImage: "treatcode-runner-test-v1",
    command: "fixture correctness",
  });
  const benchmark = service.attachEvidence(agent, workspace.id, {
    kind: "benchmark",
    label: "immutable benchmark fixture",
    payload: JSON.stringify({ cycles: 12, fixture: "p11-draft-pr" }),
    sourceCommit: "d168bc8",
    runnerImage: "treatcode-runner-test-v1",
    command: "fixture benchmark",
  });
  assert(validation.passed && correctness.immutable && benchmark.immutable, "workspace evidence was not immutable after validation");
  checks.push("draft-PR input requires a passing isolated workspace and immutable correctness plus benchmark evidence");

  await expectFailure(() => service.submitDraftPullRequest(agent, workspace.id, { title: "Add P11 contribution" }), "HUMAN_APPROVAL_REQUIRED");
  assert(github.operations.length === 0, "draft-PR approval failure caused GitHub mutations");
  checks.push("draft-PR creation stops before branch creation when human approval is absent");

  const agentWithApprovalScope = actor(module, {
    actorId: "agent-p11-approver-attempt",
    actorType: "agent",
    scopes: [...module.CONTRIBUTION_ACTIONS.filter((scope) => scope !== "merge")],
  });
  await expectFailure(() => service.recordHumanApproval(agentWithApprovalScope, workspace.id, { decision: "approved" }), "HUMAN_APPROVAL_REQUIRED");
  checks.push("an agent cannot manufacture the human approval record even when it holds the approval action name");

  const human = actor(module, {
    actorId: "security-reviewer-p11",
    actorType: "human",
    scopes: ["draft-pr:approve", "workspace:read"],
  });
  const approval = service.recordHumanApproval(human, workspace.id, { decision: "approved" });
  assert(approval.actorType === "human" && approval.reviewer === human.actorId, "approval was not tied to an authenticated human reviewer");
  checks.push("human approval is recorded with reviewer, decision, scope, base commit, and timestamp");

  const limited = actor(module, {
    actorId: "agent-p11-limited",
    scopes: ["draft-pr:create", "commit:create", "push"],
  });
  await expectFailure(() => service.submitDraftPullRequest(limited, workspace.id, { title: "Must not create branch" }), "FORBIDDEN_SCOPE");
  assert(github.operations.length === 0, "missing branch scope still caused a GitHub operation");
  checks.push("branch, commit, push, and draft-PR permissions are preflighted independently");

  const submitted = await service.submitDraftPullRequest(agent, workspace.id, {
    title: "Add P11 contribution intake fixture",
    message: "Add a quarantined, evidence-linked contribution fixture",
    branch: "contrib/p11-intake-fixture",
  });
  assert(submitted.draft === true && submitted.url.includes("/pull/1"), "GitHub App adapter did not create a draft pull request");
  assert(submitted.evidenceHashes.includes(correctness.artifactHash) && submitted.evidenceHashes.includes(benchmark.artifactHash), "draft PR omitted immutable evidence hashes");
  assert(github.operations.map((operation) => operation.action).join(",") === "branch:create,commit:create,push,draft-pr:create", "GitHub operations did not follow branch/commit/push/draft-PR order");
  checks.push("the end-to-end fixture creates an isolated branch, commit, push, and draft PR with evidence hashes attached");

  const mergeActor = actor(module, { actorId: "agent-p11-merge-attempt", scopes: [...module.CONTRIBUTION_ACTIONS] });
  await expectFailure(() => service.mergePullRequest(mergeActor, workspace.id), "MERGE_NOT_ALLOWED");
  const auditReader = actor(module, { actorId: "audit-reader", scopes: ["audit:read"] });
  const audit = service.auditLog(auditReader);
  assert(audit.length >= 10 && audit.every((entry, index) => index === 0 ? entry.previousHash === null : entry.previousHash === audit[index - 1].recordHash), "audit records are not hash chained");
  checks.push("merge authority is denied and security-relevant decisions remain in a hash-chained audit log");

  const appRequests = [];
  const appResponses = [
    { ref: "refs/heads/contrib/p11-client" },
    { tree: { sha: "tree-base" } },
    { sha: "blob-1" },
    { sha: "tree-1" },
    { sha: "commit-1" },
    {},
    { number: 42, html_url: "https://github.example.test/JonasComlita/dualrail/pull/42" },
  ];
  const appClient = new module.GitHubAppClient({
    tokenProvider: { installationToken: async () => "test-installation-token" },
    apiBaseUrl: "https://api.github.example.test",
    fetchImpl: async (url, init) => {
      appRequests.push({ url, method: init.method, headers: init.headers, body: init.body });
      const body = appResponses.shift();
      return { ok: true, status: 200, text: async () => "", json: async () => body };
    },
  });
  await appClient.createBranch({ repository: "github.com/JonasComlita/dualrail", branch: "contrib/p11-client", baseCommit: "d168bc8" });
  await appClient.createCommit({ repository: "github.com/JonasComlita/dualrail", branch: "contrib/p11-client", baseCommit: "d168bc8", message: "P11 client fixture", files: [{ path: "ulib/p11.trit", bytes: content.byteLength, contentHash: "sha256:" + "1".repeat(64), uploadId: upload.id, contentBase64: Buffer.from(content).toString("base64") }], evidence: [] });
  await appClient.pushBranch({ repository: "github.com/JonasComlita/dualrail", branch: "contrib/p11-client", commit: "commit-1" });
  const clientPr = await appClient.createDraftPullRequest({ repository: "github.com/JonasComlita/dualrail", branch: "contrib/p11-client", title: "P11 client fixture", body: "draft", commit: "commit-1", draft: true });
  assert(clientPr.number === 42 && appRequests.length === 7, "GitHub App client did not issue the expected branch/data/PR requests");
  assert(appRequests.every((request) => request.headers.Authorization === "Bearer test-installation-token"), "GitHub App client did not use the injected installation token");
  assert(appRequests.map((request) => request.method).join(",") === "POST,GET,POST,POST,POST,PATCH,POST", "GitHub App client request order is incorrect");
  checks.push("the production-shaped GitHub App adapter uses an injected installation token for branch, Git data, push, and draft-PR calls");

  writeReport("draft-pr-e2e.json", "treatcode.p11.draft-pr-e2e.v1", checks, errors, {
    repository: workspace.repository,
    workspace_id: workspace.id,
    base_commit: workspace.baseCommit,
    validation_artifact_hash: validation.artifactHash,
    approval_id: approval.id,
    pull_request: submitted,
    github_operations: github.operations,
    audit_count: audit.length,
  });
} catch (error) {
  errors.push(String(error?.stack || error));
  writeReport("draft-pr-e2e.json", "treatcode.p11.draft-pr-e2e.v1", checks, errors);
  process.exitCode = 1;
}
