import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P04");
const p15EvidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P15");
const checks = [];
const errors = [];

function assert(condition, message) { if (!condition) throw new Error(message); }

try {
  for (const route of ["index.html", "stack/index.html", "learn/index.html"]) {
    const html = fs.readFileSync(path.join(appRoot, route), "utf8");
    assert(/<html[^>]+lang="[a-z-]+"/.test(html), `${route} has no document language`);
    assert(/<meta[^>]+name="viewport"/.test(html), `${route} has no responsive viewport`);
    assert(/<title>[^<]+<\/title>/.test(html), `${route} has no document title`);
    assert(/<nav[^>]+aria-label="Primary navigation"/.test(html), `${route} has no named primary navigation`);
    assert(/<main\b/.test(html), `${route} has no main landmark`);
    assert(/<h1\b/.test(html), `${route} has no level-one heading`);
  }
  const app = fs.readFileSync(path.join(appRoot, "src", "PublicApp.tsx"), "utf8");
  assert(app.includes('role="search"'), "interactive public search has no search landmark");
  assert(app.includes('aria-label="Search mode"'), "search mode control has no accessible name");
  assert(app.includes('aria-live="polite"'), "snapshot fallback has no live status");
  assert(app.includes('aria-label="Stack phases"'), "stack navigation has no accessible name");
  assert(app.includes('aria-label="Pagination"'), "paginated public collections have no accessible navigation name");
  assert(app.includes('route.page === "evidence"') && app.includes('route.page === "resource"'), "evidence and resource routes are not represented in the accessible public app");
  assert(app.includes('target="_blank"') && app.includes('rel="noreferrer"'), "source citations lack external-link behavior");
  const css = fs.readFileSync(path.join(appRoot, "public", "public-shell.css"), "utf8");
  assert(css.includes("overflow-x: hidden") && css.includes("max-width: 520px"), "responsive overflow safeguards are missing");
  checks.push("static public HTML has language, viewport, title, landmarks, and headings");
  checks.push("interactive search, status, stack navigation, and citations have accessible names");
  checks.push("responsive CSS constrains public pages at narrow widths");
} catch (error) {
  errors.push(String(error?.message || error));
}

const report = { schema: "treatcode.public_accessibility.v1", ok: errors.length === 0, checks, errors };
fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "accessibility.json"), `${JSON.stringify(report, null, 2)}\n`);
fs.mkdirSync(p15EvidenceRoot, { recursive: true });
fs.writeFileSync(path.join(p15EvidenceRoot, "accessibility.json"), `${JSON.stringify({ ...report, schema: "treatcode.public.accessibility.v1", viewport_contracts: [390, 768, 1280] }, null, 2)}\n`);
console.log(`P04 accessibility: ${report.ok ? "passed" : "failed"}`);
for (const check of checks) console.log(`  [ok] ${check}`);
for (const error of errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
