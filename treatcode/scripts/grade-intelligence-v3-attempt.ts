import { createHash } from "node:crypto";
import { mkdir, readFile, realpath, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { IntelligenceV3TaskExecutor } from "../src/intelligenceV3Executor";

const argument = (name: string): string | undefined => process.argv.find((value) => value.startsWith(`--${name}=`))?.slice(name.length + 3);
const taskId = argument("task");
const participantArgument = argument("participant");
if (!taskId || !/^TC-V3-\d{3}$/.test(taskId) || !participantArgument) {
  throw new Error("usage: bun run scripts/grade-intelligence-v3-attempt.ts --task=TC-V3-NNN --participant=<participant-root>");
}

const repositoryRoot = path.resolve(import.meta.dir, "..", "..");
const participantRoot = await realpath(path.resolve(participantArgument));
const attemptRoot = path.dirname(participantRoot);
const subjectWorkspaceRoot = await realpath(attemptRoot);
const privateBase = process.env.TREATCODE_V3_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v3-private");
const graderRoot = path.join(privateBase, taskId);
const resultPath = path.join(attemptRoot, "sealed-grade.json");
const lockPath = path.join(attemptRoot, "sealed-grade.lock.json");

await writeFile(lockPath, `${JSON.stringify({ schema: "treatcode.intelligence.v3-grade-lock.v1", task_id: taskId, locked_at: new Date().toISOString(), rule: "no retry after lock creation" }, null, 2)}\n`, { encoding: "utf8", flag: "wx" });

const executor = await IntelligenceV3TaskExecutor.create({
  repository_root: repositoryRoot,
  subject_workspace_root: subjectWorkspaceRoot,
  participant_root: participantRoot,
  grader_root: graderRoot,
  artifact_root: path.join(attemptRoot, "sealed-runner-artifacts"),
  source_commit: "intelligence-v3-two-task-development-comparison",
});
const publicReport = await executor.runPublic();
const hiddenReport = await executor.submitHidden();
const [attemptText, discussion] = await Promise.all([
  readFile(path.join(attemptRoot, "attempt.json"), "utf8"),
  readFile(path.join(attemptRoot, "discussion.md"), "utf8"),
]);
const attempt = JSON.parse(attemptText);
if (attempt.task_id !== taskId) throw new Error("attempt metadata belongs to a different task");
const words = discussion.trim().split(/\s+/).filter(Boolean);
const result = {
  schema: "treatcode.intelligence.v3-development-grade.v1",
  official: false,
  scope: "single executable development draft",
  task_id: taskId,
  model: attempt.model,
  reasoning_effort: attempt.reasoning_effort,
  subject_attempt: attempt,
  evaluator_public: {
    passed: publicReport.passed,
    passed_cases: publicReport.cases.filter((item) => item.passed).length,
    total_cases: publicReport.cases.length,
  },
  sealed_hidden: hiddenReport,
  discussion: {
    word_count: words.length,
    sha256: createHash("sha256").update(discussion).digest("hex"),
  },
  graded_at: new Date().toISOString(),
};
await mkdir(path.dirname(resultPath), { recursive: true });
await writeFile(resultPath, `${JSON.stringify(result, null, 2)}\n`, { encoding: "utf8", flag: "wx" });
console.log(JSON.stringify({ result_path: resultPath, ...result }, null, 2));
