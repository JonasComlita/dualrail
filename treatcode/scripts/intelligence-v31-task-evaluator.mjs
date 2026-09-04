import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";

const argument = (name) => process.argv.find((value) => value.startsWith(`--${name}=`))?.slice(name.length + 3);
const workspace = path.resolve(argument("workspace") || "");
const casesPath = path.resolve(argument("cases") || "");
const tritc = path.resolve(argument("tritc") || "");
const visibility = argument("visibility") === "private" ? "private" : "public";
const SAFE_ID = /^[A-Za-z_][A-Za-z0-9_]*$/;

function fail(message, detail = "") {
  console.error(visibility === "public" && detail ? `${message}: ${detail}` : message);
  process.exit(1);
}

function inside(root, candidate) {
  const relative = path.relative(path.resolve(root), path.resolve(candidate));
  return relative === "" || (relative !== ".." && !relative.startsWith(`..${path.sep}`) && !path.isAbsolute(relative));
}

function safeRelative(value) {
  if (typeof value !== "string" || !value || path.isAbsolute(value)) return false;
  const segments = value.replaceAll("\\", "/").split("/");
  return segments.every((segment) => segment && segment !== "." && segment !== "..");
}

if (!fs.statSync(workspace).isDirectory() || !fs.statSync(casesPath).isFile() || !fs.statSync(tritc).isFile()) fail("evaluator paths are unavailable");
if (visibility === "public" && !inside(workspace, casesPath)) fail("public cases must be inside the participant workspace");
if (visibility === "private" && inside(workspace, casesPath)) fail("private cases must be outside the participant workspace");

const suite = JSON.parse(fs.readFileSync(casesPath, "utf8"));
if (suite.schema !== "treatcode.intelligence.repository-cases.v3.1" || suite.version !== "3.1" || !Array.isArray(suite.trit_files) || suite.trit_files.length < 2 || !Array.isArray(suite.cases) || suite.cases.length === 0) fail("case bundle is invalid");
const tritFiles = suite.trit_files.map((relative) => {
  if (!safeRelative(relative)) fail("case bundle contains an unsafe Trit path");
  const absolute = path.resolve(workspace, relative);
  if (!inside(workspace, absolute) || !fs.statSync(absolute).isFile() || !relative.endsWith(".trit")) fail("case bundle references an invalid Trit file");
  return absolute;
});

for (const assertion of suite.file_assertions || []) {
  if (!safeRelative(assertion.path) || typeof assertion.contains !== "string" || assertion.contains.length === 0) fail("case bundle contains an invalid file assertion");
  const absolute = path.resolve(workspace, assertion.path);
  if (!inside(workspace, absolute) || !fs.statSync(absolute).isFile()) fail("file assertion escaped the participant workspace");
  if (!fs.readFileSync(absolute, "utf8").includes(assertion.contains)) fail("adjacent repository contract failed", assertion.path);
}

const harnessLines = ["fn main() -> t40 {"];
for (let index = 0; index < suite.cases.length; index += 1) {
  const item = suite.cases[index];
  if (!SAFE_ID.test(item.function) || !Array.isArray(item.args) || item.args.length > 6 || !item.args.every(Number.isSafeInteger) || !Number.isSafeInteger(item.expected)) fail("case bundle contains an invalid invocation");
  harnessLines.push(`    if ${item.function}(${item.args.join(", ")}) != ${item.expected} { return -${index + 1}; }`);
}
harnessLines.push("    return 1;", "}", "");
const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), "treatcode-v31-harness-"));
const harnessPath = path.join(tempRoot, "harness.trit");
fs.writeFileSync(harnessPath, harnessLines.join("\n"));
try {
  const result = spawnSync(tritc, ["run", ...tritFiles, harnessPath, "--no-ansi", "--steps", String(suite.step_limit || 500000)], {
    cwd: workspace,
    env: process.env,
    encoding: "utf8",
    shell: false,
    windowsHide: true,
    timeout: 90_000,
    maxBuffer: 4 * 1_048_576,
  });
  const output = `${result.stdout || ""}\n${result.stderr || ""}`;
  const returnMatch = output.match(/Return Register r13:\s*(-?\d+)/);
  const cycleMatch = output.match(/Total CPU Cycles:\s*(\d+)/);
  const passed = result.status === 0 && !result.error && output.includes("Final CPU Status:   HALTED") && returnMatch?.[1] === "1";
  if (!passed) fail("Trit behavioral suite failed", output.slice(-1500));
  const cycles = Number(cycleMatch?.[1] || 0);
  if (suite.maximum_cycles && (!cycles || cycles > suite.maximum_cycles)) fail("Trit performance budget failed", `${cycles} cycles exceeds ${suite.maximum_cycles}`);
  console.log(JSON.stringify({ schema: "treatcode.intelligence.repository-command-result.v3.1", passed: true, suite: visibility === "public" ? suite.suite : "aggregate-hidden", cases_passed: suite.cases.length, cases_total: suite.cases.length, cycles }));
} finally {
  fs.rmSync(tempRoot, { recursive: true, force: true });
}
