import {
  assert,
  loadCourse,
  referenceExists,
  writeEvidence,
} from "./learning-test-utils.mjs";

const report = {
  schema: "trit.treatcode_curriculum_coverage.v1",
  ok: false,
  counts: {},
  checks: [],
  errors: [],
};

function registryMap(records) {
  return new Map(records.map((record) => [record.id, record]));
}

function assertAcyclic(pagesById) {
  const visiting = new Set();
  const visited = new Set();
  const visit = (id) => {
    if (visiting.has(id)) throw new Error(`prerequisite cycle includes ${id}`);
    if (visited.has(id)) return;
    visiting.add(id);
    for (const prerequisite of pagesById.get(id)?.metadata.prerequisites || []) visit(prerequisite);
    visiting.delete(id);
    visited.add(id);
  };
  for (const id of pagesById.keys()) visit(id);
}

try {
  const course = loadCourse();
  const { catalog, matrix, pages, pagesById, stackNodes, tests, benchmarks, gaps } = course;
  const testsById = registryMap(tests);
  const benchmarksById = registryMap(benchmarks);
  const gapsById = registryMap(gaps);
  const nodesById = registryMap(stackNodes);
  const pageIds = new Set(pages.map((page) => page.metadata.id));
  const matrixLessons = matrix.phases.flatMap((phase) => phase.lessons);
  const matrixLessonIds = new Set(matrixLessons.map((lesson) => lesson.id));

  assert(matrix.generated_from === "treatcode/public/api/v1/stack_nodes.json", "matrix is not generated from the current stack registry");
  assert(matrix.phase_count === stackNodes.length, `matrix has ${matrix.phase_count} phases but stack registry has ${stackNodes.length}`);
  assert(matrix.lesson_count === matrixLessons.length && matrixLessons.length === pages.length, "matrix, lesson rows, and Markdown pages disagree");
  assert(matrix.phases.length === stackNodes.length, "matrix phase rows do not cover every stack node");
  assert(new Set(matrixLessons.map((lesson) => lesson.id)).size === matrixLessons.length, "matrix lesson IDs are not unique");

  for (const phase of matrix.phases) {
    const node = nodesById.get(phase.phase_id);
    assert(node, `matrix phase ${phase.phase_id} is missing from stack registry`);
    assert(phase.ordinal === node.ordinal && phase.slug === node.slug && phase.name === node.name, `matrix phase ${phase.phase_id} does not preserve registry identity`);
    assert(phase.lessons.length >= 2, `${phase.slug} has fewer than two substantive lessons`);
    assert(phase.canonical_lesson_ids.length === phase.lessons.length, `${phase.slug} canonical lesson list is incomplete`);
    assert(phase.source_paths.length >= 2, `${phase.slug} has fewer than two source paths`);
    for (const sourcePath of phase.source_paths) assert(referenceExists(sourcePath), `${phase.slug} references missing source ${sourcePath}`);
    for (const testId of phase.test_ids) assert(testsById.has(testId), `${phase.slug} references unknown test ${testId}`);
    for (const benchmarkId of phase.benchmark_ids) assert(benchmarksById.has(benchmarkId), `${phase.slug} references unknown benchmark ${benchmarkId}`);
    for (const gapId of phase.gap_ids) assert(gapsById.has(gapId), `${phase.slug} references unknown gap ${gapId}`);
    for (const lesson of phase.lessons) {
      const page = pagesById.get(lesson.id);
      assert(page, `${phase.slug} matrix lesson ${lesson.id} has no Markdown page`);
      assert(page.metadata.phase_id === phase.phase_id && page.metadata.phase_slug === phase.slug, `${lesson.id} points at the wrong phase`);
      assert(lesson.stack_phase_id === phase.phase_id, `${lesson.id} has no stack phase edge`);
      assert(lesson.sources.length >= 2 && lesson.evidence.length >= 1, `${lesson.id} has incomplete provenance`);
      assert(lesson.test_ids.every((testId) => testsById.has(testId)), `${lesson.id} has an unknown test ID`);
      assert(lesson.benchmark_ids.every((benchmarkId) => benchmarksById.has(benchmarkId)), `${lesson.id} has an unknown benchmark ID`);
      assert(lesson.gap_ids.every((gapId) => gapsById.has(gapId)), `${lesson.id} has an unknown gap ID`);
      assert(page.metadata.stack_links?.phase === `/stack/${phase.slug}`, `${lesson.id} is not bidirectionally linked to Stack Explorer`);
    }
  }

  assert(matrix.glossary_count >= 75 && catalog.glossary.length === matrix.glossary_count, "glossary does not meet the 75-term requirement");
  for (const entry of catalog.glossary) {
    assert(entry.term && entry.definition && entry.phase_id && entry.page_id, "glossary entry is incomplete");
    assert(pageIds.has(entry.page_id), `glossary term ${entry.term} points to a missing page`);
    assert(nodesById.has(entry.phase_id), `glossary term ${entry.term} points to a missing phase`);
    assert(entry.source_paths?.length >= 1 && entry.source_paths.every(referenceExists), `glossary term ${entry.term} is not source-linked`);
  }

  for (const pathItem of catalog.paths) {
    assert(pathItem.required_phase_coverage === "all", `${pathItem.id} does not declare full phase coverage`);
    assert(pathItem.page_ids.length === matrixLessons.length, `${pathItem.id} does not reach every lesson`);
    assert(pathItem.page_ids.every((id) => pageIds.has(id)), `${pathItem.id} contains an unknown lesson`);
    assert(pathItem.page_ids.at(-1) === "full-system-validation", `${pathItem.id} does not end at full-system-validation`);
  }
  const eecs = catalog.paths.find((item) => item.id === "eecs");
  assert(new Set(eecs.page_ids).size === matrixLessonIds.size && eecs.page_ids.every((id) => matrixLessonIds.has(id)), "EECS/systems path is not an exact all-lesson traversal");
  const beginner = catalog.paths.find((item) => item.id === "beginner");
  assert(beginner.page_ids[0] === "representation-boundaries", "Beginner path does not start at representation");
  assert(beginner.starting_phase === "tc:layer:phase-01-representation", "Beginner path has the wrong starting phase");
  assertAcyclic(pagesById);

  report.counts = {
    stack_phases: stackNodes.length,
    matrix_phases: matrix.phases.length,
    lessons: matrixLessons.length,
    glossary_terms: catalog.glossary.length,
    tests: tests.length,
    benchmarks: benchmarks.length,
    gaps: gaps.length,
    paths: catalog.paths.length,
    prerequisite_edges: matrix.prerequisite_edge_count,
  };
  report.checks.push("matrix preserves all current stack phase IDs, slugs, ordinals, and names");
  report.checks.push("every phase has two lessons, source links, test evidence, and Stack Explorer links");
  report.checks.push("all test, benchmark, gap, and local source references resolve against repository registries");
  report.checks.push("glossary terms are phase-linked, page-linked, and source-linked");
  report.checks.push("Beginner starts at representation; all paths reach the closure lesson");
  report.checks.push("prerequisite graph is acyclic");
  report.ok = true;
} catch (error) {
  report.errors.push(String(error?.message || error));
}

writeEvidence("curriculum-matrix.json", report);
writeEvidence("glossary-prereq.json", {
  schema: "trit.treatcode_glossary_prerequisite.v1",
  ok: report.ok,
  glossary_terms: report.counts.glossary_terms || 0,
  prerequisite_edges: report.counts.prerequisite_edges || 0,
  checks: report.checks.filter((check) => check.includes("glossary") || check.includes("prerequisite")),
  errors: report.errors,
});
console.log(`TreatCode curriculum coverage: ${report.ok ? "passed" : "failed"}`);
for (const check of report.checks) console.log(`  [ok] ${check}`);
for (const error of report.errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
