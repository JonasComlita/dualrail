import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { cp, mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { describe, test } from "bun:test";
import {
  INTELLIGENCE_V31_REPOSITORY_GRADER_SCHEMA,
  INTELLIGENCE_V31_REPOSITORY_PATCH_SCHEMA,
  INTELLIGENCE_V31_REPOSITORY_TASK_SCHEMA,
  createIntelligenceV31FrozenSuite,
  gradeIntelligenceV31RepositoryAttempt,
  verifyIntelligenceV31FrozenSuite,
  type IntelligenceV31CommandRegistry,
  type IntelligenceV31RepositoryGraderManifest,
  type IntelligenceV31RepositoryTaskManifest,
} from "../src/intelligenceV31Repository";
import { IntelligenceV31FinalCommandRegistry } from "../src/intelligenceV31FinalRegistry";

const repositoryRoot = path.resolve(import.meta.dir, "..", "..");
const sha256 = (value: string | Uint8Array) => createHash("sha256").update(value).digest("hex");

async function filesIn(root: string): Promise<Array<{ path: string; bytes: number; sha256: string }>> {
  const { readdir, lstat } = await import("node:fs/promises");
  const result: Array<{ path: string; bytes: number; sha256: string }> = [];
  const visit = async (directory: string): Promise<void> => {
    const entries = await readdir(directory, { withFileTypes: true });
    entries.sort((a, b) => a.name.localeCompare(b.name));
    for (const entry of entries) {
      const absolute = path.join(directory, entry.name);
      const metadata = await lstat(absolute);
      if (entry.isDirectory()) await visit(absolute);
      else {
        const content = await readFile(absolute);
        result.push({ path: path.relative(root, absolute).replace(/\\/g, "/"), bytes: metadata.size, sha256: sha256(content) });
      }
    }
  };
  await visit(root);
  return result;
}

function bundleHash(files: Array<{ path: string; bytes: number; sha256: string }>): string {
  return sha256(files.map((file) => `${file.path}\0${file.bytes}\0${file.sha256}\n`).join(""));
}

async function fixture(options: { phase?: IntelligenceV31RepositoryTaskManifest["phase"]; outputBytes?: number; floodOutput?: boolean } = {}): Promise<{
  root: string;
  subject: string;
  baseline: string;
  submission: string;
  grader: string;
  evidence: string;
  releasedHash: string;
  registry: IntelligenceV31CommandRegistry;
}> {
  const root = await mkdtemp(path.join(os.tmpdir(), "treatcode-v31-test-"));
  const subject = path.join(root, "subject");
  const baseline = path.join(subject, "baseline");
  const submission = path.join(subject, "submission");
  const grader = path.join(root, "privileged-grader");
  const evidence = path.join(root, "private-evidence");
  await Promise.all([mkdir(path.join(baseline, "src"), { recursive: true }), mkdir(grader, { recursive: true }), mkdir(evidence, { recursive: true })]);
  const contents = {
    "src/logic.trit": "fn repaired() -> t40 { return 0; }\n",
    "src/helper.trit": "fn helper(x: t40) -> t40 { return x; }\n",
    "src/bridge.ts": "export const ABI = 31;\n",
    "README.md": "Repair the repository without changing the public contract.\n",
  };
  for (const [relative, content] of Object.entries(contents)) {
    const absolute = path.join(baseline, relative);
    await mkdir(path.dirname(absolute), { recursive: true });
    await writeFile(absolute, content);
  }
  const manifest: IntelligenceV31RepositoryTaskManifest = {
    schema: INTELLIGENCE_V31_REPOSITORY_TASK_SCHEMA,
    version: "3.1",
    task_id: "TC-V31-TEST-001",
    title: "Repository executor contract fixture",
    phase: options.phase ?? "pilot",
    initial_signal: "The public repository check reports a behavioral mismatch after a state transition.",
    known_failing_command_id: "public.check",
    public_command_ids: ["public.check"],
    required_trit_change: true,
    files: Object.entries(contents).map(([relative, content]) => ({ path: relative, editable: relative !== "README.md", sha256: sha256(content) })),
    limits: { wall_clock_ms: 1_200_000, cpu_ms: 1_200_000, memory_mb: 256, process_count: 4, output_bytes: options.outputBytes ?? 65_536, maximum_changed_files: 3 },
  };
  const manifestText = `${JSON.stringify(manifest, null, 2)}\n`;
  await writeFile(path.join(baseline, "task.repository.v3.1.json"), manifestText);
  const releasedHash = bundleHash(await filesIn(baseline));
  const privateManifest: IntelligenceV31RepositoryGraderManifest = {
    schema: INTELLIGENCE_V31_REPOSITORY_GRADER_SCHEMA,
    version: "3.1",
    task_id: manifest.task_id,
    participant_manifest_sha256: sha256(manifestText),
    baseline_bundle_sha256: releasedHash,
    suites: [
      { id: "behavior", kind: "behavioral", command_id: "private.behavior", required: true },
      { id: "mutation", kind: "adversarial", command_id: "private.adversarial", required: true },
      { id: "budget", kind: "performance", command_id: "private.performance", required: true },
    ],
  };
  await writeFile(path.join(grader, "grader.repository.v3.1.json"), `${JSON.stringify(privateManifest, null, 2)}\n`);
  await cp(baseline, submission, { recursive: true });
  await writeFile(path.join(submission, "src", "logic.trit"), "fn repaired() -> t40 { return 1; }\n");
  const registry: IntelligenceV31CommandRegistry = {
    resolve(commandId, context) {
      const allowed = new Set(["public.check", "private.behavior", "private.adversarial", "private.performance"]);
      if (!allowed.has(commandId)) return undefined;
      const script = `const fs=require('fs'),p=require('path');const root=process.argv[1];const code=fs.readFileSync(p.join(root,'src','logic.trit'),'utf8');if(process.env.TREATCODE_NETWORK!=='disabled'||!code.includes('return 1;'))process.exit(7);${options.floodOutput ? "process.stdout.write('x'.repeat(4096));" : ""}`;
      return { executable: process.execPath, args: ["-e", script, context.workspace_root] };
    },
  };
  return { root, subject, baseline, submission, grader, evidence, releasedHash, registry };
}

describe("Intelligence v3.1 repository executor", () => {
  test("applies an allowlisted snapshot, requires a Trit change, seals aggregates, and refuses retries", async () => {
    const item = await fixture();
    try {
      const report = await gradeIntelligenceV31RepositoryAttempt({
        task_id: "TC-V31-TEST-001", attempt_id: "attempt-1", repository_root: repositoryRoot,
        subject_workspace_root: item.subject, baseline_root: item.baseline, submission_root: item.submission,
        grader_root: item.grader, evidence_root: item.evidence, released_bundle_hash: item.releasedHash, command_registry: item.registry,
      });
      assert.equal(report.passed, true);
      assert.equal(report.public_commands_passed, 1);
      assert.equal(report.required_suites_passed, 3);
      assert.equal(report.behavioral_passed, 1);
      assert.equal(report.adversarial_passed, 1);
      assert.equal(report.performance_passed, 1);
      assert.equal(report.changed_files, 1);
      assert.equal(report.trit_files_changed, 1);
      assert.equal(report.submission_kind, "snapshot");
      assert.ok(report.command_runtime_ms >= 0);
      assert.match(report.evidence_hash, /^[a-f0-9]{64}$/);
      const privateEvidence = JSON.parse(await readFile(path.join(item.evidence, "TC-V31-TEST-001-attempt-1.private.json"), "utf8"));
      assert.equal(privateEvidence.network_disabled, false);
      assert.equal(privateEvidence.network_isolation.enforcement, "development_proxy_only");
      assert.equal(privateEvidence.shell_disabled, true);
      await assert.rejects(() => gradeIntelligenceV31RepositoryAttempt({
        task_id: "TC-V31-TEST-001", attempt_id: "attempt-1", repository_root: repositoryRoot,
        subject_workspace_root: item.subject, baseline_root: item.baseline, submission_root: item.submission,
        grader_root: item.grader, evidence_root: item.evidence, released_bundle_hash: item.releasedHash, command_registry: item.registry,
      }), /EEXIST/);
    } finally { await rm(item.root, { recursive: true, force: true }); }
  }, 30_000);

  test("rejects nonallowlisted commands, path traversal, grader leakage, and submissions without Trit changes", async () => {
    const item = await fixture();
    try {
      const manifestPath = path.join(item.baseline, "task.repository.v3.1.json");
      const original = JSON.parse(await readFile(manifestPath, "utf8"));
      original.command = "powershell -Command Invoke-WebRequest https://example.com";
      await writeFile(manifestPath, `${JSON.stringify(original, null, 2)}\n`);
      await assert.rejects(() => gradeIntelligenceV31RepositoryAttempt({ task_id: "TC-V31-TEST-001", attempt_id: "raw-command", repository_root: repositoryRoot, subject_workspace_root: item.subject, baseline_root: item.baseline, submission_root: item.submission, grader_root: item.grader, evidence_root: item.evidence, released_bundle_hash: item.releasedHash, command_registry: item.registry }), /unsupported fields/);
      delete original.command;
      original.public_command_ids = ["raw;command"];
      await writeFile(manifestPath, `${JSON.stringify(original, null, 2)}\n`);
      await assert.rejects(() => gradeIntelligenceV31RepositoryAttempt({ task_id: "TC-V31-TEST-001", attempt_id: "bad-command", repository_root: repositoryRoot, subject_workspace_root: item.subject, baseline_root: item.baseline, submission_root: item.submission, grader_root: item.grader, evidence_root: item.evidence, released_bundle_hash: item.releasedHash, command_registry: item.registry }), /command ids are invalid|different participant manifest|bundle hash/);

      original.public_command_ids = ["public.check"];
      original.files[0].path = "../escape.trit";
      await writeFile(manifestPath, `${JSON.stringify(original, null, 2)}\n`);
      await assert.rejects(() => gradeIntelligenceV31RepositoryAttempt({ task_id: "TC-V31-TEST-001", attempt_id: "bad-path", repository_root: repositoryRoot, subject_workspace_root: item.subject, baseline_root: item.baseline, submission_root: item.submission, grader_root: item.grader, evidence_root: item.evidence, released_bundle_hash: item.releasedHash, command_registry: item.registry }), /unsafe|different participant manifest|bundle hash/);
    } finally { await rm(item.root, { recursive: true, force: true }); }

    const noTrit = await fixture();
    try {
      await cp(noTrit.baseline, noTrit.submission, { recursive: true, force: true });
      await writeFile(path.join(noTrit.submission, "src", "bridge.ts"), "export const ABI = 32;\n");
      await assert.rejects(() => gradeIntelligenceV31RepositoryAttempt({ task_id: "TC-V31-TEST-001", attempt_id: "no-trit", repository_root: repositoryRoot, subject_workspace_root: noTrit.subject, baseline_root: noTrit.baseline, submission_root: noTrit.submission, grader_root: noTrit.grader, evidence_root: noTrit.evidence, released_bundle_hash: noTrit.releasedHash, command_registry: noTrit.registry }), /must change at least one editable Trit file/);
      await assert.rejects(() => gradeIntelligenceV31RepositoryAttempt({ task_id: "TC-V31-TEST-001", attempt_id: "grader-leak", repository_root: repositoryRoot, subject_workspace_root: noTrit.subject, baseline_root: noTrit.baseline, submission_root: noTrit.submission, grader_root: noTrit.baseline, evidence_root: noTrit.evidence, released_bundle_hash: noTrit.releasedHash, command_registry: noTrit.registry }), /private grader must be outside/);
    } finally { await rm(noTrit.root, { recursive: true, force: true }); }
  }, 30_000);

  test("applies a hash-bound replacement patch and rejects traversal", async () => {
    const item = await fixture();
    try {
      const original = "fn repaired() -> t40 { return 0; }\n";
      const patchPath = path.join(item.subject, "attempt.patch.v3.1.json");
      await writeFile(patchPath, `${JSON.stringify({
        schema: INTELLIGENCE_V31_REPOSITORY_PATCH_SCHEMA,
        version: "3.1",
        task_id: "TC-V31-TEST-001",
        base_bundle_sha256: item.releasedHash,
        files: [{ path: "src/logic.trit", base_sha256: sha256(original), replacement_utf8: "fn repaired() -> t40 { return 1; }\n" }],
      }, null, 2)}\n`);
      const report = await gradeIntelligenceV31RepositoryAttempt({
        task_id: "TC-V31-TEST-001", attempt_id: "patch-1", repository_root: repositoryRoot,
        subject_workspace_root: item.subject, baseline_root: item.baseline, submission_patch_path: patchPath,
        grader_root: item.grader, evidence_root: item.evidence, released_bundle_hash: item.releasedHash, command_registry: item.registry,
      });
      assert.equal(report.passed, true);
      assert.equal(report.submission_kind, "replacement_patch");

      await writeFile(patchPath, `${JSON.stringify({
        schema: INTELLIGENCE_V31_REPOSITORY_PATCH_SCHEMA,
        version: "3.1",
        task_id: "TC-V31-TEST-001",
        base_bundle_sha256: item.releasedHash,
        files: [{ path: "../escape.trit", base_sha256: sha256(original), replacement_utf8: "malicious" }],
      }, null, 2)}\n`);
      await assert.rejects(() => gradeIntelligenceV31RepositoryAttempt({
        task_id: "TC-V31-TEST-001", attempt_id: "patch-traversal", repository_root: repositoryRoot,
        subject_workspace_root: item.subject, baseline_root: item.baseline, submission_patch_path: patchPath,
        grader_root: item.grader, evidence_root: item.evidence, released_bundle_hash: item.releasedHash, command_registry: item.registry,
      }), /unsafe segment/);
    } finally { await rm(item.root, { recursive: true, force: true }); }
  }, 30_000);

  test("detects frozen-suite mutation", () => {
    const hash = (value: string) => value.repeat(64);
    const tasks = Array.from({ length: 100 }, (_, index) => ({ task_id: `TC-V31-FINAL-${String(index + 1).padStart(3, "0")}`, participant_bundle_hash: hash("a"), grader_bundle_hash: hash("b") }));
    const frozen = createIntelligenceV31FrozenSuite({ frozen_at: "2026-08-27T12:00:00Z", protocol_sha256: hash("c"), tasks });
    assert.deepEqual(verifyIntelligenceV31FrozenSuite(frozen), []);
    const mutated = { ...frozen, tasks: frozen.tasks.map((task, index) => index === 0 ? { ...task, participant_bundle_hash: hash("d") } : task) };
    assert.ok(verifyIntelligenceV31FrozenSuite(mutated).includes("frozen suite hash mismatch"));
  });

  test("final command registry exposes only fixed public and hidden command ids", () => {
    const privateRoot = path.join(repositoryRoot, "private-v31-test");
    const registry = new IntelligenceV31FinalCommandRegistry(repositoryRoot, privateRoot);
    const context = { task_id: "TC-V31-FINAL-001", workspace_root: path.join(repositoryRoot, "subject-v31-test"), visibility: "public" as const };
    const publicCommand = registry.resolve("final.public", context);
    assert.equal(publicCommand?.executable, process.execPath);
    assert.ok(publicCommand?.args.includes("--suite=public"));
    assert.equal(registry.resolve("final.private.behavioral", context), undefined);
    assert.equal(registry.resolve("final.public;Remove-Item", context), undefined);
    assert.equal(registry.resolve("final.public", { ...context, task_id: "TC-V31-PILOT-001" }), undefined);
    const hidden = registry.resolve("final.private.adversarial", { ...context, visibility: "private" });
    assert.ok(hidden?.args.includes("--suite=adversarial"));
  });

  test("enforces output limits and requires verified network, resource, and append-only evidence isolation for frozen grading", async () => {
    const limited = await fixture({ outputBytes: 64, floodOutput: true });
    try {
      const report = await gradeIntelligenceV31RepositoryAttempt({ task_id: "TC-V31-TEST-001", attempt_id: "output-limit", repository_root: repositoryRoot, subject_workspace_root: limited.subject, baseline_root: limited.baseline, submission_root: limited.submission, grader_root: limited.grader, evidence_root: limited.evidence, released_bundle_hash: limited.releasedHash, command_registry: limited.registry });
      assert.equal(report.passed, false);
      assert.equal(report.public_commands_passed, 0);
    } finally { await rm(limited.root, { recursive: true, force: true }); }

    const frozen = await fixture({ phase: "frozen" });
    try {
      await assert.rejects(() => gradeIntelligenceV31RepositoryAttempt({ task_id: "TC-V31-TEST-001", attempt_id: "no-network-proof", repository_root: repositoryRoot, subject_workspace_root: frozen.subject, baseline_root: frozen.baseline, submission_root: frozen.submission, grader_root: frozen.grader, evidence_root: frozen.evidence, released_bundle_hash: frozen.releasedHash, command_registry: frozen.registry }), /requires verified OS or container network isolation/);
      await assert.rejects(() => gradeIntelligenceV31RepositoryAttempt({ task_id: "TC-V31-TEST-001", attempt_id: "network-only-proof", repository_root: repositoryRoot, subject_workspace_root: frozen.subject, baseline_root: frozen.baseline, submission_root: frozen.submission, grader_root: frozen.grader, evidence_root: frozen.evidence, released_bundle_hash: frozen.releasedHash, command_registry: frozen.registry, network_isolation: { enforcement: "verified_os_or_container", provider: "test-container", evidence_sha256: "f".repeat(64) } }), /requires verified per-process CPU, memory, and process accounting/);
      await assert.rejects(() => gradeIntelligenceV31RepositoryAttempt({ task_id: "TC-V31-TEST-001", attempt_id: "network-resource-proof", repository_root: repositoryRoot, subject_workspace_root: frozen.subject, baseline_root: frozen.baseline, submission_root: frozen.submission, grader_root: frozen.grader, evidence_root: frozen.evidence, released_bundle_hash: frozen.releasedHash, command_registry: frozen.registry, network_isolation: { enforcement: "verified_os_or_container", provider: "test-container", evidence_sha256: "f".repeat(64) }, resource_isolation: { enforcement: "verified_os_or_container", provider: "test-job-object", evidence_sha256: "e".repeat(64) } }), /requires a verified append-only evidence store/);
      const verified = await gradeIntelligenceV31RepositoryAttempt({ task_id: "TC-V31-TEST-001", attempt_id: "all-isolation-proofs", repository_root: repositoryRoot, subject_workspace_root: frozen.subject, baseline_root: frozen.baseline, submission_root: frozen.submission, grader_root: frozen.grader, evidence_root: frozen.evidence, released_bundle_hash: frozen.releasedHash, command_registry: frozen.registry, network_isolation: { enforcement: "verified_os_or_container", provider: "test-container", evidence_sha256: "f".repeat(64) }, resource_isolation: { enforcement: "verified_os_or_container", provider: "test-job-object", evidence_sha256: "e".repeat(64) }, evidence_store: { enforcement: "verified_append_only", provider: "test-worm-store", evidence_sha256: "d".repeat(64) } });
      assert.equal(verified.network_isolation_verified, true);
      assert.equal(verified.resource_limits_verified, true);
      assert.equal(verified.append_only_evidence_verified, true);
      assert.equal(verified.passed, true);
    } finally { await rm(frozen.root, { recursive: true, force: true }); }
  }, 30_000);

  test("an externally frozen calibration bundle cannot bypass verified infrastructure", async () => {
    const item = await fixture({ phase: "calibration" });
    try {
      await assert.rejects(() => gradeIntelligenceV31RepositoryAttempt({ task_id: "TC-V31-TEST-001", attempt_id: "externally-frozen", repository_root: repositoryRoot, subject_workspace_root: item.subject, baseline_root: item.baseline, submission_root: item.submission, grader_root: item.grader, evidence_root: item.evidence, released_bundle_hash: item.releasedHash, command_registry: item.registry, require_verified_infrastructure: true }), /requires verified OS or container network isolation/);
    } finally { await rm(item.root, { recursive: true, force: true }); }
  });
});
