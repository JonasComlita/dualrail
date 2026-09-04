import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const contributionRoot = path.join(repositoryRoot, "benchmarks", "intelligence-v3.1", "authoring", "contributions");
const testManifest = JSON.parse(fs.readFileSync(path.join(repositoryRoot, "TEST_MANIFEST.json"), "utf8"));
const suiteNames = new Set(testManifest.suites.map((suite) => suite.name));
const targetNames = new Set(testManifest.suites.flatMap((suite) => [...(suite.targets || []), ...(suite.ctest_tests || [])]));
const errors = [];
const sha = (value) => crypto.createHash("sha256").update(value).digest("hex");
const normalizeText = (value) => value.replaceAll("\r\n", "\n");
const count = (text, needle) => needle ? text.split(needle).length - 1 : 0;
const safePath = (value) => typeof value === "string" && value.length > 0 && !path.isAbsolute(value) && !value.includes("\\") && !value.split("/").some((part) => !part || part === "." || part === "..");
const canonical = (value) => Array.isArray(value) ? value.map(canonical) : value && typeof value === "object" ? Object.fromEntries(Object.entries(value).sort(([a], [b]) => a.localeCompare(b)).map(([key, item]) => [key, canonical(item)])) : value;
const expected = [
  { file: "author-a-gpt54.json", first: 1, last: 34, author: "model-agent-gpt54-author-a4", model: "gpt-5.4", counts: { algorithm_and_data_structure: 5, balanced_ternary_and_numeric: 4, compiler_and_codegen: 5, memory_and_pointer_safety: 3, concurrency_and_state: 3, syscall_and_abi: 3, multi_module_repository_repair: 5, build_test_and_tooling: 3, performance_and_resource_constraints: 3 } },
  { file: "author-b-gpt54.json", first: 35, last: 67, author: "model-agent-gpt54-author-b3", model: "gpt-5.4", counts: { algorithm_and_data_structure: 5, balanced_ternary_and_numeric: 3, compiler_and_codegen: 5, memory_and_pointer_safety: 3, concurrency_and_state: 3, syscall_and_abi: 3, multi_module_repository_repair: 5, build_test_and_tooling: 4, performance_and_resource_constraints: 2 } },
  { file: "author-c-gpt54-v2.json", first: 68, last: 100, author: "model-agent-gpt54-author-c2", model: "gpt-5.4", counts: { algorithm_and_data_structure: 5, balanced_ternary_and_numeric: 3, compiler_and_codegen: 5, memory_and_pointer_safety: 4, concurrency_and_state: 4, syscall_and_abi: 4, multi_module_repository_repair: 5, build_test_and_tooling: 3, performance_and_resource_constraints: 0 } },
];
const contributions = [];

for (const assignment of expected) {
  const filePath = path.join(contributionRoot, assignment.file);
  if (!fs.existsSync(filePath)) { errors.push(`${assignment.file}: contribution is missing`); continue; }
  const text = fs.readFileSync(filePath, "utf8");
  let contribution;
  try { contribution = JSON.parse(text); } catch (error) { errors.push(`${assignment.file}: invalid JSON (${error.message})`); continue; }
  if (contribution.schema !== "treatcode.intelligence.author-contribution.v3.1" || contribution.version !== "3.1") errors.push(`${assignment.file}: invalid contribution schema`);
  if (contribution.authorship?.contributor_id !== assignment.author || contribution.authorship?.model !== assignment.model || contribution.authorship?.contributor_kind !== "model_agent" || !Number.isFinite(Date.parse(contribution.authorship?.completed_at || "")) || `${contribution.authorship?.attestation || ""}`.length < 40) errors.push(`${assignment.file}: authorship metadata is incomplete or mismatched`);
  if (["gpt-5.6-luna", "gpt-5.6-sol"].includes(contribution.authorship?.model)) errors.push(`${assignment.file}: subject model family authored final candidates`);
  const wantedIds = Array.from({ length: assignment.last - assignment.first + 1 }, (_, index) => `TC-V31-FINAL-${String(assignment.first + index).padStart(3, "0")}`);
  if (!Array.isArray(contribution.tasks) || contribution.tasks.length !== wantedIds.length || contribution.tasks.some((task, index) => task.task_id !== wantedIds[index])) errors.push(`${assignment.file}: task ids/count/order do not match the assigned range`);
  const categories = Object.fromEntries(Object.keys(assignment.counts).map((category) => [category, contribution.tasks?.filter((task) => task.category === category).length || 0]));
  if (JSON.stringify(categories) !== JSON.stringify(assignment.counts)) errors.push(`${assignment.file}: category allocation does not match the assigned mix`);
  for (const task of contribution.tasks || []) {
    const label = `${assignment.file}/${task.task_id}`;
    if (!Array.isArray(task.participant_files) || task.participant_files.length < 8 || new Set(task.participant_files).size !== task.participant_files.length || task.participant_files.some((file) => !safePath(file))) errors.push(`${label}: participant_files must contain at least 8 safe distinct paths`);
    if (!Array.isArray(task.editable_files) || task.editable_files.length < 3 || new Set(task.editable_files).size !== task.editable_files.length || task.editable_files.some((file) => !task.participant_files?.includes(file))) errors.push(`${label}: editable_files must be at least 3 participant paths`);
    if (!task.editable_files?.some((file) => file.endsWith(".trit"))) errors.push(`${label}: no editable Trit file`);
    if (!Array.isArray(task.distractor_files) || task.distractor_files.length < 3 || new Set(task.distractor_files).size !== task.distractor_files.length || task.distractor_files.some((file) => !safePath(file) || !fs.existsSync(path.join(repositoryRoot, file)))) errors.push(`${label}: distractor_files must name at least 3 safe existing repository paths`);
    if (!Array.isArray(task.mutations) || task.mutations.length < 2 || new Set(task.mutations?.map((mutation) => mutation.path)).size < 2) errors.push(`${label}: at least two cross-file mutations are required`);
    for (const file of task.participant_files || []) if (!fs.existsSync(path.join(repositoryRoot, file))) errors.push(`${label}: participant file does not exist: ${file}`);
    for (const mutation of task.mutations || []) {
      if (!task.editable_files?.includes(mutation.path) || typeof mutation.before !== "string" || typeof mutation.after !== "string" || mutation.before === mutation.after || `${mutation.invariant || ""}`.length < 12 || `${mutation.hidden_test_rationale || ""}`.length < 12) { errors.push(`${label}: malformed mutation for ${mutation.path}`); continue; }
      const sourcePath = path.join(repositoryRoot, mutation.path);
      if (!fs.existsSync(sourcePath)) continue;
      const occurrences = count(normalizeText(fs.readFileSync(sourcePath, "utf8")), normalizeText(mutation.before));
      if (occurrences !== 1) errors.push(`${label}: mutation before snippet occurs ${occurrences} times in ${mutation.path}, expected exactly once`);
    }
    if (!Array.isArray(task.focused_suites) || task.focused_suites.length === 0 || task.focused_suites.some((suite) => !suiteNames.has(suite))) errors.push(`${label}: focused_suites contains an unknown TEST_MANIFEST suite`);
    if (!Array.isArray(task.focused_targets) || task.focused_targets.length === 0 || task.focused_targets.some((target) => !targetNames.has(target))) errors.push(`${label}: focused_targets contains an unknown TEST_MANIFEST target`);
    const features = task.design_features || {};
    const featureKeys = ["repository_exploration_required", "multi_module_change", "misleading_or_incomplete_tests", "conflicting_invariants", "initial_build_failure", "abi_or_integration_bug", "performance_constraint"];
    if (features.repository_exploration_required !== true || featureKeys.some((key) => typeof features[key] !== "boolean")) errors.push(`${label}: design_features are incomplete`);
  }
  contributions.push({ assignment, filePath, text, contribution, sha256: sha(text) });
}

const tasks = contributions.flatMap((item) => item.contribution.tasks || []);
const categoryCounts = Object.fromEntries([...new Set(tasks.map((task) => task.category))].sort().map((category) => [category, tasks.filter((task) => task.category === category).length]));
const featureCounts = Object.fromEntries(["repository_exploration_required", "multi_module_change", "misleading_or_incomplete_tests", "conflicting_invariants", "initial_build_failure", "abi_or_integration_bug", "performance_constraint"].map((feature) => [feature, tasks.filter((task) => task.design_features?.[feature]).length]));
const minimums = { repository_exploration_required: 100, multi_module_change: 80, misleading_or_incomplete_tests: 40, conflicting_invariants: 40, initial_build_failure: 30, abi_or_integration_bug: 30, performance_constraint: 25 };
for (const [feature, minimum] of Object.entries(minimums)) if ((featureCounts[feature] || 0) < minimum) errors.push(`aggregate feature ${feature} has ${featureCounts[feature] || 0}, requires ${minimum}`);
if (tasks.length !== 100 || new Set(tasks.map((task) => task.task_id)).size !== 100 || new Set(tasks.map((task) => task.title)).size !== 100 || new Set(tasks.map((task) => task.initial_signal)).size !== 100) errors.push("aggregate tasks, titles, or initial signals are not 100 distinct items");

const report = {
  schema: "treatcode.intelligence.authoring-validation.v3.1",
  version: "3.1",
  official: false,
  status: errors.length ? "invalid" : "authored_candidates_validated_pending_review_and_qualification",
  task_count: tasks.length,
  category_counts: categoryCounts,
  design_feature_counts: featureCounts,
  contributions: contributions.map((item) => ({ contributor_id: item.contribution.authorship.contributor_id, model: item.contribution.authorship.model, task_count: item.contribution.tasks.length, contribution_sha256: item.sha256, attestation_sha256: sha(JSON.stringify(canonical(item.contribution.authorship))) })),
  errors,
};
const evidenceRoot = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14");
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "intelligence-v31-authoring-validation.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(JSON.stringify({ status: report.status, task_count: report.task_count, category_counts: report.category_counts, design_feature_counts: report.design_feature_counts, errors: errors.slice(0, 50), error_count: errors.length }, null, 2));
process.exitCode = errors.length ? 1 : 0;
