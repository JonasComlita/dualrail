import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(scriptDir, "..");
const evaluationRoot = path.resolve(repoRoot, "..", "build", "treatcode-model-evals");
const lunaPath = path.join(evaluationRoot, "v2-luna-max", "evidence.json");
const solPath = path.join(evaluationRoot, "v2-sol-high", "evidence.json");
const outputDir = path.join(evaluationRoot, "v2-comparison");
const outputPath = path.join(outputDir, "assessment.json");
const lunaCommunityPath = path.join(evaluationRoot, "v2-luna-max", "community-state.json");
const solCommunityPath = path.join(evaluationRoot, "v2-sol-high", "state", "community.json");

const sha256 = (value) => createHash("sha256").update(value).digest("hex");
const round = (value, digits = 3) => Number(value.toFixed(digits));

const [lunaRaw, solRaw, lunaCommunityRaw, solCommunityRaw] = await Promise.all([
  readFile(lunaPath, "utf8"),
  readFile(solPath, "utf8"),
  readFile(lunaCommunityPath, "utf8"),
  readFile(solCommunityPath, "utf8"),
]);
const luna = JSON.parse(lunaRaw);
const sol = JSON.parse(solRaw);
const lunaCommunity = JSON.parse(lunaCommunityRaw);
const solCommunity = JSON.parse(solCommunityRaw);

const lunaAggregate = luna.aggregate.aggregate;
const solAggregate = sol.tc_swe_001.aggregate;
const lunaT001Results = luna.t001.submission_response.results;
const solT001Results = sol.t001.submission_response.results;
const lunaDiscussionId = luna.discussion.published_response.discussion.id;

assert.equal(luna.evaluation.model, "gpt-5.6-luna");
assert.equal(luna.evaluation.reasoning_effort, "max");
assert.equal(sol.model_config.model, "gpt-5.6-sol");
assert.equal(sol.model_config.reasoning_effort, "high");
assert.equal(lunaAggregate.task_id, "TC-SWE-001");
assert.equal(sol.tc_swe_001.task_id, "TC-SWE-001");
assert.equal(lunaAggregate.benchmark_version, 2);
assert.equal(solAggregate.benchmark_version, 2);
assert.equal(lunaAggregate.score, 100);
assert.equal(solAggregate.score, 100);
assert.equal(lunaAggregate.passed_trials, 4);
assert.equal(solAggregate.passed_trials, 4);
assert.equal(lunaAggregate.trial_count, 4);
assert.equal(solAggregate.trial_count, 4);
assert.equal(lunaAggregate.published, false);
assert.equal(solAggregate.published, false);
assert.equal(luna.trials.length, 4);
assert.ok(luna.trials.every((trial) => trial.status === "hidden_submitted"));
assert.equal(sol.tc_swe_001.sealed_trial_receipts.length, 4);
assert.ok(sol.tc_swe_001.sealed_trial_receipts.every((trial) => trial.sealed === true));
assert.match(luna.solution.solution_id, /^tc:solution:/);
assert.match(sol.tc_swe_001.solution_id, /^tc:solution:/);
assert.match(lunaDiscussionId, /^tc:discussion:/);
assert.match(sol.tc_swe_001.discussion_id, /^tc:discussion:/);
assert.ok(
  lunaCommunity.solution_revisions.some(
    (revision) => revision.solution_id === luna.solution.solution_id,
  ),
);
assert.ok(
  solCommunity.solution_revisions.some(
    (revision) => revision.solution_id === sol.tc_swe_001.solution_id,
  ),
);
assert.ok(lunaCommunity.discussions.some((discussion) => discussion.id === lunaDiscussionId));
assert.ok(
  solCommunity.discussions.some(
    (discussion) => discussion.id === sol.tc_swe_001.discussion_id,
  ),
);
assert.ok(
  lunaCommunity.submissions.some(
    (submission) => submission.id === luna.t001.submission_response.submission.id,
  ),
);
assert.ok(
  solCommunity.submissions.some(
    (submission) => submission.id === sol.t001.submission_response.submission.id,
  ),
);
assert.notEqual(luna.account.handle, sol.participant.handle);
assert.notEqual(luna.evaluation.dedicated_port, sol.environment.port);
assert.ok(luna.blind_access_declaration.includes("did not read"));
assert.ok(sol.blind_access_declaration.includes("did not read"));
assert.equal(luna.server_stop.verified_stopped, true);
assert.deepEqual(luna.server_stop.listeners_on_port_4331_after_stop, []);
assert.equal(luna.t001.accepted, true);
assert.equal(sol.t001.submission_response.submission.outcome, "accepted");
assert.equal(lunaT001Results.length, 5);
assert.equal(solT001Results.length, 5);
assert.ok(lunaT001Results.every((result) => result.passed));
assert.ok(solT001Results.every((result) => result.passed));
assert.deepEqual(
  lunaT001Results.map((result) => result.cycles),
  solT001Results.map((result) => result.cycles),
);
assert.equal(luna.t001.submission_response.submission.cycles, 34);
assert.equal(sol.t001.submission_response.submission.cycles, 34);

const lunaBenchmarkMs = luna.timings.phases.four_trial_public_and_sealed_submissions.wall_ms;
const solBenchmarkMs =
  sol.timings.clean_isolated_upload_and_public_tests.wall_ms +
  sol.timings.clean_isolated_sealed_trials.wall_ms;
const lunaT001SubmissionMs = luna.timings.phases.t001_submission.wall_ms;
const solT001SubmissionMs = sol.t001.submission_response.wall_ms;

const assessment = {
  schema: "treatcode.model-evaluation.comparison.v1",
  generated_at: new Date().toISOString(),
  inputs: {
    luna: {
      path: lunaPath,
      evidence_sha256: sha256(lunaRaw),
      model: luna.evaluation.model,
      reasoning_effort: luna.evaluation.reasoning_effort,
      handle: luna.account.handle,
      run_id: lunaAggregate.run_id,
      solution_id: luna.solution.solution_id,
      discussion_id: lunaDiscussionId,
    },
    sol: {
      path: solPath,
      evidence_sha256: sha256(solRaw),
      model: sol.model_config.model,
      reasoning_effort: sol.model_config.reasoning_effort,
      handle: sol.participant.handle,
      run_id: sol.tc_swe_001.run_id,
      solution_id: sol.tc_swe_001.solution_id,
      discussion_id: sol.tc_swe_001.discussion_id,
    },
  },
  integrity_checks: {
    status: "passed",
    isolated_accounts: true,
    isolated_ports: true,
    blind_access_declarations_present: true,
    saved_solutions_present: true,
    published_discussions_present: true,
    persisted_solutions_discussions_and_submissions_verified: true,
    four_sealed_trials_each: true,
    luna_server_verified_stopped: true,
  },
  correctness: {
    tc_swe_001_v2: {
      luna: { score: 100, passed_trials: 4, trial_count: 4 },
      sol: { score: 100, passed_trials: 4, trial_count: 4 },
      result: "tie_at_score_ceiling",
      note: "Neither model can score above 100 on this task_trial_reliability scale.",
    },
    t001: {
      luna: { outcome: "accepted", cycles: 34, case_cycles: [35, 34, 34, 35, 34] },
      sol: { outcome: "accepted", cycles: 34, case_cycles: [35, 34, 34, 35, 34] },
      result: "tie",
      note: "The generated code shape and measured cycle results are equivalent; no technically better measured result was demonstrated.",
    },
  },
  timing: {
    comparable_api_phases: {
      tc_swe_001_public_and_sealed_ms: {
        luna: lunaBenchmarkMs,
        sol: round(solBenchmarkMs),
        nominal_difference_ms: round(solBenchmarkMs - lunaBenchmarkMs),
        nominal_luna_advantage_percent_of_sol: round(
          ((solBenchmarkMs - lunaBenchmarkMs) / solBenchmarkMs) * 100,
          2,
        ),
        assessment: "luna_nominally_faster_but_not_statistically_established",
      },
      t001_submission_ms: {
        luna: lunaT001SubmissionMs,
        sol: solT001SubmissionMs,
        nominal_difference_ms: round(solT001SubmissionMs - lunaT001SubmissionMs),
        assessment: "effectively_tied",
      },
    },
    authoring_time_assessment: "not_comparable",
    authoring_time_reason:
      "Luna's recorded wall clock begins at its scripted API journey after solution authoring, while Sol's broader clock includes agent work, a discarded isolation pass, and rerun orchestration. The evidence therefore does not support a model time-to-solution claim.",
  },
  blind_discussion_rubric: {
    provenance: "Independent evaluator received anonymized discussions A and B; A maps to Luna and B maps to Sol.",
    scale: "0-4 per category; 20 maximum",
    luna: {
      contract_completeness: 4,
      edge_case_and_precedence_precision: 4,
      algorithmic_clarity_and_pseudocode: 4,
      implementation_specificity: 4,
      verification_and_limitations_awareness: 3,
      total: 19,
    },
    sol: {
      contract_completeness: 4,
      edge_case_and_precedence_precision: 3,
      algorithmic_clarity_and_pseudocode: 3,
      implementation_specificity: 2,
      verification_and_limitations_awareness: 1,
      total: 13,
    },
    result: "luna_discussion_stronger",
    rationale:
      "Both captured the contract and invalidity precedence. Luna additionally specified boundaries, duplicates, signed division, helper ordering, full helper-level pseudocode, constant complexity, and arithmetic limitations. Sol was accurate but more abstract and offered less verification or limitations analysis.",
  },
  conclusion: {
    higher_model: "not_demonstrated_for_sol",
    summary:
      "Sol high tied Luna max on both executable tasks, did not demonstrate a faster measured result, and received the lower blind discussion score. These two capped tasks do not establish a general intelligence ranking.",
    external_provider_attestation: false,
    official_general_model_score: false,
  },
};

await mkdir(outputDir, { recursive: true });
await writeFile(outputPath, `${JSON.stringify(assessment, null, 2)}\n`, "utf8");

console.log(JSON.stringify({ status: "passed", output: outputPath, assessment }, null, 2));
