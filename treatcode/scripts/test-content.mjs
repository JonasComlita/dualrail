import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const treatcodeRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(treatcodeRoot, "..");
const contentRoot = path.join(treatcodeRoot, "src", "content", "learn");
const catalogPath = path.join(contentRoot, "learning-catalog.json");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P05");

function relative(filePath) {
  return path.relative(repoRoot, filePath).replaceAll(path.sep, "/");
}

function parsePage(filePath) {
  const source = fs.readFileSync(filePath, "utf8");
  const match = source.match(/^---\r?\n([\s\S]*?)\r?\n---\r?\n([\s\S]*)$/);
  if (!match) throw new Error(`${relative(filePath)} is missing JSON front matter`);
  let metadata;
  try {
    metadata = JSON.parse(match[1]);
  } catch (error) {
    throw new Error(`${relative(filePath)} has invalid JSON front matter: ${error.message}`);
  }
  return { filePath, metadata, content: match[2].trim() };
}

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const report = {
  schema: "trit.treatcode_learning_content_report.v1",
  ok: false,
  catalog: relative(catalogPath),
  pages: [],
  checks: [],
  errors: []
};

try {
  const catalog = JSON.parse(fs.readFileSync(catalogPath, "utf8"));
  assert(catalog.schema === "trit.treatcode_learning_catalog.v1", "catalog schema is incorrect");
  assert(Array.isArray(catalog.paths) && catalog.paths.length === 3, "catalog must define beginner, programmer, and EECS paths");
  assert(Array.isArray(catalog.prerequisite_graph), "catalog is missing prerequisite_graph");
  assert(Array.isArray(catalog.glossary) && catalog.glossary.length >= 10, "catalog glossary is too small");

  const pageFiles = fs.readdirSync(contentRoot)
    .filter((name) => name.endsWith(".md"))
    .sort()
    .map((name) => parsePage(path.join(contentRoot, name)));
  const pagesById = new Map();

  for (const page of pageFiles) {
    const meta = page.metadata;
    assert(typeof meta.id === "string" && meta.id.length > 0, `${relative(page.filePath)} has no id`);
    assert(!pagesById.has(meta.id), `duplicate page id ${meta.id}`);
    assert(typeof meta.title === "string" && meta.title.length > 0, `${meta.id} has no title`);
    assert(["beginner", "programmer", "eecs"].includes(meta.level), `${meta.id} has an invalid level`);
    assert(Array.isArray(meta.prerequisites), `${meta.id} has no prerequisites array`);
    assert(Array.isArray(meta.sources) && meta.sources.length >= 2, `${meta.id} needs at least two authoritative sources`);
    assert(Array.isArray(meta.evidence) && meta.evidence.length >= 1, `${meta.id} needs test or validation evidence`);
    assert(meta.interactive && ["choice", "code"].includes(meta.interactive.kind), `${meta.id} needs an interactive module`);
    assert(/^##\s+Prerequisites/m.test(page.content), `${meta.id} is missing a prerequisites section`);
    assert(meta.next === null || typeof meta.next === "string", `${meta.id} has an invalid next field`);
    assert(!/\b(TBD|TODO|lorem ipsum)\b/i.test(page.content), `${meta.id} contains a placeholder`);

    for (const reference of [...meta.sources, ...meta.evidence]) {
      assert(reference.path && !path.isAbsolute(reference.path), `${meta.id} has a non-repository reference`);
      const referencePath = path.join(repoRoot, reference.path);
      assert(fs.existsSync(referencePath), `${meta.id} references missing path ${reference.path}`);
    }

    const links = [...page.content.matchAll(/\[[^\]]+\]\(([^)]+)\)/g)].map((match) => match[1]);
    for (const link of links) {
      if (!/^(https?:|mailto:|#)/.test(link)) {
        assert(fs.existsSync(path.join(repoRoot, link.split("#")[0])), `${meta.id} has a broken link ${link}`);
      }
    }

    if (meta.interactive.kind === "choice") {
      assert(meta.interactive.options.length >= 2, `${meta.id} choice needs at least two options`);
      assert(Number.isInteger(meta.interactive.answer) && meta.interactive.answer >= 0 && meta.interactive.answer < meta.interactive.options.length, `${meta.id} choice answer is invalid`);
    } else {
      assert(meta.interactive.starter.includes("fn "), `${meta.id} code module needs a function starter`);
      for (const fragment of meta.interactive.expectedIncludes) {
        assert(meta.interactive.starter.includes(fragment), `${meta.id} starter is missing ${fragment}`);
      }
    }

    pagesById.set(meta.id, page);
    report.pages.push({ id: meta.id, file: relative(page.filePath), sources: meta.sources.length, evidence: meta.evidence.length, interactive: meta.interactive.kind });
  }

  const requiredPages = [
    "representation-boundaries",
    "tritwise-operations",
    "first-tcl-program",
    "isa-vm-execution",
    "compiler-pipeline",
    "boot-and-traps",
    "kernel-syscalls",
    "storage-and-images",
    "applications-and-validation"
  ];
  for (const id of requiredPages) assert(pagesById.has(id), `missing required learning page ${id}`);

  for (const learningPath of catalog.paths) {
    assert(learningPath.page_ids.length >= 3, `${learningPath.id} path is too short`);
    for (const pageId of learningPath.page_ids) assert(pagesById.has(pageId), `${learningPath.id} references missing page ${pageId}`);
  }

  for (const edge of catalog.prerequisite_graph) {
    assert(pagesById.has(edge.from) && pagesById.has(edge.to), `prerequisite edge references a missing page`);
    assert(pagesById.get(edge.to).metadata.prerequisites.includes(edge.from), `prerequisite edge ${edge.from}->${edge.to} is not reflected by page metadata`);
  }

  const beginner = catalog.paths.find((item) => item.id === "beginner");
  assert(beginner.page_ids.at(-1) === "first-tcl-program", "beginner path must end at the first TCL program");
  assert(pagesById.get("first-tcl-program").metadata.interactive.kind === "code", "first TCL page must expose a code module");
  assert(pagesById.get("representation-boundaries").content.toLowerCase().includes("numeric") && pagesById.get("representation-boundaries").content.toLowerCase().includes("lane"), "representation page must distinguish numeric and lane meanings");
  assert(pagesById.get("representation-boundaries").content.toLowerCase().includes("hardware"), "representation page must mention hardware-facing meaning");

  const appSource = fs.readFileSync(path.join(treatcodeRoot, "src", "App.tsx"), "utf8");
  const publicAppSource = fs.readFileSync(path.join(treatcodeRoot, "src", "PublicApp.tsx"), "utf8");
  assert(!appSource.includes("guideContent"), "published Guide still depends on guideContent.ts");
  assert(publicAppSource.includes("LEARNING_CATALOG") && publicAppSource.includes("MarkdownLearnPage"), "public /learn route is not backed by the learning catalog");
  report.checks.push("front matter, provenance, and repository references");
  report.checks.push("ordered paths, prerequisite graph, and glossary");
  report.checks.push("interactive modules and code-example fragments");
  report.checks.push("legacy guideContent dependency removed");
  report.checks.push("public /learn route consumes Markdown learning pages");
  report.ok = true;
} catch (error) {
  report.errors.push(String(error.message || error));
}

fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "content-validation.json"), `${JSON.stringify(report, null, 2)}\n`);
console.log(`P05 content validation: ${report.ok ? "passed" : "failed"}`);
for (const check of report.checks) console.log(`  [ok] ${check}`);
for (const error of report.errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
