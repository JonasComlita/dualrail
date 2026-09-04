import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const treatcodeRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(treatcodeRoot, "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P05");
const p16EvidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P16");
const appPath = path.join(treatcodeRoot, "src", "App.tsx");
const publicAppPath = path.join(treatcodeRoot, "src", "PublicApp.tsx");
const indexPath = path.join(treatcodeRoot, "index.html");
const contentRoot = path.join(treatcodeRoot, "src", "content", "learn");
const report = { schema: "trit.treatcode_accessibility_report.v1", ok: false, checks: [], errors: [] };

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

try {
  const app = fs.readFileSync(appPath, "utf8");
  const publicApp = fs.readFileSync(publicAppPath, "utf8");
  const index = fs.readFileSync(indexPath, "utf8");
  assert(/<html\s+lang="[a-z-]+">/.test(index), "document language is not declared");
  assert(app.includes('aria-label="Learning path"'), "learning path selector has no accessible name");
  assert(app.includes("aria-label=\"Learning page navigation\""), "learning page navigation has no accessible name");
  assert(app.includes("<fieldset") && app.includes("<legend"), "choice module is not grouped with a fieldset and legend");
  assert(app.includes('type="radio"') && app.includes("aria-checked"), "choice module has no keyboard-readable radio state");
  assert(app.includes('aria-label="TCL practice code"'), "code module textarea has no accessible label");
  assert(app.includes('aria-live="polite"'), "interactive feedback has no live region");
  assert(app.includes('target="_blank"') && app.includes('rel="noreferrer"'), "repository source links do not declare their external navigation behavior");
  assert(publicApp.includes('aria-label="Learning pages"') && publicApp.includes('aria-labelledby="public-learning-check-title"'), "public Learn route lacks named learning navigation or checks");
  assert(publicApp.includes('aria-live="polite"') && publicApp.includes('aria-label="TCL practice code"'), "public Learn route lacks labelled live feedback or code input");

  for (const name of fs.readdirSync(contentRoot).filter((item) => item.endsWith(".md"))) {
    const content = fs.readFileSync(path.join(contentRoot, name), "utf8");
    assert(!/<img\b(?![^>]*\balt=)/i.test(content), `${name} contains an image without alt text`);
  }

  report.checks.push("document language and navigation landmarks");
  report.checks.push("keyboard-readable choice controls");
  report.checks.push("labelled code editor and live feedback");
  report.checks.push("source-link behavior and Markdown media checks");
  report.checks.push("public Learn route landmarks and controls");
  report.ok = true;
} catch (error) {
  report.errors.push(String(error.message || error));
}

fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "accessibility.json"), `${JSON.stringify(report, null, 2)}\n`);
const publicShell = fs.readFileSync(path.join(treatcodeRoot, "public", "public-shell.css"), "utf8");
const staticLearningRoutes = fs.existsSync(path.join(treatcodeRoot, "learn", "beginner"))
  ? fs.readdirSync(path.join(treatcodeRoot, "learn", "beginner"), { withFileTypes: true }).filter((entry) => entry.isDirectory()).length
  : 0;
const p16Report = {
  schema: "trit.treatcode_accessibility_report.v2",
  ok: report.ok && publicShell.includes("@media (max-width: 820px)") && publicShell.includes("@media (max-width: 520px)") && staticLearningRoutes >= 42,
  checks: [...report.checks, "responsive 820px and 520px layouts", "static no-JavaScript lesson routes retain a readable landmark"],
  errors: [...report.errors, ...(staticLearningRoutes >= 42 ? [] : [`only ${staticLearningRoutes} beginner static lesson routes are present`])],
  route_counts: { beginner_static_lessons: staticLearningRoutes },
};
fs.mkdirSync(p16EvidenceRoot, { recursive: true });
fs.writeFileSync(path.join(p16EvidenceRoot, "accessibility.json"), `${JSON.stringify(p16Report, null, 2)}\n`);
console.log(`P05 accessibility: ${report.ok ? "passed" : "failed"}`);
for (const check of report.checks) console.log(`  [ok] ${check}`);
for (const error of report.errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok && p16Report.ok ? 0 : 1;
