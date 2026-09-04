import catalog from "./content/learn/learning-catalog.json";
import curriculumMatrix from "./content/learn/P05_CURRICULUM_MATRIX.json";

export interface LearningSource {
  path: string;
  label: string;
  kind: string;
}

export interface LearningInteractiveChoice {
  kind: "choice";
  title: string;
  prompt: string;
  options: string[];
  answer: number;
  explanation: string;
}

export interface LearningInteractiveCode {
  kind: "code";
  title: string;
  starter: string;
  expectedIncludes: string[];
  exercise_id: string;
  runner: string;
  explanation: string;
}

export interface LearningTraceStep {
  label: string;
  state: string;
  explanation: string;
}

export interface LearningInteractiveTrace {
  kind: "trace";
  title: string;
  prompt: string;
  steps: LearningTraceStep[];
}

export interface LearningInteractiveSource {
  kind: "source";
  title: string;
  prompt: string;
  sourcePath: string;
  sourceHref: string;
  expectedIncludes: string[];
  explanation: string;
}

export type LearningInteractive =
  | LearningInteractiveChoice
  | LearningInteractiveCode
  | LearningInteractiveTrace
  | LearningInteractiveSource;

export interface LearningStackLinks {
  phase: string;
  source: string | null;
  tests: string;
}

export interface LearningPage {
  schema: string;
  id: string;
  title: string;
  module: string;
  level: "beginner" | "programmer" | "eecs";
  order: number;
  summary: string;
  phase_id: string;
  phase_slug: string;
  phase_name: string;
  lesson_kind: "mental-model" | "build-trace";
  implementation_status: string;
  canonical_terms: string[];
  objectives: string[];
  prerequisites: string[];
  sources: LearningSource[];
  evidence: LearningSource[];
  test_ids: string[];
  benchmark_ids: string[];
  gap_ids: string[];
  stack_links: LearningStackLinks;
  next: string | null;
  interactive: LearningInteractive;
  content: string;
}

export interface LearningPath {
  id: string;
  title: string;
  audience: string;
  description: string;
  page_ids: string[];
  starting_phase?: string;
  terminal_lesson_id?: string;
  required_phase_coverage?: "all" | "partial";
  capstone?: string;
}

export interface GlossaryEntry {
  term: string;
  definition: string;
  page_id: string;
  phase_id: string;
  source_paths: string[];
  related_terms: string[];
}

export interface LearningCatalog {
  schema: string;
  generated_from: string;
  snapshot_commit: string;
  phase_ids: string[];
  paths: LearningPath[];
  prerequisite_graph: Array<{ from: string; to: string }>;
  glossary: GlossaryEntry[];
  interactive_module_kinds: string[];
  counts: { phases: number; lessons: number; glossary_terms: number };
}

export interface CurriculumLessonRow {
  id: string;
  title: string;
  kind: string;
  objectives: string[];
  sources: string[];
  evidence: string[];
  test_ids: string[];
  benchmark_ids: string[];
  gap_ids: string[];
  exercise: {
    interaction_kind: string;
    execution_kind: string;
    id: string;
  };
  prerequisites: string[];
  next: string | null;
  stack_phase_id: string;
}

export interface CurriculumPhaseRow {
  phase_id: string;
  ordinal: number;
  slug: string;
  name: string;
  required_focus: string;
  entry: string;
  exit: string;
  implementation_status: string;
  lessons: CurriculumLessonRow[];
  canonical_lesson_ids: string[];
  glossary_terms: string[];
  source_paths: string[];
  test_ids: string[];
  benchmark_ids: string[];
  gap_ids: string[];
}

export interface CurriculumMatrix {
  schema: string;
  generated_from: string;
  snapshot: { id: string; repository: string; commit: string; generated_at: string; source: string };
  phase_count: number;
  lesson_count: number;
  phases: CurriculumPhaseRow[];
  paths: Array<{ id: string; page_ids: string[]; terminal_lesson_id: string; required_phase_coverage: string }>;
  glossary_count: number;
  prerequisite_edge_count: number;
}

const rawLearningDocuments = import.meta.glob(
  "./content/learn/*.md",
  { query: "?raw", import: "default", eager: true }
) as Record<string, string>;

function parseLearningDocument(source: string, filename: string): LearningPage {
  const match = source.match(/^---\r?\n([\s\S]*?)\r?\n---\r?\n([\s\S]*)$/);
  if (!match) {
    throw new Error(`Learning document ${filename} is missing JSON front matter`);
  }

  let metadata: Omit<LearningPage, "content">;
  try {
    metadata = JSON.parse(match[1]) as Omit<LearningPage, "content">;
  } catch (error) {
    throw new Error(`Learning document ${filename} has invalid JSON front matter: ${String(error)}`);
  }

  return {
    ...metadata,
    content: match[2].trim(),
  };
}

export const LEARNING_CATALOG = catalog as LearningCatalog;
export const LEARNING_MATRIX = curriculumMatrix as CurriculumMatrix;

export const LEARNING_PAGES: LearningPage[] = Object.entries(rawLearningDocuments)
  .map(([filename, source]) => parseLearningDocument(source, filename))
  .sort((left, right) => left.order - right.order);

export const LEARNING_PAGE_BY_ID = new Map(LEARNING_PAGES.map((page) => [page.id, page]));

export function learningPathPages(path: LearningPath): LearningPage[] {
  return path.page_ids
    .map((pageId) => LEARNING_PAGE_BY_ID.get(pageId))
    .filter((page): page is LearningPage => Boolean(page));
}

export interface Block {
  type: "p" | "h2" | "h3" | "h4" | "code" | "ul" | "ol" | "table";
  content: string;
  lang?: string;
  items?: string[];
  rows?: string[][];
  headers?: string[];
}

function splitTableRow(line: string): string[] {
  return line
    .split("|")
    .map((part) => part.trim())
    .filter((_, index, parts) => index > 0 && index < parts.length - 1);
}

function isTableDivider(parts: string[]): boolean {
  return parts.length > 0 && parts.every((part) => /^:?-{3,}:?$/.test(part));
}

/** Parse the small, deliberately safe Markdown subset used by learning pages. */
export function parseMarkdown(text: string): Block[] {
  const lines = text.split(/\r?\n/);
  const blocks: Block[] = [];
  let current: Block | null = null;

  const flush = () => {
    if (current) {
      blocks.push(current);
      current = null;
    }
  };

  for (const line of lines) {
    const trimmed = line.trim();

    if (trimmed.startsWith("```")) {
      if (current?.type === "code") {
        flush();
      } else {
        flush();
        current = { type: "code", content: "", lang: trimmed.substring(3).trim() };
      }
      continue;
    }

    if (current?.type === "code") {
      current.content += current.content ? `\n${line}` : line;
      continue;
    }

    if (!trimmed) {
      flush();
      continue;
    }

    const heading = trimmed.match(/^(#{2,4})\s+(.+)$/);
    if (heading) {
      flush();
      const type = heading[1].length === 2 ? "h2" : heading[1].length === 3 ? "h3" : "h4";
      current = { type, content: heading[2].trim() };
      flush();
      continue;
    }

    if (trimmed.startsWith("|")) {
      const parts = splitTableRow(trimmed);
      if (isTableDivider(parts)) {
        continue;
      }
      if (!current || current.type !== "table") {
        flush();
        current = { type: "table", content: "", headers: parts, rows: [] };
      } else {
        current.rows = current.rows || [];
        current.rows.push(parts);
      }
      continue;
    }

    const unordered = trimmed.match(/^[-*]\s+(.+)$/);
    if (unordered) {
      if (!current || current.type !== "ul") {
        flush();
        current = { type: "ul", content: "", items: [] };
      }
      current.items?.push(unordered[1]);
      continue;
    }

    const ordered = trimmed.match(/^\d+\.\s+(.+)$/);
    if (ordered) {
      if (!current || current.type !== "ol") {
        flush();
        current = { type: "ol", content: "", items: [] };
      }
      current.items?.push(ordered[1]);
      continue;
    }

    if (!current || current.type !== "p") {
      flush();
      current = { type: "p", content: trimmed };
    } else {
      current.content += ` ${trimmed}`;
    }
  }

  flush();
  return blocks;
}
