import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const corpusRoot = path.join(repoRoot, "benchmarks", "intelligence-v3.1");
const privateRoot = process.env.TREATCODE_V31_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v31-private", "pilot");
const evaluator = path.join(appRoot, "scripts", "intelligence-v31-task-evaluator.mjs");
const tritc = path.join(repoRoot, "build", process.platform === "win32" ? "tritc.exe" : "tritc");
const pilot = JSON.parse(fs.readFileSync(path.join(corpusRoot, "pilot-corpus.v3.1.json"), "utf8"));
const failures = [];
const results = [];

if (pilot.tasks.every((task) => task.package_status === "disposed" && task.disposition === "discarded_after_ceiling_analysis")) {
  const disposalPath = path.join(repoRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v31-pilot-disposal-manifest.json");
  const disposal = JSON.parse(fs.readFileSync(disposalPath, "utf8"));
  const valid = disposal.schema === "treatcode.intelligence.pilot-disposal.v3.1" && disposal.tasks?.length === 30 && !fs.existsSync(path.join(corpusRoot, "pilots")) && !fs.existsSync(privateRoot);
  console.log(`Intelligence v3.1 pilot packages: ${valid ? "disposed with hash-bound evidence" : "invalid disposal evidence"} (30/30)`);
  process.exit(valid ? 0 : 1);
}

function evaluate(workspace, cases, visibility) {
  return spawnSync(process.execPath, [evaluator, `--workspace=${workspace}`, `--cases=${cases}`, `--tritc=${tritc}`, `--visibility=${visibility}`], { cwd: appRoot, encoding: "utf8", shell: false, windowsHide: true, timeout: 100000, maxBuffer: 4 * 1_048_576 });
}

for (const task of pilot.tasks) {
  const participant = path.join(repoRoot, task.participant_package);
  const grader = path.join(privateRoot, task.task_id);
  const reference = path.join(grader, "reference");
  const starterPublic = evaluate(participant, path.join(participant, "public", "cases.json"), "public");
  const starterFails = starterPublic.status !== 0;
  const referenceOutcomes = [
    evaluate(reference, path.join(reference, "public", "cases.json"), "public"),
    ...["behavioral", "adversarial", "performance"].map((suite) => evaluate(reference, path.join(grader, `${suite}.cases.v3.1.json`), "private")),
  ];
  const referencePasses = referenceOutcomes.every((result) => result.status === 0);
  const mutantOutcomes = ["boundary", "transition", "fold"].map((mutant) => evaluate(path.join(grader, "mutants", mutant), path.join(grader, "adversarial.cases.v3.1.json"), "private"));
  const mutantsCaught = mutantOutcomes.every((result) => result.status !== 0);
  const ok = starterFails && referencePasses && mutantsCaught;
  results.push({ task_id: task.task_id, starter_fails: starterFails, reference_passes: referencePasses, adversarial_mutants_caught: mutantOutcomes.filter((result) => result.status !== 0).length, adversarial_mutants_total: mutantOutcomes.length, ok });
  if (!ok) failures.push({ task_id: task.task_id, starter_status: starterPublic.status, reference_statuses: referenceOutcomes.map((result) => result.status), mutant_statuses: mutantOutcomes.map((result) => result.status) });
  console.log(`  [${ok ? "ok" : "fail"}] ${task.task_id}: starter=${starterFails ? "fail" : "PASS"} reference=${referencePasses ? "pass" : "FAIL"} mutants=${mutantOutcomes.filter((result) => result.status !== 0).length}/3`);
}

const report = {
  schema: "trit.treatcode_intelligence_v31_pilot_package_validation.v1",
  official: false,
  status: failures.length === 0 ? "executable_development_packages_validated" : "validation_failed",
  ok: failures.length === 0,
  task_count: results.length,
  starters_failed: results.filter((item) => item.starter_fails).length,
  references_passed: results.filter((item) => item.reference_passes).length,
  adversarial_mutants_caught: results.reduce((sum, item) => sum + item.adversarial_mutants_caught, 0),
  adversarial_mutants_total: results.reduce((sum, item) => sum + item.adversarial_mutants_total, 0),
  results,
  failures,
};
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P14");
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "intelligence-v31-pilot-package-validation.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(`Intelligence v3.1 pilot packages: ${report.ok ? "passed" : "failed"} (${report.references_passed}/${report.task_count} references, ${report.adversarial_mutants_caught}/${report.adversarial_mutants_total} mutants caught)`);
process.exitCode = report.ok ? 0 : 1;
