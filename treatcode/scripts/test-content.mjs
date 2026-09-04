import fs from "node:fs";
import path from "node:path";
import {
  assert,
  loadCourse,
  internalLinks,
  referenceExists,
  relativeToRepo,
  repoRoot,
  treatcodeRoot,
  writeEvidence,
} from "./learning-test-utils.mjs";

const report = {
  schema: "trit.treatcode_learning_content_report.v2",
  ok: false,
  catalog: "treatcode/src/content/learn/learning-catalog.json",
  matrix: "treatcode/src/content/learn/P05_CURRICULUM_MATRIX.json",
  pages: [],
  checks: [],
  errors: [],
};

const requiredSections = [
  "Objectives",
  "Prerequisites",
  "Why this topic exists",
  "Explanation",
  "Worked example",
  "Common misconception",
  "Learner action",
  "Source links",
  "Validation",
  "Next step",
];

function validateInteractive(page) {
  const interactive = page.metadata.interactive;
  assert(interactive && typeof interactive === "object", `${page.metadata.id} needs an interactive module`);
  assert(["choice", "trace", "source", "code"].includes(interactive.kind), `${page.metadata.id} has an unsupported interactive kind`);
  if (interactive.kind === "choice") {
    assert(Array.isArray(interactive.options) && interactive.options.length >= 2, `${page.metadata.id} choice needs at least two options`);
    assert(Number.isInteger(interactive.answer) && interactive.answer >= 0 && interactive.answer < interactive.options.length, `${page.metadata.id} choice answer is invalid`);
  } else if (interactive.kind === "trace") {
    assert(Array.isArray(interactive.steps) && interactive.steps.length >= 2, `${page.metadata.id} trace needs at least two states`);
    assert(interactive.steps.every((step) => step.label && step.state && step.explanation), `${page.metadata.id} trace has an incomplete state`);
  } else if (interactive.kind === "source") {
    assert(typeof interactive.sourcePath === "string" && referenceExists(interactive.sourcePath), `${page.metadata.id} source investigation has no repository source`);
    assert(typeof interactive.sourceHref === "string" && /^https?:\/\//.test(interactive.sourceHref), `${page.metadata.id} source investigation has no stable source URL`);
    assert(Array.isArray(interactive.expectedIncludes) && interactive.expectedIncludes.length >= 1, `${page.metadata.id} source investigation has no observation target`);
  } else {
    assert(typeof interactive.exercise_id === "string" && interactive.exercise_id.length > 0, `${page.metadata.id} code module has no exercise_id`);
    assert(typeof interactive.runner === "string" && interactive.runner.length > 0, `${page.metadata.id} code module has no runner contract`);
    assert(typeof interactive.starter === "string" && /\bfn\s+/.test(interactive.starter), `${page.metadata.id} code module needs a function starter`);
    assert(Array.isArray(interactive.expectedIncludes) && interactive.expectedIncludes.length >= 1, `${page.metadata.id} code module has no executable-shape contract`);
    for (const fragment of interactive.expectedIncludes) assert(interactive.starter.includes(fragment), `${page.metadata.id} starter is missing ${fragment}`);
  }
}

try {
  const course = loadCourse();
  const { catalog, matrix, pages, pagesById } = course;
  assert(catalog.schema === "trit.treatcode_learning_catalog.v2", "catalog schema is incorrect");
  assert(matrix.schema === "trit.treatcode_curriculum_matrix.v1", "curriculum matrix schema is incorrect");
  assert(catalog.paths?.length === 3, "catalog must define beginner, programmer, and EECS paths");
  assert(Array.isArray(catalog.prerequisite_graph), "catalog is missing prerequisite_graph");
  assert(Array.isArray(catalog.glossary) && catalog.glossary.length >= 75, "catalog glossary must contain at least 75 terms");
  assert(pages.length >= 42, `catalog must publish at least 42 lessons, found ${pages.length}`);
  assert(new Set(matrix.phases.map((phase) => phase.phase_id)).size === matrix.phase_count, "matrix phase IDs are not unique");
  assert(matrix.lesson_count === pages.length, "matrix lesson count does not match Markdown pages");

  const interactiveKinds = new Set();
  for (const page of pages) {
    const meta = page.metadata;
    assert(typeof meta.id === "string" && meta.id.length > 0, `${relativeToRepo(page.filePath)} has no id`);
    assert(!pagesById.has(meta.id) || pagesById.get(meta.id) === page, `duplicate page id ${meta.id}`);
    assert(typeof meta.title === "string" && meta.title.length > 0, `${meta.id} has no title`);
    assert(["beginner", "programmer", "eecs"].includes(meta.level), `${meta.id} has an invalid level`);
    assert(typeof meta.phase_id === "string" && typeof meta.phase_slug === "string", `${meta.id} has no phase identity`);
    assert(Array.isArray(meta.objectives) && meta.objectives.length >= 3, `${meta.id} needs measurable objectives`);
    assert(Array.isArray(meta.prerequisites), `${meta.id} has no prerequisites array`);
    assert(Array.isArray(meta.sources) && meta.sources.length >= 2, `${meta.id} needs at least two authoritative sources`);
    assert(Array.isArray(meta.evidence) && meta.evidence.length >= 1, `${meta.id} needs test or validation evidence`);
    assert(Array.isArray(meta.canonical_terms) && meta.canonical_terms.length >= 3, `${meta.id} needs phase-linked canonical terms`);
    assert(meta.stack_links?.phase === `/stack/${meta.phase_slug}`, `${meta.id} has no bidirectional Stack Explorer phase link`);
    assert(meta.next === null || typeof meta.next === "string", `${meta.id} has an invalid next field`);
    assert(!/\b(TBD|TODO|lorem ipsum)\b/i.test(page.content), `${meta.id} contains a placeholder`);
    for (const section of requiredSections) assert(new RegExp(`^##\\s+${section}`, "im").test(page.content), `${meta.id} is missing the ${section} section`);

    for (const reference of [...meta.sources, ...meta.evidence]) {
      assert(reference.path && !path.isAbsolute(reference.path), `${meta.id} has a non-repository reference`);
      assert(referenceExists(reference.path), `${meta.id} references missing path ${reference.path}`);
    }
    for (const link of internalLinks(page.content)) {
      if (!/^(https?:|mailto:|\/|#)/.test(link)) assert(referenceExists(link.split("#")[0]), `${meta.id} has a broken repository link ${link}`);
    }
    validateInteractive(page);
    interactiveKinds.add(meta.interactive.kind);
    report.pages.push({ id: meta.id, file: relativeToRepo(page.filePath), phase_id: meta.phase_id, sources: meta.sources.length, evidence: meta.evidence.length, interactive: meta.interactive.kind });
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
    "applications-and-validation",
  ];
  for (const id of requiredPages) assert(pagesById.has(id), `missing required learning page ${id}`);
  assert(["choice", "trace", "source", "code"].every((kind) => interactiveKinds.has(kind)), "all four required interaction types are not published");

  for (const learningPath of catalog.paths) {
    assert(learningPath.page_ids.length >= 42, `${learningPath.id} path is incomplete`);
    for (const pageId of learningPath.page_ids) assert(pagesById.has(pageId), `${learningPath.id} references missing page ${pageId}`);
    assert(learningPath.terminal_lesson_id === "full-system-validation", `${learningPath.id} does not name the closure lesson`);
  }

  for (const edge of catalog.prerequisite_graph) {
    assert(pagesById.has(edge.from) && pagesById.has(edge.to), "prerequisite edge references a missing page");
    assert(pagesById.get(edge.to).metadata.prerequisites.includes(edge.from), `prerequisite edge ${edge.from}->${edge.to} is not reflected by page metadata`);
  }

  const beginner = catalog.paths.find((item) => item.id === "beginner");
  assert(beginner.page_ids[0] === "representation-boundaries", "beginner path must start at representation");
  assert(beginner.page_ids.at(-1) === "full-system-validation", "beginner path must continue through closure");
  assert(pagesById.get("first-tcl-program").metadata.interactive.kind === "code", "first TCL page must expose a code module");
  assert(pagesById.get("representation-boundaries").content.toLowerCase().includes("numeric") && pagesById.get("representation-boundaries").content.toLowerCase().includes("lane"), "representation page must distinguish numeric and lane meanings");
  assert(pagesById.get("representation-boundaries").content.toLowerCase().includes("hardware"), "representation page must mention hardware-facing meaning");

  const appSource = fs.readFileSync(path.join(treatcodeRoot, "src", "App.tsx"), "utf8");
  const publicAppSource = fs.readFileSync(path.join(treatcodeRoot, "src", "PublicApp.tsx"), "utf8");
  assert(!appSource.includes("guideContent"), "published Guide still depends on guideContent.ts");
  assert(publicAppSource.includes("LEARNING_CATALOG") && publicAppSource.includes("MarkdownLearnPage"), "public /learn route is not backed by the learning catalog");
  report.checks.push("v2 front matter, objectives, required sections, provenance, and repository references");
  report.checks.push("21-phase matrix and 42-lesson publication");
  report.checks.push("ordered paths, prerequisite graph, and 75+ term glossary");
  report.checks.push("choice, trace, source investigation, and bounded code interactions");
  report.checks.push("legacy guideContent dependency removed and public /learn consumes generated pages");
  report.ok = true;
} catch (error) {
  report.errors.push(String(error?.message || error));
}

writeEvidence("content-validation.json", report, [path.join(repoRoot, "build", "treatcode-plan-evidence", "P05"), path.join(repoRoot, "build", "treatcode-plan-evidence", "P16")]);
console.log(`TreatCode learning content validation: ${report.ok ? "passed" : "failed"}`);
for (const check of report.checks) console.log(`  [ok] ${check}`);
for (const error of report.errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
