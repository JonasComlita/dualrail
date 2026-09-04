import { createHash } from "node:crypto";
import { existsSync, readdirSync, readFileSync, writeFileSync, mkdirSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { calibrateIntelligenceV31Candidates, type IntelligenceV31CalibrationObservation } from "../src/intelligenceV31Calibration";

const repositoryRoot = path.resolve(import.meta.dir, "..", "..");
const privateRoot = process.env.TREATCODE_V31_FINAL_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v31-private");
const authorRoot = path.join(privateRoot, "final-authoring", "contributions");
const graderRoot = path.join(privateRoot, "final-graders");
const observationRoot = path.join(privateRoot, "final-calibration", "observations");
const qualificationRoot = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v31-final-qualification");
const reportPath = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v31-final-calibration.json");
const sha256 = (value: string | Uint8Array) => createHash("sha256").update(value).digest("hex");

if (!existsSync(authorRoot) || !existsSync(graderRoot)) throw new Error("ingested authorship and generated final graders are required");
const authorFiles = readdirSync(authorRoot).filter((file) => file.endsWith(".json")).sort();
const authored = authorFiles.flatMap((file) => {
  const contribution = JSON.parse(readFileSync(path.join(authorRoot, file), "utf8"));
  return contribution.tasks.map((task: { task_id: string }) => ({ task_id: task.task_id, author_model_family: contribution.authorship.model }));
});
if (authored.length !== 100) throw new Error("calibration aggregation requires exactly 100 ingested authored tasks");
const qualificationFiles = existsSync(qualificationRoot) ? readdirSync(qualificationRoot).filter((file) => file.endsWith(".json")) : [];
const qualifications = qualificationFiles.map((file) => JSON.parse(readFileSync(path.join(qualificationRoot, file), "utf8")));
const tasks = authored.sort((a, b) => a.task_id.localeCompare(b.task_id)).map((authoredTask) => {
  const grader = JSON.parse(readFileSync(path.join(graderRoot, authoredTask.task_id, "grader.repository.v3.1.json"), "utf8"));
  const qualification = qualifications.find((item) => item.task_id === authoredTask.task_id && item.participant_bundle_sha256 === grader.baseline_bundle_sha256);
  return {
    ...authoredTask,
    participant_bundle_hash: grader.baseline_bundle_sha256,
    qualification: qualification ? { reference_passed: qualification.reference_passed, starter_failed: qualification.starter_failed, mutants_caught: qualification.mutants_caught, mutants_total: qualification.mutants_total } : { reference_passed: false, starter_failed: false, mutants_caught: 0, mutants_total: 1 },
  };
});
const observationFiles = existsSync(observationRoot) ? readdirSync(observationRoot).filter((file) => file.endsWith(".json")).sort() : [];
const observations = observationFiles.map((file) => JSON.parse(readFileSync(path.join(observationRoot, file), "utf8")) as IntelligenceV31CalibrationObservation);
const report = calibrateIntelligenceV31Candidates({ tasks, observations });
const observationSetHash = sha256(observationFiles.map((file, index) => `${file}\0${sha256(JSON.stringify(observations[index]))}\n`).join(""));
const output = { ...report, generated_at: new Date().toISOString(), observation_set_sha256: observationSetHash, infrastructure_blocked: observations.some((item) => !item.network_isolation_verified || !item.resource_limits_verified || !item.append_only_evidence_verified) || observations.length === 0 };
mkdirSync(path.dirname(reportPath), { recursive: true });
writeFileSync(reportPath, `${JSON.stringify(output, null, 2)}\n`);
console.log(JSON.stringify({ status: output.status, task_count: output.task_count, observation_count: output.observation_count, accepted_tasks: output.accepted_tasks, revise_tasks: output.revise_tasks, insufficient_tasks: output.insufficient_tasks, ceiling_fraction: output.ceiling_fraction, floor_fraction: output.floor_fraction, infrastructure_blocked: output.infrastructure_blocked, blocker_count: output.blockers.length }, null, 2));
