import { strict as assert } from "node:assert";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { AuthStore, DEFAULT_PROJECT_ID } from "../src/auth";
import { CommunityStore } from "../src/communityStore";

const root = mkdtempSync(join(tmpdir(), "treatcode-p14-account-community-"));
const identityPath = join(root, "identities.json");
const communityPath = join(root, "community.json");
let now = Date.parse("2026-08-20T00:00:00.000Z");

try {
  const auth = new AuthStore({ identity_path: identityPath, audit_path: join(root, "audit.jsonl"), now: () => now });
  assert.throws(() => auth.registerParticipant({ handle: "ab", password: "too-short" }), /invalid_handle|invalid_password_length/);
  assert.throws(() => auth.registerParticipant({ handle: "valid_handle", password: "too-short" }), /invalid_password_length/);
  assert.throws(() => auth.registerParticipant({ handle: "valid_handle", password: "x".repeat(129) }), /invalid_password_length/);
  const identity = auth.registerParticipant({ handle: "ternary_dev", password: "correct horse battery" });
  assert.equal(identity.handle, "ternary_dev");
  assert.deepEqual(identity.roles, ["participant"]);
  assert.throws(() => auth.registerParticipant({ handle: "TERNARY_DEV", password: "another correct password" }), /duplicate_handle/);

  const login = auth.login({ handle: "TERNARY_DEV", password: "correct horse battery" });
  assert(login.ok);
  assert.deepEqual(login.credential.credential.actions, ["read", "test", "benchmark", "artifact"]);
  const deniedEdit = auth.authorize({ token: login.credential.token, action: "edit", project_id: DEFAULT_PROJECT_ID, require_nonce: false });
  assert(!deniedEdit.allowed && deniedEdit.denial.code === "action_not_granted");

  const restarted = new AuthStore({ identity_path: identityPath, audit_path: null, now: () => now });
  const restartedLogin = restarted.loginParticipant("ternary_dev", "correct horse battery");
  assert(restartedLogin.ok, "participant login should survive an AuthStore restart");
  const identityState = readFileSync(identityPath, "utf8");
  assert(!identityState.includes("correct horse battery"));
  assert(identityState.includes("access_key_hash") && identityState.includes("access_key_salt"));

  const community = new CommunityStore({ state_path: communityPath, now: () => now });
  const actor = { identity: restartedLogin.identity, credential: restartedLogin.credential.credential };
  const solution = community.saveSolutionRevision(actor, {
    challenge_id: "T001",
    title: "A plain text solution",
    code: "fn main() -> t40 { return 0; }",
    metadata: { username: "caller-controlled-name", password: "must-not-persist" },
  });
  assert.equal(solution.owner_identity_id, restartedLogin.identity.id);
  assert.equal(solution.owner_handle, "ternary_dev");
  assert.equal(solution.visibility, "private");
  assert.deepEqual(solution.metadata, { username: "caller-controlled-name" });
  const submission = community.recordChallengeSubmission(actor, {
    problem_id: "T001",
    code: "fn main() -> t40 { return 0; }",
    username: "spoofed" as never,
    metadata: { access_token: "must-not-persist", engine: "native" },
  });
  assert.equal(submission.owner_handle, "ternary_dev");
  assert.notEqual(submission.owner_handle, "spoofed");
  assert.equal(community.listPublicSolutions({ challenge_id: "T001" }).length, 0, "private benchmark drafts must not appear in the public solution feed");

  const postedSolutionId = "tc:solution:posted";
  const postedSubmission = community.recordChallengeSubmission(actor, {
    problem_id: "T001",
    solution_id: postedSolutionId,
    code: "fn main() -> t40 { return 1; }",
    outcome: "accepted",
    engine: "bootstrap",
    cycles: 12,
    metrics: {
      runtime_ms: 14,
      memory_kib: 1024,
      cycles: 12,
      compile_cycles: 88,
      tests_passed: 3,
      tests_total: 3,
      engine: "bootstrap",
      opt_level: "-O2",
    },
  });
  const published = community.publishSolution(actor, {
    challenge_id: "T001",
    solution_id: postedSolutionId,
    title: "A posted practice solution",
    code: "fn main() -> t40 { return 1; }",
    visibility: "public",
    explanation: "Return the required value directly after verification confirms the contract.",
    pseudocode: "return 1",
  });
  const postedSolution = published.solution;
  assert.equal(postedSubmission.solution_id, postedSolution.solution_id);
  assert.equal(postedSubmission.metrics.runtime_ms, 14);
  assert.equal(postedSubmission.metrics.memory_kib, 1024);
  const publicSolutions = community.listPublicSolutions({ challenge_id: "T001" });
  assert.equal(publicSolutions.length, 1);
  assert.equal(publicSolutions[0].owner_handle, "ternary_dev");
  assert.equal(publicSolutions[0].code, postedSolution.code);
  assert.equal(publicSolutions[0].solved, true);
  assert.equal(publicSolutions[0].upvotes, 0);
  assert.equal(publicSolutions[0].metrics?.runtime_ms, 14);
  assert(publicSolutions[0].discussion_body.includes("Plain-English explanation"));
  assert.equal(community.toggleSolutionVote(actor, postedSolution.solution_id).upvotes, 1);
  assert.equal(community.toggleSolutionVote(actor, postedSolution.solution_id).upvotes, 0);
  assert.equal(community.toggleSolutionVote(actor, postedSolution.solution_id).upvotes, 1);
  const discussion = community.addDiscussion(actor, { challenge_id: "T001", body: "<script>alert(1)</script>A plain-text note." });
  assert.equal(discussion.author_handle, "ternary_dev");
  assert.equal(discussion.body, "alert(1)A plain-text note.");

  const communityRestart = new CommunityStore({ state_path: communityPath, now: () => now });
  assert.equal(communityRestart.listSolutionRevisions({ challenge_id: "T001" }).length, 2);
  assert.equal(communityRestart.listChallengeSubmissions({ challenge_id: "T001" }).length, 2);
  assert.equal(communityRestart.listPublicSolutions({ challenge_id: "T001" })[0].solved, true);
  assert.equal(communityRestart.listPublicSolutions({ challenge_id: "T001" })[0].upvotes, 1);
  assert.equal(communityRestart.listDiscussions({ challenge_id: "T001" }).length, 2);
  const state = readFileSync(communityPath, "utf8");
  assert(state.includes('"schema_version":"treatcode.community.state.v1"'));
  assert(!state.includes("must-not-persist"));
  console.log("P14 account/community: passed");
} finally {
  rmSync(root, { recursive: true, force: true });
}
