import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const corpusRoot = path.join(repositoryRoot, "benchmarks", "intelligence-v3.1");
const participantRoot = path.join(corpusRoot, "final-candidates");
const privateRoot = process.env.TREATCODE_V31_FINAL_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v31-private");
const authorRoot = path.join(privateRoot, "final-authoring", "contributions");
const baseRoot = path.join(privateRoot, "final-base", "source");
const graderRoot = path.join(privateRoot, "final-graders");
const contractPath = path.join(corpusRoot, "final-corpus-contract.v3.1.json");
const evidencePath = path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", "intelligence-v31-final-package-generation.json");
const testManifest = JSON.parse(fs.readFileSync(path.join(repositoryRoot, "TEST_MANIFEST.json"), "utf8"));
const knownTargets = new Set(testManifest.suites.flatMap((suite) => suite.targets || []));
const sha256 = (value) => crypto.createHash("sha256").update(value).digest("hex");

function safeRelative(value) {
  return typeof value === "string" && value.length > 0 && !path.isAbsolute(value) && !value.includes("\\") && value.split("/").every((part) => part && part !== "." && part !== "..");
}

function writeNew(root, relative, content) {
  if (!safeRelative(relative)) throw new Error(`unsafe package path: ${relative}`);
  const absolute = path.join(root, relative);
  fs.mkdirSync(path.dirname(absolute), { recursive: true });
  fs.writeFileSync(absolute, content, { flag: "wx" });
}

function collectFiles(root) {
  const files = [];
  const visit = (directory) => {
    for (const entry of fs.readdirSync(directory, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
      const absolute = path.join(directory, entry.name);
      if (entry.isDirectory()) visit(absolute);
      else if (entry.isFile()) {
        const bytes = fs.readFileSync(absolute);
        files.push({ path: path.relative(root, absolute).replaceAll(path.sep, "/"), bytes: bytes.length, sha256: sha256(bytes) });
      }
    }
  };
  visit(root);
  return files;
}

function bundleHash(root) {
  return sha256(collectFiles(root).map((file) => `${file.path}\0${file.bytes}\0${file.sha256}\n`).join(""));
}

function replaceExactlyOnce(text, before, after, label) {
  const first = text.indexOf(before);
  if (first < 0 || text.indexOf(before, first + before.length) >= 0) throw new Error(`${label}: reference snippet must occur exactly once`);
  return `${text.slice(0, first)}${after}${text.slice(first + before.length)}`;
}

function copyBaseFile(relative, destinationRoot) {
  if (!safeRelative(relative)) throw new Error(`unsafe authored path: ${relative}`);
  const source = path.join(baseRoot, relative);
  if (!fs.existsSync(source) || !fs.statSync(source).isFile()) throw new Error(`final base is missing ${relative}`);
  const destination = path.join(destinationRoot, relative);
  fs.mkdirSync(path.dirname(destination), { recursive: true });
  fs.copyFileSync(source, destination, fs.constants.COPYFILE_EXCL);
}

if (!fs.existsSync(baseRoot) || !fs.existsSync(authorRoot)) throw new Error("ingested authorship and the private final base are required before package generation");
if (fs.existsSync(participantRoot) || fs.existsSync(graderRoot)) throw new Error("final package generation is one-shot; candidate or grader roots already exist");

const contributionFiles = fs.readdirSync(authorRoot).filter((file) => file.endsWith(".json")).sort();
const contributions = contributionFiles.map((file) => ({ file, text: fs.readFileSync(path.join(authorRoot, file), "utf8") })).map((item) => ({ ...item, value: JSON.parse(item.text), hash: sha256(item.text) }));
const authoredTasks = contributions.flatMap((contribution) => contribution.value.tasks.map((task) => ({ contribution, task })));
if (authoredTasks.length !== 100 || new Set(authoredTasks.map(({ task }) => task.task_id)).size !== 100) throw new Error("exactly 100 distinct ingested final tasks are required");

fs.mkdirSync(participantRoot, { recursive: false });
fs.mkdirSync(graderRoot, { recursive: false });
const records = [];
for (const { contribution, task } of authoredTasks.sort((a, b) => a.task.task_id.localeCompare(b.task.task_id))) {
  if (!/^TC-V31-FINAL-\d{3}$/.test(task.task_id)) throw new Error(`invalid final task id: ${task.task_id}`);
  if (task.focused_targets.some((target) => !knownTargets.has(target))) throw new Error(`${task.task_id}: focused target is not allowlisted by TEST_MANIFEST.json`);
  const released = path.join(participantRoot, task.task_id, "participant");
  const privateTaskRoot = path.join(graderRoot, task.task_id);
  const referenceRoot = path.join(privateTaskRoot, "reference");
  const participantFiles = [...new Set([...task.participant_files, ...task.distractor_files])].sort();
  fs.mkdirSync(released, { recursive: true });
  fs.mkdirSync(privateTaskRoot, { recursive: true });
  for (const relative of participantFiles) copyBaseFile(relative, released);
  for (const mutation of task.mutations) {
    if (!task.editable_files.includes(mutation.path)) throw new Error(`${task.task_id}: mutation path is not editable: ${mutation.path}`);
    const mutationPath = path.join(released, mutation.path);
    const current = fs.readFileSync(mutationPath, "utf8").replaceAll("\r\n", "\n");
    fs.writeFileSync(mutationPath, replaceExactlyOnce(current, mutation.before.replaceAll("\r\n", "\n"), mutation.after.replaceAll("\r\n", "\n"), `${task.task_id}/${mutation.path}`));
  }
  const readme = `# ${task.task_id}: ${task.title}\n\n${task.initial_signal}\n\nPublic signal: ${task.public_signal}\n\nInspect the released repository slice, repair the interacting defect, and preserve unrelated behavior. Only the files marked editable in task.repository.v3.1.json may change. A Trit source change is mandatory. Hidden grading covers behavioral, adversarial, integration, and resource-sensitive cases.\n`;
  writeNew(released, "README.md", readme);
  const editable = new Set(task.editable_files);
  const declared = collectFiles(released).map((file) => ({ path: file.path, editable: editable.has(file.path), sha256: file.sha256 }));
  if (!declared.some((file) => file.editable && file.path.endsWith(".trit"))) throw new Error(`${task.task_id}: no editable Trit source was released`);
  const manifest = {
    schema: "treatcode.intelligence.repository-task.v3.1",
    version: "3.1",
    task_id: task.task_id,
    title: task.title,
    phase: "calibration",
    initial_signal: task.initial_signal,
    known_failing_command_id: "final.public",
    public_command_ids: ["final.public"],
    required_trit_change: true,
    files: declared,
    limits: { wall_clock_ms: 1_200_000, cpu_ms: 1_200_000, memory_mb: 2048, process_count: 32, output_bytes: 8_388_608, maximum_changed_files: task.editable_files.length },
  };
  const manifestText = `${JSON.stringify(manifest, null, 2)}\n`;
  writeNew(released, "task.repository.v3.1.json", manifestText);
  const releasedHash = bundleHash(released);
  fs.cpSync(released, referenceRoot, { recursive: true, errorOnExist: true, force: false });
  for (const mutation of task.mutations) {
    const referencePath = path.join(referenceRoot, mutation.path);
    const current = fs.readFileSync(referencePath, "utf8").replaceAll("\r\n", "\n");
    fs.writeFileSync(referencePath, replaceExactlyOnce(current, mutation.after.replaceAll("\r\n", "\n"), mutation.before.replaceAll("\r\n", "\n"), `${task.task_id}/reference/${mutation.path}`));
  }
  const mutantRoots = [];
  for (let index = 0; index < task.mutations.length; index += 1) {
    const mutation = task.mutations[index];
    const mutantRoot = path.join(privateTaskRoot, "mutants", `mutation-${String(index + 1).padStart(2, "0")}`);
    fs.cpSync(referenceRoot, mutantRoot, { recursive: true, errorOnExist: true, force: false });
    const mutantPath = path.join(mutantRoot, mutation.path);
    const current = fs.readFileSync(mutantPath, "utf8").replaceAll("\r\n", "\n");
    fs.writeFileSync(mutantPath, replaceExactlyOnce(current, mutation.before.replaceAll("\r\n", "\n"), mutation.after.replaceAll("\r\n", "\n"), `${task.task_id}/mutant-${index + 1}/${mutation.path}`));
    mutantRoots.push(path.relative(privateTaskRoot, mutantRoot).replaceAll(path.sep, "/"));
  }
  const privateTask = {
    schema: "treatcode.intelligence.private-task.v3.1",
    version: "3.1",
    task,
    authorship: contribution.value.authorship,
    contribution_sha256: contribution.hash,
    focused_suite_targets: Object.fromEntries(task.focused_suites.map((suiteName) => {
      const suite = testManifest.suites.find((item) => item.name === suiteName);
      if (!suite) throw new Error(`${task.task_id}: unknown focused suite ${suiteName}`);
      return [suiteName, [...(suite.targets || [])]];
    })),
    mutant_roots: mutantRoots,
  };
  writeNew(privateTaskRoot, "task.private.v3.1.json", `${JSON.stringify(privateTask, null, 2)}\n`);
  const grader = {
    schema: "treatcode.intelligence.repository-grader.v3.1",
    version: "3.1",
    task_id: task.task_id,
    participant_manifest_sha256: sha256(manifestText),
    baseline_bundle_sha256: releasedHash,
    suites: [
      { id: "behavioral", kind: "behavioral", command_id: "final.private.behavioral", required: true },
      { id: "adversarial", kind: "adversarial", command_id: "final.private.adversarial", required: true },
      { id: "performance", kind: "performance", command_id: "final.private.performance", required: true },
    ],
  };
  writeNew(privateTaskRoot, "grader.repository.v3.1.json", `${JSON.stringify(grader, null, 2)}\n`);
  records.push({ task_id: task.task_id, participant_bundle_sha256: releasedHash, grader_bundle_sha256: bundleHash(privateTaskRoot), participant_file_count: declared.length, editable_file_count: task.editable_files.length, mutant_count: mutantRoots.length });
}

const contract = JSON.parse(fs.readFileSync(contractPath, "utf8"));
contract.status = "unseen_candidates_packaged_for_qualification";
contract.readiness.candidate_tasks = 100;
contract.readiness.executable_packages = 100;
contract.readiness.independently_authored = 100;
fs.writeFileSync(contractPath, `${JSON.stringify(contract, null, 2)}\n`);
const evidence = { schema: "treatcode.intelligence.final-package-generation-evidence.v3.1", version: "3.1", official: false, generated_at: new Date().toISOString(), task_count: records.length, records };
fs.mkdirSync(path.dirname(evidencePath), { recursive: true });
fs.writeFileSync(evidencePath, `${JSON.stringify(evidence, null, 2)}\n`, { flag: "wx" });
console.log(JSON.stringify({ status: "generated", official: false, task_count: records.length, participant_root: path.relative(repositoryRoot, participantRoot).replaceAll(path.sep, "/"), private_graders: records.length }, null, 2));
