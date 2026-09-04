import { createHash } from "node:crypto";
import { mkdir, readFile, stat, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { gradeIntelligenceV31RepositoryAttempt } from "../src/intelligenceV31Repository";
import { IntelligenceV31PilotCommandRegistry } from "../src/intelligenceV31PilotRegistry";

const argument = (name: string): string | undefined => process.argv.find((value) => value.startsWith(`--${name}=`))?.slice(name.length + 3);
const attemptArgument = argument("attempt-root");
if (!attemptArgument) throw new Error("usage: --attempt-root=<fresh pilot attempt root>");
const repositoryRoot = path.resolve(import.meta.dir, "..", "..");
const attemptRoot = path.resolve(attemptArgument);
const attempt = JSON.parse(await readFile(path.join(attemptRoot, "attempt.json"), "utf8"));
if (!/^TC-V31-PILOT-\d{3}$/.test(attempt.task_id) || !/^[A-Za-z0-9._-]+$/.test(attempt.model) || !/^[A-Za-z0-9._-]+$/.test(attempt.reasoning_effort)) throw new Error("attempt metadata is invalid");
const privateRoot = process.env.TREATCODE_V31_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v31-private", "pilot");
const graderRoot = path.join(privateRoot, attempt.task_id);
const graderManifest = JSON.parse(await readFile(path.join(graderRoot, "grader.repository.v3.1.json"), "utf8"));
const evidenceRoot = path.join(repositoryRoot, "build", "intelligence-v31-private-evidence", "pilot", attempt.run_id, attempt.task_id, `${attempt.model}-${attempt.reasoning_effort}`);
await mkdir(evidenceRoot, { recursive: true });
const report = await gradeIntelligenceV31RepositoryAttempt({
  task_id: attempt.task_id,
  attempt_id: `${attempt.model}-${attempt.reasoning_effort}`,
  repository_root: repositoryRoot,
  subject_workspace_root: attemptRoot,
  baseline_root: path.join(attemptRoot, "baseline"),
  submission_root: path.join(attemptRoot, "submission"),
  grader_root: graderRoot,
  evidence_root: evidenceRoot,
  released_bundle_hash: graderManifest.baseline_bundle_sha256,
  command_registry: new IntelligenceV31PilotCommandRegistry(repositoryRoot, privateRoot),
});
const discussion = await readFile(path.join(attemptRoot, "discussion.md"), "utf8");
const discussionStat = await stat(path.join(attemptRoot, "discussion.md"));
const releasedMs = Date.parse(attempt.released_at);
const submittedMs = discussionStat.mtimeMs;
const subjectElapsedMs = Math.max(0, Math.round(submittedMs - releasedMs));
const withinSubjectBudget = Number.isFinite(releasedMs) && discussion.trim().length > 0 && subjectElapsedMs <= 1_200_000;
const result = {
  schema: "treatcode.intelligence.pilot-subject-grade.v3.1",
  official: false,
  disposable: true,
  task_id: attempt.task_id,
  model: attempt.model,
  reasoning_effort: attempt.reasoning_effort,
  released_at: attempt.released_at,
  graded_at: new Date().toISOString(),
  passed: report.passed && withinSubjectBudget,
  completion_status: withinSubjectBudget ? "completed" : "subject_deadline_or_completion_evidence_failed",
  subject_timing: { released_at: attempt.released_at, submitted_at: discussionStat.mtime.toISOString(), elapsed_ms: subjectElapsedMs, budget_ms: 1_200_000, within_budget: withinSubjectBudget },
  sealed_report: report,
  discussion: { word_count: discussion.trim().split(/\s+/).filter(Boolean).length, sha256: createHash("sha256").update(discussion).digest("hex"), scored: false },
};
await writeFile(path.join(attemptRoot, "sealed-grade.json"), `${JSON.stringify(result, null, 2)}\n`, { encoding: "utf8", flag: "wx", mode: 0o444 });
console.log(JSON.stringify(result));
