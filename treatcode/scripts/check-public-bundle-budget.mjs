import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P04");
const p15EvidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P15");
const budgetBytes = 100 * 1024;
const checks = [];
const errors = [];

function assert(condition, message) { if (!condition) throw new Error(message); }
function assetPath(url) { return path.join(appRoot, "dist", url.replace(/^\//, "")); }
function initialScripts(html) {
  const urls = [...html.matchAll(/<(?:script|link)[^>]+(?:src|href)="([^"]+)"[^>]*>/g)]
    .map((match) => match[1])
    .filter((url) => url.endsWith(".js"));
  return [...new Set(urls)];
}

try {
  for (const route of ["index.html", "stack/index.html", "learn/index.html"]) {
    const htmlPath = path.join(appRoot, "dist", route);
    assert(fs.existsSync(htmlPath), `built route is missing: ${route}`);
    const html = fs.readFileSync(htmlPath, "utf8");
    const urls = initialScripts(html);
    assert(urls.length > 0, `${route} has no JavaScript entry`);
    const contents = urls.map((url) => {
      const file = assetPath(url);
      assert(fs.existsSync(file), `${route} references missing asset ${url}`);
      return fs.readFileSync(file);
    });
    const compressedBytes = contents.reduce((sum, content) => sum + zlib.gzipSync(content, { level: 9 }).byteLength, 0);
    assert(compressedBytes <= budgetBytes, `${route} initial JavaScript is ${compressedBytes} bytes gzip, over the ${budgetBytes} byte budget`);
    const initialCode = Buffer.concat(contents).toString("utf8");
    assert(!initialCode.includes("compileAndRunTrit") && !initialCode.includes("STARTER_CODES"), `${route} initial JavaScript contains the editor/runner bundle`);
    checks.push(`${route}: ${compressedBytes} bytes gzip initial JavaScript`);
  }
} catch (error) {
  errors.push(String(error?.message || error));
}

const report = { schema: "treatcode.public_bundle_budget.v1", ok: errors.length === 0, budget_bytes_gzip: budgetBytes, checks, errors };
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "bundle-budget.json"), `${JSON.stringify(report, null, 2)}\n`);
fs.mkdirSync(p15EvidenceRoot, { recursive: true });
fs.writeFileSync(path.join(p15EvidenceRoot, "bundle-budget.json"), `${JSON.stringify({ ...report, schema: "treatcode.public.bundle-budget.v1", routes: ["/", "/stack", "/learn", "/resources/*", "/evidence", "/search"] }, null, 2)}\n`);
const requiredEvidence = ["stack-coverage.json", "resource-coverage.json", "no-truncation.json", "search-coverage.json", "provenance-audit.json", "static-route-e2e.json", "public-browser-e2e.json", "accessibility.json", "security.json", "bundle-budget.json"];
const evidenceStatus = Object.fromEntries(requiredEvidence.map((name) => {
  const file = path.join(p15EvidenceRoot, name);
  if (!fs.existsSync(file)) return [name, { present: false, ok: false }];
  try { const value = JSON.parse(fs.readFileSync(file, "utf8")); return [name, { present: true, ok: value.ok === true }]; } catch { return [name, { present: true, ok: false }]; }
}));
const p15Result = {
  schema: "treatcode.public.plan-result.v1",
  plan_id: "P15",
  status: "in_progress",
  complete: false,
  completion_blocked_until: ["complete evidence review", "Product owner approval", "Architecture owner approval", "Accessibility reviewer approval"],
  command_order: [
    "python tools/trit_tool.py knowledge status",
    "npm.cmd --prefix treatcode run build",
    "npm.cmd --prefix treatcode run test:api",
    "npm.cmd --prefix treatcode run test:public-coverage",
    "npm.cmd --prefix treatcode run test:e2e:public-complete",
    "npm.cmd --prefix treatcode run test:a11y",
    "npm.cmd --prefix treatcode run test:public-security",
    "npm.cmd --prefix treatcode run check:bundle-budget",
    "python tools/trit_tool.py website plans validate",
    "python tools/trit_tool.py website plan verify P15",
  ],
  evidence: evidenceStatus,
  human_approvals: { "Product owner": null, "Architecture owner": null, "Accessibility reviewer": null },
  note: "Automated implementation evidence is not a substitute for the named review artifacts; P15 remains in_progress until they are attached.",
};
fs.writeFileSync(path.join(p15EvidenceRoot, "result.json"), `${JSON.stringify(p15Result, null, 2)}\n`);
console.log(`P04 bundle budget: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
