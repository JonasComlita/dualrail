import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";

const argument = (name) => process.argv.find((value) => value.startsWith(`--${name}=`))?.slice(name.length + 3);
const attemptRoot = path.resolve(argument("attempt-root") || "");
const reason = argument("reason") || "deadline_exceeded";
if (!fs.existsSync(path.join(attemptRoot, "attempt.json")) || !/^[A-Za-z0-9._-]+$/.test(reason)) throw new Error("usage: --attempt-root=<root> [--reason=<safe-id>]");
if (fs.existsSync(path.join(attemptRoot, "sealed-grade.json"))) throw new Error("attempt already has a sealed grade");
const attempt = JSON.parse(fs.readFileSync(path.join(attemptRoot, "attempt.json"), "utf8"));
const discussion = fs.readFileSync(path.join(attemptRoot, "discussion.md"), "utf8");
const releasedMs = Date.parse(attempt.released_at);
const elapsedMs = Date.now() - releasedMs;
if (!Number.isFinite(releasedMs) || elapsedMs <= 1_200_000) throw new Error("attempt has not exceeded the subject deadline");
const result = {
  schema: "treatcode.intelligence.pilot-subject-grade.v3.1",
  official: false,
  disposable: true,
  task_id: attempt.task_id,
  model: attempt.model,
  reasoning_effort: attempt.reasoning_effort,
  released_at: attempt.released_at,
  graded_at: new Date().toISOString(),
  passed: false,
  completion_status: "deadline_exceeded_before_submission",
  correctness_observed: false,
  infrastructure_interruption: reason,
  subject_timing: { released_at: attempt.released_at, submitted_at: null, elapsed_ms: elapsedMs, budget_ms: 1_200_000, within_budget: false },
  sealed_report: null,
  discussion: { word_count: discussion.trim().split(/\s+/).filter(Boolean).length, sha256: crypto.createHash("sha256").update(discussion).digest("hex"), scored: false },
};
fs.writeFileSync(path.join(attemptRoot, "sealed-grade.json"), `${JSON.stringify(result, null, 2)}\n`, { flag: "wx", mode: 0o444 });
console.log(JSON.stringify(result));
