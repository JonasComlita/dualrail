import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { afterEach, describe, test } from "bun:test";
import {
  FEATURE_KEYS,
  checkpointAuthorShard,
  createAuthorShardScaffold,
  isExcludedSubjectModel,
  loadAuthorShardPlan,
  sha256,
  validateAuthorShardSet,
} from "./intelligence-v31-author-shards.mjs";

const repositoryRoot = path.resolve(import.meta.dir, "..", "..");
const temporaryRoots: string[] = [];

function temporaryRoot() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "treatcode-v31-author-shard-"));
  temporaryRoots.push(root);
  return root;
}

function uniqueAnchor(relative: string): string {
  const text = fs.readFileSync(path.join(repositoryRoot, relative), "utf8").replaceAll("\r\n", "\n");
  const line = text.split("\n").find((candidate) => candidate.trim().length >= 20 && text.split(candidate).length === 2);
  if (!line) throw new Error(`no stable unique line in ${relative}`);
  return line;
}

function validContribution(shardId = "V31-SHARD-001") {
  const loaded = loadAuthorShardPlan(repositoryRoot);
  const assignment = loaded.assignments.find((item) => item.shard_id === shardId)!;
  const participantFiles = ["kernel.trit", "apps/os_sdk.trit", "apps/libwidget.trit", "ternary_compiler_ir.h", "ternary_compiler_codegen.h", "TEST_MANIFEST.json", "SYSCALL_MANIFEST.json", "APP_MANIFEST.json"];
  const distractorFiles = ["IMAGE_FORMAT_MANIFEST.json", "ROADMAP_STATUS.json", "KNOWN_GAPS.md"];
  const testManifest = JSON.parse(fs.readFileSync(path.join(repositoryRoot, "TEST_MANIFEST.json"), "utf8"));
  const suite = testManifest.suites.find((item: { targets?: string[] }) => item.targets?.length);
  const firstAnchor = uniqueAnchor("kernel.trit");
  const secondAnchor = uniqueAnchor("apps/os_sdk.trit");
  return {
    schema: "treatcode.intelligence.author-contribution.v3.1",
    version: "3.1",
    shard_id: shardId,
    authorship: {
      contributor_id: `fixture-author-${shardId}`,
      contributor_kind: "model_agent",
      model: "gpt-5.4-mini",
      reasoning_effort: "xhigh",
      completed_at: "2026-09-05T12:00:00.000Z",
      attestation: "I independently authored these five unseen task designs without subject-model assistance.",
    },
    tasks: assignment.task_ids.map((taskId, index) => ({
      task_id: taskId,
      track: assignment.tracks[index],
      category: assignment.categories[index],
      title: `Independent repository repair fixture ${taskId}`,
      initial_signal: `Observed repository failure signal unique to ${taskId}.`,
      participant_files: participantFiles,
      editable_files: ["kernel.trit", "apps/os_sdk.trit", "apps/libwidget.trit"],
      mutations: [
        { path: "kernel.trit", before: firstAnchor, after: `${firstAnchor} `, invariant: "Kernel and SDK state transitions must remain aligned.", hidden_test_rationale: "A hidden transition sequence catches local-only repairs." },
        { path: "apps/os_sdk.trit", before: secondAnchor, after: `${secondAnchor} `, invariant: "SDK and kernel representations must agree across the ABI.", hidden_test_rationale: "An integration probe catches repairs that change only one layer." },
      ],
      distractor_files: distractorFiles,
      public_signal: `Sparse public symptom for ${taskId}.`,
      focused_suites: [suite.name],
      focused_targets: [suite.targets[0]],
      required_interaction: "The kernel transition and SDK wrapper must be reasoned about together.",
      invariant: "The same balanced-ternary state must cross both repository layers unchanged.",
      hidden_test_rationale: "Hidden sequences vary state and call ordering so examples cannot be hard-coded.",
      design_features: Object.fromEntries(FEATURE_KEYS.map((feature) => [feature, true])),
    })),
  };
}

afterEach(() => {
  for (const root of temporaryRoots.splice(0)) fs.rmSync(root, { recursive: true, force: true });
});

describe("Intelligence v3.1 recoverable author shards", () => {
  test("fixes twenty five-task shards with exact category and feature coverage", () => {
    const loaded = loadAuthorShardPlan(repositoryRoot);
    assert.equal(loaded.assignments.length, 20);
    assert.equal(new Set(loaded.assignments.flatMap((assignment) => assignment.task_ids)).size, 100);
    assert.deepEqual(loaded.categoryCounts, loaded.plan.category_mix);
    const trackCounts = Object.fromEntries(Object.keys(loaded.plan.track_mix).map((track) => [track, loaded.assignments.flatMap((assignment) => assignment.tracks).filter((value) => value === track).length]));
    assert.deepEqual(trackCounts, loaded.plan.track_mix);
    assert.deepEqual(Object.fromEntries(FEATURE_KEYS.map((feature) => [feature, loaded.assignments.reduce((total, assignment) => total + assignment.feature_minimums[feature], 0)])), {
      repository_exploration_required: 100,
      multi_module_change: 80,
      misleading_or_incomplete_tests: 40,
      conflicting_invariants: 40,
      initial_build_failure: 30,
      abi_or_integration_bug: 30,
      performance_constraint: 25,
    });
  });

  test("excludes Luna and Sol family variants instead of only exact model labels", () => {
    assert.equal(isExcludedSubjectModel("gpt-5.6-luna"), true);
    assert.equal(isExcludedSubjectModel("gpt-5.6-sol-extra-high"), true);
    assert.equal(isExcludedSubjectModel("openai/gpt-5.6-sol/high"), true);
    assert.equal(isExcludedSubjectModel("gpt-5.4-mini"), false);
  });

  test("creates a complete five-slot scaffold before any task content is authored", () => {
    const scaffold = createAuthorShardScaffold({ repositoryRoot, shardId: "V31-SHARD-020", contributorId: "fixture-author", model: "gpt-5.4-mini", reasoningEffort: "xhigh" });
    assert.equal(scaffold.shard_id, "V31-SHARD-020");
    assert.deepEqual(scaffold.tasks.map((task) => task.task_id), ["TC-V31-FINAL-096", "TC-V31-FINAL-097", "TC-V31-FINAL-098", "TC-V31-FINAL-099", "TC-V31-FINAL-100"]);
    assert.equal(scaffold.tasks.length, 5);
    assert.deepEqual(scaffold.tasks.map((task) => task.track), loadAuthorShardPlan(repositoryRoot).assignments.find((assignment) => assignment.shard_id === "V31-SHARD-020")!.tracks);
    assert.throws(() => createAuthorShardScaffold({ repositoryRoot, shardId: "V31-SHARD-020", contributorId: "fixture-author", model: "gpt-5.6-luna-max", reasoningEffort: "max" }), /non-Luna\/non-Sol/);
  });

  test("publishes an immutable checkpoint directory and recovers without staging", () => {
    const root = temporaryRoot();
    const stagedRoot = path.join(root, "staged");
    const checkpointRoot = path.join(root, "checkpoints");
    const evidenceRoot = path.join(root, "evidence");
    fs.mkdirSync(stagedRoot, { recursive: true });
    const contributionPath = path.join(stagedRoot, "V31-SHARD-001.json");
    fs.writeFileSync(contributionPath, `${JSON.stringify(validContribution(), null, 2)}\n`);
    const checkpoint = checkpointAuthorShard({ repositoryRoot, shardId: "V31-SHARD-001", stagedRoot, checkpointRoot, evidenceRoot, now: new Date("2026-09-05T12:30:00.000Z") });
    assert.equal(checkpoint.status, "checkpointed");
    assert.match(checkpoint.receipt.contribution_sha256, /^[a-f0-9]{64}$/);
    assert.equal(Object.keys(checkpoint.receipt.task_sha256).length, 5);
    fs.rmSync(contributionPath);
    const evidencePath = path.join(evidenceRoot, "V31-SHARD-001.receipt.json");
    fs.rmSync(evidencePath);
    const replayed = checkpointAuthorShard({ repositoryRoot, shardId: "V31-SHARD-001", stagedRoot, checkpointRoot, evidenceRoot });
    assert.equal(replayed.status, "already_checkpointed");
    assert.equal(fs.existsSync(evidencePath), true);
    fs.mkdirSync(path.join(checkpointRoot, ".V31-SHARD-001.tmp-interrupted"));
    const recovered = validateAuthorShardSet({ repositoryRoot, selectedShardId: "V31-SHARD-001", requireCheckpoints: true, stagedRoot, checkpointRoot });
    assert.equal(recovered.errors.length, 0);
    assert.equal(recovered.records[0].status, "checkpointed");
    assert.equal(recovered.taskCount, 5);
  });

  test("rejects staged replacement after checkpoint while preserving the accepted shard", () => {
    const root = temporaryRoot();
    const stagedRoot = path.join(root, "staged");
    const checkpointRoot = path.join(root, "checkpoints");
    fs.mkdirSync(stagedRoot, { recursive: true });
    const contribution = validContribution();
    const contributionPath = path.join(stagedRoot, "V31-SHARD-001.json");
    fs.writeFileSync(contributionPath, `${JSON.stringify(contribution, null, 2)}\n`);
    checkpointAuthorShard({ repositoryRoot, shardId: "V31-SHARD-001", stagedRoot, checkpointRoot, evidenceRoot: path.join(root, "evidence") });
    contribution.authorship.attestation += " Mutated after checkpoint.";
    fs.writeFileSync(contributionPath, `${JSON.stringify(contribution, null, 2)}\n`);
    const checked = validateAuthorShardSet({ repositoryRoot, selectedShardId: "V31-SHARD-001", requireCheckpoints: true, stagedRoot, checkpointRoot });
    assert.equal(checked.records[0].status, "checkpoint_mismatch");
    assert.match(checked.errors.join("\n"), /differ from the immutable checkpoint/);
    assert.throws(() => checkpointAuthorShard({ repositoryRoot, shardId: "V31-SHARD-001", stagedRoot, checkpointRoot, evidenceRoot: path.join(root, "evidence") }), /binds different contribution bytes/);
  });

  test("validates one shard independently when every other assignment is absent", () => {
    const root = temporaryRoot();
    const stagedRoot = path.join(root, "staged");
    fs.mkdirSync(stagedRoot, { recursive: true });
    fs.writeFileSync(path.join(stagedRoot, "V31-SHARD-001.json"), `${JSON.stringify(validContribution(), null, 2)}\n`);
    const selected = validateAuthorShardSet({ repositoryRoot, selectedShardId: "V31-SHARD-001", stagedRoot, checkpointRoot: path.join(root, "checkpoints") });
    assert.equal(selected.errors.length, 0);
    assert.equal(selected.records[0].status, "valid_uncheckpointed");
    const aggregate = validateAuthorShardSet({ repositoryRoot, requireCheckpoints: true, stagedRoot, checkpointRoot: path.join(root, "checkpoints") });
    assert.equal(aggregate.records[0].status, "valid_uncheckpointed");
    assert.equal(aggregate.records.slice(1).every((record) => record.status === "missing"), true);
  });

  test("assembles exactly 100 tasks from twenty independently checkpointed shards", () => {
    const root = temporaryRoot();
    const stagedRoot = path.join(root, "staged");
    const checkpointRoot = path.join(root, "checkpoints");
    const evidenceRoot = path.join(root, "evidence");
    fs.mkdirSync(stagedRoot, { recursive: true });
    const assignments = loadAuthorShardPlan(repositoryRoot).assignments;
    for (const assignment of assignments) {
      fs.writeFileSync(path.join(stagedRoot, assignment.file), `${JSON.stringify(validContribution(assignment.shard_id), null, 2)}\n`);
      checkpointAuthorShard({ repositoryRoot, shardId: assignment.shard_id, stagedRoot, checkpointRoot, evidenceRoot });
    }
    const aggregate = validateAuthorShardSet({ repositoryRoot, requireCheckpoints: true, stagedRoot, checkpointRoot });
    assert.equal(aggregate.errors.length, 0);
    assert.equal(aggregate.records.length, 20);
    assert.equal(aggregate.records.every((record) => record.status === "checkpointed"), true);
    assert.equal(aggregate.taskCount, 100);
    assert.deepEqual(aggregate.categoryCounts, aggregate.loaded.plan.category_mix);
    assert.deepEqual(aggregate.trackCounts, aggregate.loaded.plan.track_mix);
  }, 30_000);

  test("rejects textual path traversal before inspecting repository files", () => {
    const root = temporaryRoot();
    const stagedRoot = path.join(root, "staged");
    fs.mkdirSync(stagedRoot, { recursive: true });
    const contribution = validContribution();
    contribution.tasks[0].participant_files[0] = "../kernel.trit";
    fs.writeFileSync(path.join(stagedRoot, "V31-SHARD-001.json"), `${JSON.stringify(contribution, null, 2)}\n`);
    const checked = validateAuthorShardSet({ repositoryRoot, selectedShardId: "V31-SHARD-001", stagedRoot, checkpointRoot: path.join(root, "checkpoints") });
    assert.match(checked.errors.join("\n"), /safe distinct paths/);
  });

  test("creates review scaffolds only in the private root and binds all five task ids", () => {
    const root = temporaryRoot();
    const contributionRoot = path.join(root, "final-authoring", "contributions");
    fs.mkdirSync(contributionRoot, { recursive: true });
    const contribution = validContribution();
    fs.writeFileSync(path.join(contributionRoot, "V31-SHARD-001.json"), `${JSON.stringify(contribution, null, 2)}\n`);
    const command = spawnSync(process.execPath, [path.join(import.meta.dir, "prepare-intelligence-v31-review-shard.mjs"), "--shard=V31-SHARD-001", "--reviewer=fixture-reviewer", "--model=gpt-5.5", "--reasoning=xhigh"], { env: { ...process.env, TREATCODE_V31_FINAL_PRIVATE_ROOT: root }, encoding: "utf8", shell: false, windowsHide: true });
    assert.equal(command.status, 0, command.stderr);
    const reviewPath = path.join(root, "final-authoring", "reviews", "V31-SHARD-001.review-fixture-reviewer.json");
    assert.equal(fs.existsSync(reviewPath), true);
    const review = JSON.parse(fs.readFileSync(reviewPath, "utf8"));
    assert.equal(review.reviewed_shard_id, "V31-SHARD-001");
    assert.deepEqual(review.reviews.map((item: { task_id: string }) => item.task_id), contribution.tasks.map((item) => item.task_id));
    assert.equal(review.reviews.length, 5);
    assert.equal(fs.existsSync(path.join(repositoryRoot, "benchmarks", "intelligence-v3.1", "authoring", "reviews", "V31-SHARD-001.review-fixture-reviewer.json")), false);
  });

  test("resumes a partially copied twenty-shard private ingest without changing hashes", () => {
    const root = temporaryRoot();
    const fakeRepository = path.join(root, "repository");
    const fakeScripts = path.join(fakeRepository, "treatcode", "scripts");
    const fakeCorpus = path.join(fakeRepository, "benchmarks", "intelligence-v3.1");
    fs.mkdirSync(fakeScripts, { recursive: true });
    fs.mkdirSync(fakeCorpus, { recursive: true });
    for (const script of ["intelligence-v31-author-shards.mjs", "ingest-intelligence-v31-author-contributions.mjs"]) fs.copyFileSync(path.join(import.meta.dir, script), path.join(fakeScripts, script));
    for (const file of ["author-shard-plan.v3.1.json", "final-corpus-contract.v3.1.json"]) fs.copyFileSync(path.join(repositoryRoot, "benchmarks", "intelligence-v3.1", file), path.join(fakeCorpus, file));
    fs.copyFileSync(path.join(repositoryRoot, "TEST_MANIFEST.json"), path.join(fakeRepository, "TEST_MANIFEST.json"));
    const sourceFiles = ["kernel.trit", "apps/os_sdk.trit", "apps/libwidget.trit", "ternary_compiler_ir.h", "ternary_compiler_codegen.h", "SYSCALL_MANIFEST.json", "APP_MANIFEST.json", "IMAGE_FORMAT_MANIFEST.json", "ROADMAP_STATUS.json", "KNOWN_GAPS.md"];
    for (const relative of sourceFiles) {
      const target = path.join(fakeRepository, relative);
      fs.mkdirSync(path.dirname(target), { recursive: true });
      fs.copyFileSync(path.join(repositoryRoot, relative), target);
    }
    const stagedRoot = path.join(fakeCorpus, "authoring", "contributions");
    const checkpointRoot = path.join(root, "checkpoints");
    fs.mkdirSync(stagedRoot, { recursive: true });
    for (const assignment of loadAuthorShardPlan(fakeRepository).assignments) {
      fs.writeFileSync(path.join(stagedRoot, assignment.file), `${JSON.stringify(validContribution(assignment.shard_id), null, 2)}\n`);
      checkpointAuthorShard({ repositoryRoot: fakeRepository, shardId: assignment.shard_id, stagedRoot, checkpointRoot, evidenceRoot: path.join(root, "checkpoint-evidence") });
    }
    const privateBase = path.join(root, "private");
    const privateContributionRoot = path.join(privateBase, "final-authoring", "contributions");
    fs.mkdirSync(privateContributionRoot, { recursive: true });
    fs.copyFileSync(path.join(checkpointRoot, "V31-SHARD-001", "contribution.json"), path.join(privateContributionRoot, "V31-SHARD-001.json"));
    const environment = { ...process.env, TREATCODE_V31_AUTHOR_CHECKPOINT_ROOT: checkpointRoot, TREATCODE_V31_FINAL_PRIVATE_ROOT: privateBase };
    const first = spawnSync(process.execPath, [path.join(fakeScripts, "ingest-intelligence-v31-author-contributions.mjs")], { env: environment, encoding: "utf8", shell: false, windowsHide: true });
    assert.equal(first.status, 0, first.stderr);
    assert.match(first.stdout, /"newly_copied_shards": 19/);
    assert.match(first.stdout, /"resumed_shards": 1/);
    assert.equal(fs.readdirSync(privateContributionRoot).filter((file) => file.endsWith(".json")).length, 20);
    assert.equal(fs.readdirSync(stagedRoot).filter((file) => /^V31-SHARD-\d{3}\.json$/.test(file)).length, 0);
    const contract = JSON.parse(fs.readFileSync(path.join(fakeCorpus, "final-corpus-contract.v3.1.json"), "utf8"));
    assert.equal(contract.readiness.independently_authored, 100);
    const hashesBefore = fs.readdirSync(privateContributionRoot).sort().map((file) => sha256(fs.readFileSync(path.join(privateContributionRoot, file))));
    const second = spawnSync(process.execPath, [path.join(fakeScripts, "ingest-intelligence-v31-author-contributions.mjs")], { env: environment, encoding: "utf8", shell: false, windowsHide: true });
    assert.equal(second.status, 0, second.stderr);
    assert.match(second.stdout, /"resumed_shards": 20/);
    assert.deepEqual(fs.readdirSync(privateContributionRoot).sort().map((file) => sha256(fs.readFileSync(path.join(privateContributionRoot, file)))), hashesBefore);
  }, 30_000);
});
