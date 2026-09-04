import {
  assert,
  countWords,
  loadCourse,
  writeEvidence,
} from "./learning-test-utils.mjs";

const report = {
  schema: "trit.treatcode_content_depth.v1",
  ok: false,
  minimum_words: 600,
  counts: {},
  lessons: [],
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

try {
  const { pages, matrix } = loadCourse();
  const phaseIds = new Set(matrix.phases.map((phase) => phase.phase_id));
  const kinds = new Set();
  let minimumObserved = Number.POSITIVE_INFINITY;
  let maximumObserved = 0;
  let totalWords = 0;
  for (const page of pages) {
    const meta = page.metadata;
    const words = countWords(page.content);
    minimumObserved = Math.min(minimumObserved, words);
    maximumObserved = Math.max(maximumObserved, words);
    totalWords += words;
    assert(phaseIds.has(meta.phase_id), `${meta.id} is not attached to a matrix phase`);
    assert(words >= report.minimum_words, `${meta.id} contains only ${words} substantive prose words; minimum is ${report.minimum_words}`);
    for (const section of requiredSections) assert(new RegExp(`^##\\s+${section}`, "im").test(page.content), `${meta.id} is missing ${section}`);
    assert(page.content.includes("general concept") && page.content.includes("current Trit implementation"), `${meta.id} does not distinguish concept from implementation`);
    assert(page.content.includes("planned") && page.content.includes("unavailable"), `${meta.id} does not label planned or unavailable work`);
    assert(page.content.includes("worked example") || page.content.includes("Worked example"), `${meta.id} does not describe a worked example`);
    assert(page.content.includes("Common misconception"), `${meta.id} does not surface a misconception heading`);
    assert(page.content.includes("Learner action"), `${meta.id} does not give a learner action heading`);
    assert(meta.sources.length >= 2 && meta.evidence.length >= 1, `${meta.id} is missing source or evidence references`);
    kinds.add(meta.interactive.kind);
    report.lessons.push({ id: meta.id, phase_id: meta.phase_id, words, interaction: meta.interactive.kind, sources: meta.sources.length, evidence: meta.evidence.length });
  }
  assert(kinds.size === 4, "content does not include all four interaction kinds");
  report.counts = { phases: matrix.phase_count, lessons: pages.length, total_words: totalWords, average_words: Math.round(totalWords / pages.length), minimum_words: minimumObserved, maximum_words: maximumObserved, interaction_kinds: [...kinds].sort() };
  report.checks.push("every lesson has 600+ substantive prose words");
  report.checks.push("every lesson names objectives, prerequisites, purpose, explanation, example, misconception, action, source links, validation, and next step");
  report.checks.push("general concepts are separated from current, planned, and unavailable implementation status");
  report.checks.push("all four interaction types have substantive lesson context");
  report.ok = true;
} catch (error) {
  report.errors.push(String(error?.message || error));
}

writeEvidence("content-depth.json", report);
console.log(`TreatCode content depth: ${report.ok ? "passed" : "failed"}`);
for (const check of report.checks) console.log(`  [ok] ${check}`);
for (const error of report.errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
