import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";

const argument = (name) => process.argv.find((value) => value.startsWith(`--${name}=`))?.slice(name.length + 3);
const workspace = path.resolve(argument("workspace") || "");
const privateRoot = path.resolve(argument("private-root") || "");
const taskId = argument("task") || "";
const suiteKind = argument("suite") || "";
const privateTaskRoot = path.join(privateRoot, "final-graders", taskId);
const baseRoot = path.join(privateRoot, "final-base", "source");
const taskRecordPath = path.join(privateTaskRoot, "task.private.v3.1.json");
const sha256 = (value) => crypto.createHash("sha256").update(value).digest("hex");

function fail(message, detail = "") {
  if (suiteKind === "public" && detail) console.error(`${message}: ${detail}`);
  else console.error(message);
  process.exit(1);
}

function inside(root, candidate) {
  const relative = path.relative(path.resolve(root), path.resolve(candidate));
  return relative === "" || (relative !== ".." && !relative.startsWith(`..${path.sep}`) && !path.isAbsolute(relative));
}

function safeRelative(value) {
  return typeof value === "string" && value.length > 0 && !path.isAbsolute(value) && !value.includes("\\") && value.split("/").every((part) => part && part !== "." && part !== "..");
}

function run(executable, args, cwd, timeout) {
  const result = spawnSync(executable, args, { cwd, encoding: "utf8", shell: false, windowsHide: true, timeout, maxBuffer: 8 * 1_048_576, env: { ...process.env, GIT_CONFIG_NOSYSTEM: "1", GIT_TERMINAL_PROMPT: "0", NO_PROXY: "*", no_proxy: "*", HTTP_PROXY: "http://127.0.0.1:9", HTTPS_PROXY: "http://127.0.0.1:9", ALL_PROXY: "http://127.0.0.1:9" } });
  return { status: result.status, stdout: result.stdout || "", stderr: result.stderr || "", error: result.error?.message || null };
}

function participantContentHash(recordValue) {
  const relatives = [...new Set([...recordValue.task.participant_files, ...recordValue.task.distractor_files])].sort();
  return sha256(relatives.map((relative) => {
    if (!safeRelative(relative)) fail("private task contains an unsafe path");
    const absolute = path.join(workspace, relative);
    if (!inside(workspace, absolute) || !fs.existsSync(absolute) || !fs.statSync(absolute).isFile()) fail("participant snapshot is incomplete", relative);
    return `${relative}\0${sha256(fs.readFileSync(absolute))}\n`;
  }).join(""));
}

function overlayParticipant(recordValue) {
  for (const relative of [...new Set([...recordValue.task.participant_files, ...recordValue.task.distractor_files])]) {
    if (!safeRelative(relative)) fail("private task contains an unsafe path");
    const from = path.join(workspace, relative);
    const to = path.join(sourceRoot, relative);
    if (!inside(workspace, from) || !fs.existsSync(from) || !fs.statSync(from).isFile()) fail("participant snapshot is incomplete", relative);
    fs.mkdirSync(path.dirname(to), { recursive: true });
    fs.copyFileSync(from, to);
  }
}

function findExecutable(root, target) {
  const expected = process.platform === "win32" ? `${target}.exe`.toLowerCase() : target;
  const queue = [root];
  while (queue.length) {
    const directory = queue.shift();
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
      const absolute = path.join(directory, entry.name);
      if (entry.isDirectory()) queue.push(absolute);
      else if (entry.isFile() && entry.name.toLowerCase() === expected) return absolute;
    }
  }
  return null;
}

if (!/^TC-V31-FINAL-\d{3}$/.test(taskId) || !["public", "behavioral", "adversarial", "performance"].includes(suiteKind)) fail("invalid final evaluator request");
if (!fs.existsSync(workspace) || !fs.statSync(workspace).isDirectory() || !fs.existsSync(baseRoot) || !fs.existsSync(taskRecordPath)) fail("final evaluator inputs are unavailable");
if (inside(workspace, privateRoot) || inside(privateRoot, workspace)) fail("private grader state overlaps the participant workspace");
const record = JSON.parse(fs.readFileSync(taskRecordPath, "utf8"));
if (record.schema !== "treatcode.intelligence.private-task.v3.1" || record.task.task_id !== taskId) fail("private task record is invalid");
const testManifest = JSON.parse(fs.readFileSync(path.join(baseRoot, "TEST_MANIFEST.json"), "utf8"));
const knownTargets = new Set(testManifest.suites.flatMap((suite) => suite.targets || []));
if (record.task.focused_targets.some((target) => !knownTargets.has(target))) fail("private task requested a non-allowlisted target");

const sessionKey = sha256(path.resolve(workspace)).slice(0, 32);
const sessionRoot = path.join(os.tmpdir(), `treatcode-v31-final-${sessionKey}`);
const sourceRoot = path.join(sessionRoot, "source");
const buildRoot = path.join(sessionRoot, "build");
const ownerPath = path.join(sessionRoot, "owner.json");
const workspaceHash = sha256(path.resolve(workspace));
const contentHash = participantContentHash(record);
if (!fs.existsSync(sessionRoot)) {
  fs.mkdirSync(sessionRoot, { recursive: false });
  fs.cpSync(baseRoot, sourceRoot, { recursive: true, errorOnExist: true, force: false });
  overlayParticipant(record);
  fs.writeFileSync(ownerPath, `${JSON.stringify({ schema: "treatcode.intelligence.final-evaluator-session.v3.1", workspace_sha256: workspaceHash, workspace_content_sha256: contentHash, task_id: taskId })}\n`, { flag: "wx" });
} else {
  const owner = JSON.parse(fs.readFileSync(ownerPath, "utf8"));
  if (owner.workspace_sha256 !== workspaceHash || owner.task_id !== taskId) fail("evaluator session ownership mismatch");
  if (owner.workspace_content_sha256 !== contentHash) {
    overlayParticipant(record);
    fs.writeFileSync(ownerPath, `${JSON.stringify({ ...owner, workspace_content_sha256: contentHash })}\n`);
  }
}
if (!fs.existsSync(path.join(buildRoot, "CMakeCache.txt"))) {
  const configured = run("cmake", ["-S", sourceRoot, "-B", buildRoot], sourceRoot, 180_000);
  if (configured.status !== 0 || configured.error) fail("repository configuration failed", `${configured.stderr}\n${configured.stdout}`.slice(-3000));
}

const focused = [...new Set(record.task.focused_targets)];
const suiteTargets = [...new Set(Object.values(record.focused_suite_targets).flat())].filter((target) => knownTargets.has(target));
const publicTargets = focused.slice(0, 1);
const behavioralTargets = focused;
const adversarialTargets = [...new Set([...focused, ...suiteTargets])].slice(0, 8);
const performanceTargets = record.task.design_features.performance_constraint ? focused : focused.slice(-1);
const selected = suiteKind === "public" ? publicTargets : suiteKind === "behavioral" ? behavioralTargets : suiteKind === "adversarial" ? adversarialTargets : performanceTargets;
if (selected.length === 0) fail("final evaluator suite has no targets");
const started = Date.now();
try {
  const timeoutSeconds = suiteKind === "performance" ? 120 : 300;
  for (const target of selected) {
    const built = run("cmake", ["--build", buildRoot, "--target", target], sourceRoot, 300_000);
    if (built.status !== 0 || built.error) fail(`${suiteKind === "public" ? "public" : "hidden"} repository build failed`, `${built.stderr}\n${built.stdout}`.slice(-4000));
    const executable = findExecutable(buildRoot, target);
    if (!executable) fail(`${suiteKind === "public" ? "public" : "hidden"} repository executable is missing`, target);
    const executed = run(executable, [], sourceRoot, timeoutSeconds * 1000);
    if (executed.status !== 0 || executed.error) fail(`${suiteKind === "public" ? "public" : "hidden"} repository suite failed`, `${executed.stderr}\n${executed.stdout}`.slice(-4000));
  }
  console.log(JSON.stringify({ schema: "treatcode.intelligence.repository-command-result.v3.1", passed: true, suite: suiteKind === "public" ? "public" : "aggregate-hidden", targets_passed: selected.length, targets_total: selected.length, wall_clock_ms: Date.now() - started }));
} finally {
  if (suiteKind === "performance") fs.rmSync(sessionRoot, { recursive: true, force: true });
}
