import assert from "node:assert/strict";
import crypto from "node:crypto";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { describe, test } from "bun:test";

const repositoryRoot = path.resolve(import.meta.dir, "..", "..");

describe("Intelligence v3.1 disposed executable pilot", () => {
  test("retains hash-bound one-shot grades after exposed participant and grader packages are discarded", async () => {
    const taskId = "TC-V31-PILOT-001";
    const attemptRoot = path.join(repositoryRoot, "build", "intelligence-v31-pilot-runs", "pilot-20260827-a", taskId, "gpt-5.6-luna-max");
    const gradeText = await readFile(path.join(attemptRoot, "sealed-grade.json"), "utf8");
    const grade = JSON.parse(gradeText);
    const disposal = JSON.parse(await readFile(path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v31-pilot-disposal-manifest.json"), "utf8"));
    const task = disposal.tasks.find((item: Record<string, any>) => item.task_id === taskId);
    const subject = task.subject_grades.find((item: Record<string, any>) => item.configuration === "gpt-5.6-luna-max");
    assert.equal(disposal.tasks.length, 30);
    assert.equal(grade.passed, true);
    assert.equal(grade.sealed_report.public_commands_passed, 1);
    assert.equal(grade.sealed_report.behavioral_passed, 1);
    assert.equal(grade.sealed_report.adversarial_passed, 1);
    assert.equal(grade.sealed_report.performance_passed, 1);
    assert.equal(subject.grade_sha256, crypto.createHash("sha256").update(gradeText).digest("hex"));
    assert.equal(subject.evidence_hash, grade.sealed_report.evidence_hash);
    assert.equal(task.participant_bundle_hash, grade.sealed_report.participant_bundle_hash);
    assert.equal(task.grader_bundle_hash, grade.sealed_report.grader_bundle_hash);
  }, 30_000);
});
