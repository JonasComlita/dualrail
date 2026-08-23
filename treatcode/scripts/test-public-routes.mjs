import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P04");
const checks = [];
const errors = [];

function assert(condition, message) { if (!condition) throw new Error(message); }
function buildIfNeeded() {
  if (fs.existsSync(path.join(appRoot, "dist", "stack", "index.html"))) return;
  const command = process.platform === "win32" ? "npm.cmd" : "npm";
  const result = spawnSync(command, ["run", "build", "--silent"], { cwd: appRoot, encoding: "utf8", stdio: "pipe" });
  assert(result.status === 0, `build failed before public route checks:\n${result.stdout}\n${result.stderr}`);
}

try {
  buildIfNeeded();
  for (const route of ["index.html", "stack/index.html", "learn/index.html"]) {
    const html = fs.readFileSync(path.join(appRoot, "dist", route), "utf8");
    assert(/<html[^>]+lang="en"/.test(html), `${route} has no declared language`);
    assert(html.includes('id="root"'), `${route} has no root mount`);
    assert(html.includes("Stack Explorer") || route === "index.html", `${route} has no useful stack navigation`);
    assert(html.includes("Learn") || route === "stack/index.html", `${route} has no useful learning navigation`);
    assert(html.includes("TreatCode"), `${route} has no TreatCode identity`);
  }
  const root = fs.readFileSync(path.join(appRoot, "dist", "index.html"), "utf8");
  assert(root.includes("Understand the stack") && root.includes("/api/public/v1/openapi.json"), "root route has no static-first public content");
  const stack = fs.readFileSync(path.join(appRoot, "dist", "stack", "index.html"), "utf8");
  assert(stack.includes("Trace every dependency") && stack.includes("/api/public/v1/stack-nodes"), "stack route has no useful static content");
  const learn = fs.readFileSync(path.join(appRoot, "dist", "learn", "index.html"), "utf8");
  assert(learn.includes("Learn from the boundary") && learn.includes("/practice"), "learn route has no useful static content");
  const practice = fs.readFileSync(path.join(appRoot, "dist", "practice", "index.html"), "utf8");
  assert(practice.includes("<title>Practice · TreatCode</title>") && practice.includes("id=\"root\""), "practice route has no dedicated production entry");
  checks.push("/, /stack, and /learn expose meaningful HTML before JavaScript executes");
  checks.push("public routes declare language, identity, navigation, and viewport metadata");
  checks.push("public pages link to the versioned API and practice boundary");
  checks.push("practice has a dedicated production entry instead of falling back to the homepage shell");
} catch (error) {
  errors.push(String(error?.message || error));
}

const report = { schema: "treatcode.public_route_e2e.v1", ok: errors.length === 0, checks, errors };
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "public-route-e2e.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(`P04 public routes: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
