import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
export const treatcodeRoot = path.resolve(scriptDirectory, "..");
export const repoRoot = path.resolve(treatcodeRoot, "..");
export const contentRoot = path.join(treatcodeRoot, "src", "content", "learn");
export const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P16");
export const p05EvidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P05");

export function relativeToRepo(filePath) {
  return path.relative(repoRoot, filePath).replaceAll(path.sep, "/");
}

export function readJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, "utf8"));
}

export function readRegistry(fileName) {
  const filePath = path.join(treatcodeRoot, "public", "api", "v1", fileName);
  const envelope = readJson(filePath);
  return Array.isArray(envelope?.data) ? envelope.data : envelope;
}

export function parseLearningDocument(filePath) {
  const source = fs.readFileSync(filePath, "utf8");
  const match = source.match(/^---\r?\n([\s\S]*?)\r?\n---\r?\n([\s\S]*)$/);
  if (!match) throw new Error(`${relativeToRepo(filePath)} is missing JSON front matter`);
  let metadata;
  try {
    metadata = JSON.parse(match[1]);
  } catch (error) {
    throw new Error(`${relativeToRepo(filePath)} has invalid JSON front matter: ${error.message}`);
  }
  return { filePath, metadata, content: match[2].trim() };
}

export function pageFiles() {
  return fs.readdirSync(contentRoot)
    .filter((name) => name.endsWith(".md"))
    .sort()
    .map((name) => parseLearningDocument(path.join(contentRoot, name)));
}

export function loadCourse() {
  const catalog = readJson(path.join(contentRoot, "learning-catalog.json"));
  const matrix = readJson(path.join(contentRoot, "P05_CURRICULUM_MATRIX.json"));
  const pages = pageFiles();
  return {
    catalog,
    matrix,
    pages,
    pagesById: new Map(pages.map((page) => [page.metadata.id, page])),
    stackNodes: readRegistry("stack_nodes.json"),
    tests: readRegistry("tests.json"),
    benchmarks: readRegistry("benchmarks.json"),
    gaps: readRegistry("gaps.json"),
    snapshot: readJson(path.join(treatcodeRoot, "src", "generated", "learning-snapshot.json")),
  };
}

export function assert(condition, message) {
  if (!condition) throw new Error(message);
}

export function resolveRepositoryPath(referencePath) {
  if (typeof referencePath !== "string" || !referencePath || path.isAbsolute(referencePath)) return null;
  const normalized = referencePath.replaceAll("\\", "/");
  if (normalized.split("/").includes("..")) return null;
  const candidates = [path.join(repoRoot, normalized)];
  if (!normalized.startsWith("treatcode/")) candidates.push(path.join(treatcodeRoot, normalized));
  return candidates.find((candidate) => fs.existsSync(candidate)) || null;
}

export function referenceExists(referencePath) {
  return Boolean(resolveRepositoryPath(referencePath));
}

export function countWords(text) {
  const prose = text
    .replace(/```[\s\S]*?```/g, " ")
    .replace(/^#{1,6}\s+/gm, "")
    .replace(/!\[[^\]]*\]\([^)]*\)/g, " ")
    .replace(/\[[^\]]*\]\([^)]*\)/g, " ")
    .replace(/[`*_>#|]/g, " ");
  return prose.match(/[\p{L}\p{N}][\p{L}\p{N}'’+\-]*/gu)?.length || 0;
}

export function internalLinks(text) {
  return [...text.matchAll(/\[[^\]]+\]\(([^)]+)\)/g)].map((match) => match[1]);
}

export function learningHref(pathId, pageId) {
  return pageId ? `/learn/${pathId}/${pageId}` : `/learn/${pathId}`;
}

export function writeEvidence(fileName, report, roots = [evidenceRoot]) {
  for (const root of roots) {
    fs.mkdirSync(root, { recursive: true });
    fs.writeFileSync(path.join(root, fileName), `${JSON.stringify(report, null, 2)}\n`, "utf8");
  }
}

export function pageIdSet(pathItem) {
  return new Set(pathItem.page_ids || []);
}

export function phaseForPage(matrix, pageId) {
  return matrix.phases.find((phase) => phase.lessons.some((lesson) => lesson.id === pageId)) || null;
}
