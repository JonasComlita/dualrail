import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const treatcodeRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(treatcodeRoot, "..");
const catalogPath = path.join(treatcodeRoot, "src", "content", "learn", "learning-catalog.json");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P05");
const report = { schema: "trit.treatcode_learning_flow_report.v1", ok: false, checks: [], errors: [] };

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

try {
  const buildCommand = process.platform === "win32" ? (process.env.ComSpec || "cmd.exe") : "npm";
  const buildArguments = process.platform === "win32"
    ? ["/d", "/s", "/c", "npm run build --silent"]
    : ["run", "build", "--silent"];
  const build = spawnSync(buildCommand, buildArguments, {
    cwd: treatcodeRoot,
    encoding: "utf8",
    stdio: "pipe"
  });
  assert(build.status === 0, `TreatCode build failed (status ${build.status}, ${build.error?.message || "no process error"}):\n${build.stdout || ""}\n${build.stderr || ""}`);

  const catalog = JSON.parse(fs.readFileSync(catalogPath, "utf8"));
  const indexHtml = fs.readFileSync(path.join(treatcodeRoot, "dist", "index.html"), "utf8");
  const assetDir = path.join(treatcodeRoot, "dist", "assets");
  const bundles = fs.readdirSync(assetDir).filter((name) => name.endsWith(".js")).map((name) => fs.readFileSync(path.join(assetDir, name), "utf8"));
  const bundle = bundles.join("\n");

  assert(indexHtml.includes('<div id="root"'), "built app has no root mount");
  assert(bundle.includes("Beginner path") && bundle.includes("Programmer path") && bundle.includes("EECS/systems"), "built app does not contain all learning paths");
  assert(bundle.includes("Representation boundaries") && bundle.includes("Applications and validation"), "built public Learn route does not contain the Markdown page catalog");
  assert(bundle.includes("Source and evidence") && bundle.includes("Production source"), "built public Learn route does not contain provenance panels");
  assert(bundle.includes("Try the first function") && bundle.includes("Open challenges"), "built app does not contain the first-program interaction");
  assert(bundle.includes("Boundary check") && bundle.includes("Decode check") && bundle.includes("Release check"), "built app does not contain the learning checks");
  assert(bundle.includes("TCL 1.0 language specification") && bundle.includes("SYSCALL_MANIFEST.json"), "built app does not retain provenance labels");
  for (const learningPath of catalog.paths) {
    assert(learningPath.page_ids.length >= 3, `${learningPath.id} path is not navigable`);
  }
  assert(bundle.includes("aria-label=\"Learning path\"") || bundle.includes("Learning path"), "built app has no named learning path control");

  report.checks.push("production build completes");
  report.checks.push("all three ordered learning paths are bundled");
  report.checks.push("first TCL code interaction is bundled");
  report.checks.push("interactive checks and provenance labels are bundled");
  report.ok = true;
} catch (error) {
  report.errors.push(String(error.message || error));
}

fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "learning-flow.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(`P05 learning flow: ${report.ok ? "passed" : "failed"}`);
for (const check of report.checks) console.log(`  [ok] ${check}`);
for (const error of report.errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
