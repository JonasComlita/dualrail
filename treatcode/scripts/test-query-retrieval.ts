import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { searchSnapshot, type PublicSnapshot, type SearchMode } from "../src/publicApi";

type QueryCase = {
  id: string;
  question: string;
  query: string;
  mode: SearchMode;
  expected_entity_ids: string[];
  expected_source_paths: string[];
  required_terms: string[];
  forbidden_generic_claims: string[];
  rationale: string;
};

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const corpusRoot = path.join(repoRoot, "docs", "11_TreatCode_Platform", "query_evaluation", "questions");
const snapshotPath = path.join(appRoot, "public", "api", "v1", "snapshot.json");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-query-evaluation");
const TOP_K = 10;

function normalize(value: string): string {
  return value.replaceAll("\\", "/").replace(/^\.\//, "");
}

function sha256(value: string | Buffer): string {
  return crypto.createHash("sha256").update(value).digest("hex");
}

function readCorpus(): { cases: QueryCase[]; hash: string; files: string[] } {
  const files = fs.readdirSync(corpusRoot).filter((name) => /^Q\d{3}-Q\d{3}\.json$/.test(name)).sort();
  const rawParts: string[] = [];
  const cases: QueryCase[] = [];
  for (const name of files) {
    const raw = fs.readFileSync(path.join(corpusRoot, name), "utf8");
    rawParts.push(raw);
    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed)) throw new Error(`${name} must contain a JSON array`);
    cases.push(...parsed);
  }
  return { cases, hash: `sha256:${sha256(rawParts.join("\n"))}`, files };
}

function publicEntityIds(snapshot: PublicSnapshot): Set<string> {
  const ids = new Set<string>();
  for (const key of ["projects", "stack_nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps"] as const) {
    for (const record of snapshot[key]) ids.add(record.id);
  }
  return ids;
}

function publishedWebsiteText(): string {
  const roots = [
    path.join(appRoot, "src", "content", "learn"),
    path.join(appRoot, "src", "learningContent.ts"),
  ];
  const chunks: string[] = [];
  const collect = (target: string) => {
    if (!fs.existsSync(target)) return;
    const stat = fs.statSync(target);
    if (stat.isFile()) {
      chunks.push(fs.readFileSync(target, "utf8"));
      return;
    }
    for (const entry of fs.readdirSync(target)) collect(path.join(target, entry));
  };
  roots.forEach(collect);
  return chunks.join("\n").toLowerCase();
}

const snapshot = JSON.parse(fs.readFileSync(snapshotPath, "utf8")) as PublicSnapshot;
const corpus = readCorpus();
const ids = publicEntityIds(snapshot);
const websiteText = publishedWebsiteText();
const errors: string[] = [];
const expectedIds = Array.from({ length: 100 }, (_, index) => `Q${String(index + 1).padStart(3, "0")}`);
const observedIds = corpus.cases.map((item) => item.id);

if (corpus.cases.length !== 100) errors.push(`expected exactly 100 questions, found ${corpus.cases.length}`);
if (JSON.stringify(observedIds) !== JSON.stringify(expectedIds)) errors.push("question IDs must be contiguous Q001-Q100 in file order");

const results = corpus.cases.map((item) => {
  const caseErrors: string[] = [];
  if (!item.question?.trim() || !item.query?.trim() || !item.rationale?.trim()) caseErrors.push("question, query, and rationale are required");
  if (!["exact", "symbol", "relationship", "semantic"].includes(item.mode)) caseErrors.push(`unsupported query mode: ${item.mode}`);
  if (!Array.isArray(item.expected_entity_ids) || item.expected_entity_ids.length === 0) caseErrors.push("expected_entity_ids must be non-empty");
  if (!Array.isArray(item.expected_source_paths) || item.expected_source_paths.length === 0) caseErrors.push("expected_source_paths must be non-empty");
  for (const id of item.expected_entity_ids || []) if (!ids.has(id)) caseErrors.push(`unknown expected entity id: ${id}`);

  const normalizedPaths = (item.expected_source_paths || []).map(normalize);
  const sourceText: string[] = [];
  for (const relativePath of normalizedPaths) {
    const absolute = path.join(repoRoot, relativePath);
    if (!fs.existsSync(absolute) || !fs.statSync(absolute).isFile()) {
      caseErrors.push(`missing authoritative source path: ${relativePath}`);
    } else {
      sourceText.push(fs.readFileSync(absolute, "utf8"));
    }
  }

  const top = searchSnapshot(snapshot, item.query, item.mode).slice(0, TOP_K);
  const topIds = top.map((result) => result.entity_id);
  const retrievalPass = item.expected_entity_ids.some((id) => topIds.includes(id));
  if (!retrievalPass) caseErrors.push(`none of the expected entities appeared in the top ${TOP_K}`);

  const provenancePaths = new Set(top.flatMap((result) => result.provenance).map((reference) => normalize(reference.path || "")).filter(Boolean));
  const provenancePass = normalizedPaths.some((sourcePath) => provenancePaths.has(sourcePath));
  if (!provenancePass) caseErrors.push(`none of the expected source paths appeared in top-${TOP_K} provenance`);

  const combinedSource = sourceText.join("\n").toLowerCase();
  const missingTerms = (item.required_terms || []).filter((term) => !combinedSource.includes(String(term).toLowerCase()));
  if (missingTerms.length) caseErrors.push(`required terms absent from authoritative sources: ${missingTerms.join(", ")}`);

  const contamination = (item.forbidden_generic_claims || []).filter((claim) => websiteText.includes(String(claim).toLowerCase()));
  if (contamination.length) caseErrors.push(`forbidden generic claims remain published: ${contamination.join(" | ")}`);

  return {
    id: item.id,
    question: item.question,
    query: item.query,
    mode: item.mode,
    ok: caseErrors.length === 0,
    retrieval_pass: retrievalPass,
    provenance_pass: provenancePass,
    source_terms_pass: missingTerms.length === 0,
    contamination_pass: contamination.length === 0,
    expected_entity_ids: item.expected_entity_ids,
    expected_source_paths: normalizedPaths,
    top_results: top.map((result) => ({
      entity_id: result.entity_id,
      entity_type: result.entity_type,
      name: result.name,
      score: result.score,
      provenance: result.provenance.map((reference) => normalize(reference.path || "")).filter(Boolean),
    })),
    missing_terms: missingTerms,
    contamination,
    errors: caseErrors,
  };
});

for (const result of results) for (const error of result.errors) errors.push(`${result.id}: ${error}`);
const report = {
  schema: "treatcode.query_evaluation.v1",
  ok: errors.length === 0,
  snapshot: snapshot.snapshot,
  corpus_files: corpus.files,
  corpus_hash: corpus.hash,
  top_k: TOP_K,
  summary: {
    total: results.length,
    passed: results.filter((result) => result.ok).length,
    retrieval_passed: results.filter((result) => result.retrieval_pass).length,
    provenance_passed: results.filter((result) => result.provenance_pass).length,
    source_terms_passed: results.filter((result) => result.source_terms_pass).length,
    contamination_passed: results.filter((result) => result.contamination_pass).length,
  },
  results,
  errors,
};

fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(path.join(evidenceRoot, "result.json"), `${JSON.stringify(report, null, 2)}\n`);
const markdown = [
  "# TreatCode Query Evaluation",
  "",
  `- Snapshot: \`${snapshot.snapshot.commit}\``,
  `- Corpus: \`${corpus.hash}\``,
  `- Passed: ${report.summary.passed}/${report.summary.total}`,
  `- Retrieval: ${report.summary.retrieval_passed}/${report.summary.total}`,
  `- Provenance: ${report.summary.provenance_passed}/${report.summary.total}`,
  `- Source support: ${report.summary.source_terms_passed}/${report.summary.total}`,
  `- Generic contamination: ${report.summary.contamination_passed}/${report.summary.total}`,
  "",
  ...results.filter((result) => !result.ok).map((result) => `- **${result.id}**: ${result.errors.join("; ")}`),
  "",
].join("\n");
fs.writeFileSync(path.join(evidenceRoot, "REPORT.md"), markdown);

console.log(`TreatCode query evaluation: ${report.summary.passed}/${report.summary.total} passed`);
console.log(`  retrieval ${report.summary.retrieval_passed}/${report.summary.total}`);
console.log(`  provenance ${report.summary.provenance_passed}/${report.summary.total}`);
console.log(`  source support ${report.summary.source_terms_passed}/${report.summary.total}`);
console.log(`  contamination ${report.summary.contamination_passed}/${report.summary.total}`);
for (const error of errors.slice(0, 30)) console.error(`  [fail] ${error}`);
if (errors.length > 30) console.error(`  ... ${errors.length - 30} more failures; see build/treatcode-query-evaluation/result.json`);
process.exitCode = report.ok ? 0 : 1;
