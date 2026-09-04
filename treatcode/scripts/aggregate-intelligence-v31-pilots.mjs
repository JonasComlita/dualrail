import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const argument = (name) => process.argv.find((value) => value.startsWith(`--${name}=`))?.slice(name.length + 3);
const runId = argument("run");
if (!/^[A-Za-z0-9._-]+$/.test(runId || "")) throw new Error("usage: --run=<pilot-run-id>");
const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const pilot = JSON.parse(fs.readFileSync(path.join(repoRoot, "benchmarks", "intelligence-v3.1", "pilot-corpus.v3.1.json"), "utf8"));
const runRoot = path.join(repoRoot, "build", "intelligence-v31-pilot-runs", runId);
const matrix = [];
const errors = [];
for (const task of pilot.tasks) {
  const row = { task_id: task.task_id, category: task.category, luna_max: null, sol_high: null };
  for (const subject of [
    { key: "luna_max", directory: "gpt-5.6-luna-max", model: "gpt-5.6-luna", effort: "max" },
    { key: "sol_high", directory: "gpt-5.6-sol-high", model: "gpt-5.6-sol", effort: "high" },
  ]) {
    const gradePath = path.join(runRoot, task.task_id, subject.directory, "sealed-grade.json");
    if (!fs.existsSync(gradePath)) continue;
    try {
      const grade = JSON.parse(fs.readFileSync(gradePath, "utf8"));
      if (grade.task_id !== task.task_id || grade.model !== subject.model || grade.reasoning_effort !== subject.effort || grade.official !== false || grade.disposable !== true) throw new Error("grade metadata mismatch");
      const timing = grade.subject_timing || (() => {
        const discussionPath = path.join(runRoot, task.task_id, subject.directory, "discussion.md");
        const submittedMs = fs.statSync(discussionPath).mtimeMs;
        const releasedMs = Date.parse(grade.released_at);
        const elapsed = Math.max(0, Math.round(submittedMs - releasedMs));
        return { released_at: grade.released_at, submitted_at: new Date(submittedMs).toISOString(), elapsed_ms: elapsed, budget_ms: 1_200_000, within_budget: grade.discussion.word_count > 0 && elapsed <= 1_200_000 };
      })();
      row[subject.key] = { passed: Boolean(grade.passed && timing.within_budget), completion_status: grade.completion_status || "completed", correctness_observed: grade.correctness_observed !== false, infrastructure_interruption: grade.infrastructure_interruption || null, changed_files: grade.sealed_report?.changed_files ?? 0, trit_files_changed: grade.sealed_report?.trit_files_changed ?? 0, wall_clock_ms: grade.sealed_report?.wall_clock_ms ?? null, subject_elapsed_ms: timing.elapsed_ms, discussion_words: grade.discussion.word_count, evidence_hash: grade.sealed_report?.evidence_hash ?? null };
    } catch (error) { errors.push(`${task.task_id}/${subject.key}: ${error.message}`); }
  }
  matrix.push(row);
}
const completeRows = matrix.filter((row) => row.luna_max && row.sol_high);
const observedPairs = completeRows.filter((row) => row.luna_max.correctness_observed && row.sol_high.correctness_observed);
const lunaObserved = matrix.map((row) => row.luna_max).filter((result) => result?.correctness_observed);
const solObserved = matrix.map((row) => row.sol_high).filter((result) => result?.correctness_observed);
const lunaPasses = lunaObserved.filter((result) => result.passed).length;
const solPasses = solObserved.filter((result) => result.passed).length;
const pairedLunaPasses = observedPairs.filter((row) => row.luna_max.passed).length;
const pairedSolPasses = observedPairs.filter((row) => row.sol_high.passed).length;
const timingSummary = (results) => {
  const values = results.map((result) => result.subject_elapsed_ms).filter(Number.isFinite).sort((a, b) => a - b);
  if (!values.length) return null;
  return {
    observed: values.length,
    mean_ms: Math.round(values.reduce((sum, value) => sum + value, 0) / values.length),
    median_ms: values[Math.floor(values.length / 2)],
    p90_ms: values[Math.min(values.length - 1, Math.floor(values.length * 0.9))],
  };
};
const classifications = {
  ceiling_pass_pass: observedPairs.filter((row) => row.luna_max.passed && row.sol_high.passed).length,
  floor_fail_fail: observedPairs.filter((row) => !row.luna_max.passed && !row.sol_high.passed).length,
  diagnostic_sol_only: observedPairs.filter((row) => !row.luna_max.passed && row.sol_high.passed).length,
  diagnostic_luna_only: observedPairs.filter((row) => row.luna_max.passed && !row.sol_high.passed).length,
  excluded_infrastructure_interruption: completeRows.length - observedPairs.length,
};
const complete = completeRows.length === 30 && errors.length === 0;
const patterns = [];
if (complete && observedPairs.length && classifications.ceiling_pass_pass / observedPairs.length > 0.5) patterns.push("Explicit local equations plus four public witnesses are ceiling-prone; final tasks must require discovery of implicit cross-module invariants rather than direct transcription.");
if (complete && observedPairs.length && classifications.floor_fail_fail / observedPairs.length > 0.15) patterns.push("Shared failures should be split by failure cause and redesigned to avoid timeout- or syntax-driven floors.");
if (complete && classifications.diagnostic_sol_only + classifications.diagnostic_luna_only < 3) patterns.push("This pilot family has weak subject discrimination; do not copy its solution structure into final candidates.");
if (complete && observedPairs.length && classifications.ceiling_pass_pass === observedPairs.length) patterns.push("Uniform task layout and an explicitly stated ABI constant made repository exploration shallow; final candidates need heterogeneous layouts, plausible distractors, and incomplete public signals.");
const report = {
  schema: "trit.treatcode_intelligence_v31_pilot_comparison.v1",
  official: false,
  disposable: true,
  run_id: runId,
  status: complete ? "complete" : "incomplete",
  expected_attempts: 60,
  terminal_attempts: matrix.reduce((sum, row) => sum + Number(Boolean(row.luna_max)) + Number(Boolean(row.sol_high)), 0),
  completed_attempts: matrix.reduce((sum, row) => sum + Number(row.luna_max?.completion_status === "completed") + Number(row.sol_high?.completion_status === "completed"), 0),
  correctness_observed_attempts: lunaObserved.length + solObserved.length,
  paired_tasks_complete: completeRows.length,
  paired_correctness_observed: observedPairs.length,
  scores: {
    luna_max: lunaObserved.length ? { percent: Number(((lunaPasses / lunaObserved.length) * 100).toFixed(2)), passed: lunaPasses, observed: lunaObserved.length } : null,
    sol_high: solObserved.length ? { percent: Number(((solPasses / solObserved.length) * 100).toFixed(2)), passed: solPasses, observed: solObserved.length } : null,
    paired_sol_lead_tasks: pairedSolPasses - pairedLunaPasses,
  },
  subject_completion_timing: {
    luna_max: timingSummary(lunaObserved),
    sol_high: timingSummary(solObserved),
    mean_sol_speedup_percent: lunaObserved.length && solObserved.length
      ? Number(((1 - (timingSummary(solObserved).mean_ms / timingSummary(lunaObserved).mean_ms)) * 100).toFixed(2))
      : null,
    inferential_status: "descriptive_only_nonrandomized_agent_wall_time",
  },
  classifications,
  infrastructure_interrupted_attempts: completeRows.reduce((sum, row) => sum + Number(Boolean(row.luna_max.infrastructure_interruption)) + Number(Boolean(row.sol_high.infrastructure_interruption)), 0),
  task_matrix: matrix,
  derived_design_patterns: patterns,
  errors,
};
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P14");
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "intelligence-v31-pilot-comparison.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(JSON.stringify({ status: report.status, completed_attempts: report.completed_attempts, paired_tasks_complete: report.paired_tasks_complete, scores: report.scores, classifications: report.classifications, errors: report.errors }, null, 2));
process.exitCode = errors.length ? 1 : 0;
