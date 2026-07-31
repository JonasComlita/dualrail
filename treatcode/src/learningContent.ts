import catalog from "./content/learn/learning-catalog.json";

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
  explanation: string;
}

export type LearningInteractive =
  | LearningInteractiveChoice
  | LearningInteractiveCode;

export interface LearningPage {
  schema: string;
  id: string;
  title: string;
  module: string;
  level: "beginner" | "programmer" | "eecs";
  order: number;
  summary: string;
  prerequisites: string[];
  sources: LearningSource[];
  evidence: LearningSource[];
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
}

export interface GlossaryEntry {
  term: string;
  definition: string;
  page_id: string;
}

export interface LearningCatalog {
  schema: string;
  paths: LearningPath[];
  prerequisite_graph: Array<{ from: string; to: string }>;
  glossary: GlossaryEntry[];
  interactive_module_kinds: string[];
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

export const LEARNING_PAGES: LearningPage[] = Object.entries(rawLearningDocuments)
  .map(([filename, source]) => parseLearningDocument(source, filename))
  .sort((left, right) => left.order - right.order);

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
