import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const corpusRoot = path.join(repoRoot, "benchmarks", "intelligence-v3.1");
const packageRoot = path.join(corpusRoot, "pilots");
const privateRoot = process.env.TREATCODE_V31_PRIVATE_ROOT || path.join(process.env.LOCALAPPDATA || os.tmpdir(), "TreatCode", "intelligence-v31-private", "pilot");
const pilotPath = path.join(corpusRoot, "pilot-corpus.v3.1.json");
const pilot = JSON.parse(fs.readFileSync(pilotPath, "utf8"));
const sha256 = (value) => crypto.createHash("sha256").update(value).digest("hex");

function collectFiles(root) {
  const result = [];
  const visit = (directory) => {
    for (const entry of fs.readdirSync(directory, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
      const absolute = path.join(directory, entry.name);
      if (entry.isDirectory()) visit(absolute);
      else if (entry.isFile()) {
        const content = fs.readFileSync(absolute);
        result.push({ path: path.relative(root, absolute).replaceAll(path.sep, "/"), bytes: content.byteLength, sha256: sha256(content) });
      }
    }
  };
  visit(root);
  return result;
}

function bundleHash(root) {
  return sha256(collectFiles(root).map((file) => `${file.path}\0${file.bytes}\0${file.sha256}\n`).join(""));
}

function write(root, relative, content) {
  const absolute = path.join(root, relative);
  fs.mkdirSync(path.dirname(absolute), { recursive: true });
  fs.writeFileSync(absolute, content);
}

function constants(index) {
  return { limit: 6 + index % 7, bias: index % 3 - 1, offset: 2 + index % 5, scale: 2 + index % 2, delta: index % 4 - 2 };
}

function normalize(x, c) {
  let y = Math.max(-c.limit, Math.min(c.limit, x));
  if (y < 0) return y - c.bias;
  if (y > 0) return y + c.bias;
  return 0;
}

function transition(state, event, aux, c) {
  if (event < 0) return normalize(state - aux - c.offset, c);
  if (event === 0) return normalize(state + c.offset, c);
  return normalize(state + aux, c);
}

function finalize(value, mode, c) {
  const normalized = normalize(value, c);
  if (mode < 0) return -normalized + c.delta;
  if (mode === 0) return normalized;
  return normalized * c.scale + c.delta;
}

function boundedFold(seed, steps, c) {
  let acc = normalize(seed, c);
  let direction = -1;
  for (let index = 0; index < steps; index += 1) {
    acc = normalize(acc + direction, c);
    direction = direction < 1 ? direction + 1 : -1;
  }
  return acc;
}

function makeCases(index, suite) {
  const c = constants(index);
  let foldDiscriminator = [1, 1];
  outer: for (let seed = -c.limit; seed <= c.limit; seed += 1) {
    for (let steps = 1; steps <= 20; steps += 1) {
      if (boundedFold(seed, steps, c) !== boundedFold(seed, steps - 1, c)) {
        foldDiscriminator = [seed, steps];
        break outer;
      }
    }
  }
  const selected = suite === "public" ? [
    ["normalize_value", [-c.limit - 3], normalize(-c.limit - 3, c)],
    ["transition_state", [2, 0, 3], transition(2, 0, 3, c)],
    ["finalize_result", [-4, -1], finalize(-4, -1, c)],
    ["bounded_fold", [1, 4], boundedFold(1, 4, c)],
  ] : suite === "behavioral" ? [
    ...[-c.limit - 3, -c.limit, -2, 0, 2, c.limit, c.limit + 3].map((x) => ["normalize_value", [x], normalize(x, c)]),
    ...[[-4, -1, 2], [4, 1, 3], [2, 0, 5], [-2, 1, 6], [5, -1, 1]].map((args) => ["transition_state", args, transition(...args, c)]),
    ...[[-5, -1], [-2, 0], [0, 1], [4, 1]].map((args) => ["finalize_result", args, finalize(...args, c)]),
  ] : suite === "adversarial" ? [
    ...[-c.limit - 40, -c.limit - 1, -1, 1, c.limit + 1, c.limit + 40].map((x) => ["normalize_value", [x], normalize(x, c)]),
    ...[[-c.limit, 0, c.limit], [c.limit, -1, c.limit], [0, 1, c.limit], [0, -1, c.limit]].map((args) => ["transition_state", args, transition(...args, c)]),
    ...[[-c.limit - 8, -1], [c.limit + 8, 1]].map((args) => ["finalize_result", args, finalize(...args, c)]),
    ...[[0, 0], [c.limit, 1], [-c.limit, 7], [2, 13], foldDiscriminator].map((args) => ["bounded_fold", args, boundedFold(...args, c)]),
  ] : [
    ["bounded_fold", [c.limit, 60], boundedFold(c.limit, 60, c)],
    ["bounded_fold", [-c.limit, 90], boundedFold(-c.limit, 90, c)],
  ];
  return selected.map(([fn, args, expected]) => ({ function: fn, args, expected }));
}

function adjacentFile(task, index, fixed) {
  const languages = task.languages || [];
  if (languages.includes("C++")) return { path: "native/contract.cpp", content: `constexpr int kTritTaskAbi = ${fixed ? index : index - 1};\n`, expected: `kTritTaskAbi = ${index};` };
  if (languages.includes("TypeScript")) return { path: "bridge/contract.ts", content: `export const TRIT_TASK_ABI = ${fixed ? index : index - 1};\n`, expected: `TRIT_TASK_ABI = ${index};` };
  if (languages.includes("CMake")) return { path: "CMakeLists.txt", content: `set(TRIT_TASK_ABI ${fixed ? index : index - 1})\n`, expected: `TRIT_TASK_ABI ${index}` };
  if (languages.includes("JSON")) return { path: "task-contract.json", content: `${JSON.stringify({ abi: fixed ? index : index - 1, required_trit: true }, null, 2)}\n`, expected: `"abi": ${index}` };
  return { path: "bridge/contract.txt", content: `TRIT_TASK_ABI=${fixed ? index : index - 1}\n`, expected: `TRIT_TASK_ABI=${index}` };
}

function sources(index, fixed) {
  const c = constants(index);
  const lower = fixed ? -c.limit : -c.limit + 1;
  const zero = fixed ? 0 : c.bias;
  const zeroTransition = fixed ? `state + ${c.offset}` : `state - ${c.offset}`;
  const negativeFinalize = fixed ? `(0 - normalized) + ${c.delta}` : `(0 - normalized) - ${c.delta}`;
  const foldLimit = fixed ? "steps" : "steps - 1";
  return {
    "src/normalize.trit": `fn normalize_value(x: t40) -> t40 {\n    var y: t40 = x;\n    if y < ${-c.limit} { y = ${lower}; }\n    if y > ${c.limit} { y = ${c.limit}; }\n    if y < 0 { return y - ${c.bias}; }\n    if y > 0 { return y + ${c.bias}; }\n    return ${zero};\n}\n`,
    "src/transition.trit": `fn transition_state(state: t40, event: t40, aux: t40) -> t40 {\n    var candidate: t40 = state;\n    if event < 0 { candidate = state - aux - ${c.offset}; }\n    else {\n        if event == 0 { candidate = ${zeroTransition}; }\n        else { candidate = state + aux; }\n    }\n    return normalize_value(candidate);\n}\n`,
    "src/finalize.trit": `fn finalize_result(value: t40, mode: t40) -> t40 {\n    var normalized: t40 = normalize_value(value);\n    if mode < 0 { return ${negativeFinalize}; }\n    if mode == 0 { return normalized; }\n    return normalized * ${c.scale} + ${c.delta};\n}\n\nfn bounded_fold(seed: t40, steps: t40) -> t40 {\n    var acc: t40 = normalize_value(seed);\n    var direction: t40 = -1;\n    var i: t40 = 0;\n    while i < ${foldLimit} {\n        acc = normalize_value(acc + direction);\n        if direction < 1 { direction = direction + 1; }\n        else { direction = -1; }\n        i = i + 1;\n    }\n    return acc;\n}\n`,
    "src/distractor.trit": `fn legacy_policy_${index}(state: t40, event: t40) -> t40 {\n    if event < 0 { return state + event; }\n    return state - event;\n}\n`,
  };
}

fs.mkdirSync(packageRoot, { recursive: true });
fs.mkdirSync(privateRoot, { recursive: true });
for (let taskOffset = 0; taskOffset < pilot.tasks.length; taskOffset += 1) {
  const task = pilot.tasks[taskOffset];
  const index = taskOffset + 1;
  const participantRoot = path.join(packageRoot, task.task_id, "participant");
  const graderRoot = path.join(privateRoot, task.task_id);
  fs.rmSync(participantRoot, { recursive: true, force: true });
  fs.rmSync(graderRoot, { recursive: true, force: true });
  fs.mkdirSync(participantRoot, { recursive: true });
  fs.mkdirSync(graderRoot, { recursive: true });
  const c = constants(index);
  const adjacentStarter = adjacentFile(task, index, false);
  const adjacentReference = adjacentFile(task, index, true);
  const participantFiles = {
    ...sources(index, false),
    [adjacentStarter.path]: adjacentStarter.content,
    "public/cases.json": `${JSON.stringify({ schema: "treatcode.intelligence.repository-cases.v3.1", version: "3.1", suite: "public", trit_files: ["src/normalize.trit", "src/transition.trit", "src/finalize.trit", "src/distractor.trit"], cases: makeCases(index, "public"), file_assertions: [] }, null, 2)}\n`,
    "build/task-config.json": `${JSON.stringify({ task_id: task.task_id, compiler_mode: "strict", step_limit: 500000 }, null, 2)}\n`,
    "README.md": `# ${task.task_id}: ${task.title}\n\n${task.initial_signal}\n\nRepair the repository contract, not only the public examples. The four exported Trit functions must obey these rules:\n\n- \`normalize_value(x)\`: clamp to [${-c.limit}, ${c.limit}], subtract ${c.bias} from negative values, add ${c.bias} to positive values, and keep zero canonical.\n- \`transition_state(state,event,aux)\`: negative events subtract aux and ${c.offset}; zero events add ${c.offset}; positive events add aux; normalize the result.\n- \`finalize_result(value,mode)\`: normalize first; negative mode returns the negated value plus ${c.delta}, zero returns it, positive mode multiplies it by ${c.scale} and adds ${c.delta}.\n- \`bounded_fold(seed,steps)\`: normalize seed, then apply the repeating delta sequence -1, 0, +1 for exactly \`steps\` transitions.\n\nThe adjacent ${path.extname(adjacentStarter.path).slice(1) || "text"} contract must advertise ABI level ${index}. Do not edit read-only fixtures or add files. At least one Trit file must change.\n`,
  };
  for (const [relative, content] of Object.entries(participantFiles)) write(participantRoot, relative, content);
  const editable = new Set(["src/normalize.trit", "src/transition.trit", "src/finalize.trit", adjacentStarter.path]);
  const manifest = {
    schema: "treatcode.intelligence.repository-task.v3.1",
    version: "3.1",
    task_id: task.task_id,
    title: task.title,
    phase: "pilot",
    initial_signal: task.initial_signal,
    known_failing_command_id: "pilot.public",
    public_command_ids: ["pilot.public"],
    required_trit_change: true,
    files: collectFiles(participantRoot).map((file) => ({ path: file.path, editable: editable.has(file.path), sha256: file.sha256 })),
    limits: { wall_clock_ms: 1_200_000, cpu_ms: 1_200_000, memory_mb: 512, process_count: 8, output_bytes: 262144, maximum_changed_files: editable.size },
  };
  const manifestText = `${JSON.stringify(manifest, null, 2)}\n`;
  write(participantRoot, "task.repository.v3.1.json", manifestText);
  const baselineHash = bundleHash(participantRoot);
  const suiteFiles = {};
  for (const suite of ["behavioral", "adversarial", "performance"]) {
    const caseBundle = { schema: "treatcode.intelligence.repository-cases.v3.1", version: "3.1", suite, trit_files: ["src/normalize.trit", "src/transition.trit", "src/finalize.trit", "src/distractor.trit"], cases: makeCases(index, suite), file_assertions: suite === "behavioral" ? [{ path: adjacentStarter.path, contains: adjacentReference.expected }] : [], ...(suite === "performance" ? { maximum_cycles: 25000, step_limit: 500000 } : {}) };
    const name = `${suite}.cases.v3.1.json`;
    write(graderRoot, name, `${JSON.stringify(caseBundle, null, 2)}\n`);
    suiteFiles[suite] = name;
  }
  const grader = {
    schema: "treatcode.intelligence.repository-grader.v3.1",
    version: "3.1",
    task_id: task.task_id,
    participant_manifest_sha256: sha256(manifestText),
    baseline_bundle_sha256: baselineHash,
    suites: [
      { id: "behavioral", kind: "behavioral", command_id: "pilot.private.behavioral", required: true },
      { id: "adversarial", kind: "adversarial", command_id: "pilot.private.adversarial", required: true },
      { id: "performance", kind: "performance", command_id: "pilot.private.performance", required: true }
    ],
  };
  write(graderRoot, "grader.repository.v3.1.json", `${JSON.stringify(grader, null, 2)}\n`);
  const referenceRoot = path.join(graderRoot, "reference");
  fs.cpSync(participantRoot, referenceRoot, { recursive: true });
  for (const [relative, content] of Object.entries(sources(index, true))) write(referenceRoot, relative, content);
  write(referenceRoot, adjacentReference.path, adjacentReference.content);
  const mutantNames = ["boundary", "transition", "fold"];
  for (const mutantName of mutantNames) {
    const mutantRoot = path.join(graderRoot, "mutants", mutantName);
    fs.cpSync(referenceRoot, mutantRoot, { recursive: true });
    const starter = sources(index, false);
    const mutatedFile = mutantName === "boundary" ? "src/normalize.trit" : mutantName === "transition" ? "src/transition.trit" : "src/finalize.trit";
    write(mutantRoot, mutatedFile, starter[mutatedFile]);
  }
  write(graderRoot, "package-evidence.v3.1.json", `${JSON.stringify({ schema: "treatcode.intelligence.pilot-package-evidence.v3.1", task_id: task.task_id, participant_bundle_sha256: baselineHash, reference_snapshot_sha256: bundleHash(referenceRoot), reference_authorship: "generated development fixture; independent authorship not claimed", suite_files: suiteFiles, adversarial_mutants: mutantNames, official: false }, null, 2)}\n`);
  task.package_status = "executable";
  task.participant_package = path.relative(repoRoot, participantRoot).replaceAll(path.sep, "/");
  task.private_grader_status = "installed_outside_repository";
}
pilot.status = "executable_packages_ready_for_disposable_subject_pilots";
pilot.warning = "Pilot packages may be regenerated for explicitly labeled development runs. They remain exposed, disposable evidence and are permanently ineligible for any Luna/Sol holdout.";
pilot.executable_package_count = 30;
pilot.private_grader_root_exposed_to_subjects = false;
fs.writeFileSync(pilotPath, `${JSON.stringify(pilot, null, 2)}\n`);
console.log(JSON.stringify({ schema: "treatcode.intelligence.pilot-package-generation.v3.1", official: false, task_count: 30, participant_root: path.relative(repoRoot, packageRoot).replaceAll(path.sep, "/"), private_graders_installed: 30 }, null, 2));
