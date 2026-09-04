export const PUBLIC_API_SCHEMA_VERSION = "treatcode.public.api.v1" as const;
export const PUBLIC_SNAPSHOT_SCHEMA_VERSION = "treatcode.public.snapshot.v1" as const;

export type PublicResource =
  | "projects"
  | "stack_nodes"
  | "components"
  | "capabilities"
  | "contracts"
  | "decisions"
  | "sources"
  | "symbols"
  | "tests"
  | "benchmarks"
  | "runs"
  | "releases"
  | "gaps";

export type SearchMode = "exact" | "symbol" | "relationship" | "semantic";

export interface SourceRef {
  repository: string;
  commit: string;
  path?: string;
  source_span?: { start_line: number; end_line: number };
  artifact_hash?: string;
  status?: string;
  role?: string;
  reason?: string;
}

export interface PublicRecord {
  id: string;
  entity_type?: string;
  name?: string;
  title?: string;
  description?: string;
  source_refs?: SourceRef[];
  evidence_refs?: SourceRef[];
  [key: string]: unknown;
}

export interface SnapshotInfo {
  id: string;
  repository: string;
  commit: string;
  generated_at: string;
  source: string;
}

export interface PublicRelation {
  from: string;
  type: string;
  to: string;
  source_refs?: SourceRef[];
  source_span?: { start_line: number; end_line: number };
  origin?: string;
}

export interface PublicSnapshot {
  schema_version: typeof PUBLIC_SNAPSHOT_SCHEMA_VERSION;
  snapshot: SnapshotInfo;
  projects: PublicRecord[];
  stack_nodes: PublicRecord[];
  components: PublicRecord[];
  capabilities: PublicRecord[];
  contracts: PublicRecord[];
  decisions: PublicRecord[];
  sources: PublicRecord[];
  symbols: PublicRecord[];
  tests: PublicRecord[];
  benchmarks: PublicRecord[];
  runs: PublicRecord[];
  releases: PublicRecord[];
  gaps: PublicRecord[];
  relations: PublicRelation[];
  statistics: Record<string, number>;
  coverage?: PublicRecord;
  relationship_index?: PublicRecord;
  freshness?: PublicRecord;
}

export interface PublicApiEnvelope<T> {
  schema_version: typeof PUBLIC_API_SCHEMA_VERSION;
  snapshot: SnapshotInfo;
  data: T;
  meta?: {
    resource?: string;
    count?: number;
    total?: number;
    offset?: number;
    limit?: number;
    query?: string;
    mode?: SearchMode;
  };
  links?: Record<string, string>;
}

export interface SearchMatch {
  entity_type: string;
  entity_id: string;
  name: string;
  match_type: SearchMode;
  score: number;
  matched_fields: string[];
  match_reason: string;
  provenance: SourceRef[];
}

export const RESOURCE_KEYS: Record<PublicResource, keyof PublicSnapshot> = {
  projects: "projects",
  stack_nodes: "stack_nodes",
  components: "components",
  capabilities: "capabilities",
  contracts: "contracts",
  decisions: "decisions",
  sources: "sources",
  symbols: "symbols",
  tests: "tests",
  benchmarks: "benchmarks",
  runs: "runs",
  releases: "releases",
  gaps: "gaps",
};

export function collectionFor(snapshot: PublicSnapshot, resource: string): PublicRecord[] | null {
  const key = resource.replace(/-/g, "_") as PublicResource;
  const snapshotKey = RESOURCE_KEYS[key];
  if (!snapshotKey) return null;
  const value = snapshot[snapshotKey];
  return Array.isArray(value) ? (value as PublicRecord[]) : null;
}

export function normalizeSearchMode(mode: string | null | undefined): SearchMode {
  if (mode === "exact" || mode === "symbol" || mode === "relationship") return mode;
  return "semantic";
}

function searchableText(record: PublicRecord): string {
  const values: string[] = [];
  const collect = (value: unknown, depth = 0): void => {
    if (depth > 3 || value === null || value === undefined) return;
    if (typeof value === "string" || typeof value === "number" || typeof value === "boolean") {
      values.push(String(value));
      return;
    }
    if (Array.isArray(value)) {
      for (const item of value) collect(item, depth + 1);
      return;
    }
    if (typeof value === "object") {
      for (const [key, item] of Object.entries(value)) {
        values.push(key);
        collect(item, depth + 1);
      }
    }
  };
  collect(record);
  return values.join(" ").toLowerCase();
}

const SEMANTIC_STOP_WORDS = new Set([
  "a", "an", "and", "are", "as", "at", "be", "by", "can", "do", "does", "for", "from", "how", "i", "in", "is",
  "it", "of", "on", "or", "our", "the", "their", "this", "to", "was", "what", "when", "where", "which", "who", "why",
  "with", "would", "you",
]);

function normalizeSearchText(value: unknown): string {
  if (typeof value !== "string") return "";
  return value
    .normalize("NFKD")
    .replace(/([a-z0-9])([A-Z])/g, "$1 $2")
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, " ")
    .trim()
    .replace(/\s+/g, " ");
}

function semanticTerms(value: string): string[] {
  const normalized = normalizeSearchText(value);
  const terms = normalized.split(" ").filter((term) => term.length > 1 && !SEMANTIC_STOP_WORDS.has(term));
  return [...new Set(terms)];
}

function normalizedField(record: PublicRecord, field: string): string {
  return normalizeSearchText(record[field]);
}

function recordName(record: PublicRecord): string {
  return String(record.name || record.title || record.symbol || record.path || record.id);
}

function entityTypeFor(resource: PublicResource): string {
  return resource.endsWith("s") ? resource.slice(0, -1) : resource;
}

function provenanceFor(record: PublicRecord): SourceRef[] {
  return [...(record.source_refs || []), ...(record.evidence_refs || [])];
}

function resultFor(
  resource: PublicResource,
  record: PublicRecord,
  matchType: SearchMode,
  score: number,
  matchedFields: string[],
  matchReason?: string,
): SearchMatch {
  return {
    entity_type: record.entity_type || entityTypeFor(resource),
    entity_id: record.id,
    name: recordName(record),
    match_type: matchType,
    score: Math.round(score * 100) / 100,
    matched_fields: [...new Set(matchedFields)],
    match_reason: matchReason || `${matchType} match on ${[...new Set(matchedFields)].join(", ") || "the public graph"}.`,
    provenance: provenanceFor(record),
  };
}

interface IndexedRecord {
  resource: PublicResource;
  record: PublicRecord;
  literalName: string;
  literalId: string;
  literalPath: string;
  name: string;
  id: string;
  path: string;
  symbol: string;
  description: string;
  content: string;
  nameTerms: Set<string>;
  idTerms: Set<string>;
  pathTerms: Set<string>;
  symbolTerms: Set<string>;
  tagTerms: Set<string>;
  sourceTerms: Set<string>;
  relationshipText: string;
}

const SNAPSHOT_SEARCH_CACHE = new WeakMap<PublicSnapshot, IndexedRecord[]>();

function termSet(value: string): Set<string> {
  return new Set(value.split(" ").filter(Boolean));
}

function indexedRecords(snapshot: PublicSnapshot): IndexedRecord[] {
  const cached = SNAPSHOT_SEARCH_CACHE.get(snapshot);
  if (cached) return cached;

  const sourceTermsByPath = new Map<string, Set<string>>();
  for (const source of snapshot.sources || []) {
    const sourcePath = typeof source.path === "string" ? source.path.replace(/\\/g, "/") : "";
    if (!sourcePath) continue;
    const terms = Array.isArray(source.search_terms) ? source.search_terms.filter((value): value is string => typeof value === "string") : [];
    sourceTermsByPath.set(sourcePath, new Set(terms));
  }

  const namesById = new Map<string, string>();
  for (const resource of Object.keys(RESOURCE_KEYS) as PublicResource[]) {
    for (const record of collectionFor(snapshot, resource) || []) namesById.set(record.id, recordName(record));
  }
  const relatedTextById = new Map<string, string[]>();
  for (const relation of snapshot.relations || []) {
    const relationText = normalizeSearchText(`${relation.from} ${namesById.get(relation.from) || ""} ${relation.type} ${relation.to} ${namesById.get(relation.to) || ""}`);
    relatedTextById.set(relation.from, [...(relatedTextById.get(relation.from) || []), relationText]);
    relatedTextById.set(relation.to, [...(relatedTextById.get(relation.to) || []), relationText]);
  }

  const documents: IndexedRecord[] = [];
  for (const resource of Object.keys(RESOURCE_KEYS) as PublicResource[]) {
    for (const record of collectionFor(snapshot, resource) || []) {
      const name = normalizeSearchText(recordName(record));
      const id = normalizeSearchText(record.id);
      const recordPath = normalizeSearchText(record.path);
      const symbol = normalizeSearchText(record.symbol);
      const sourceTerms = new Set<string>();
      for (const reference of record.source_refs || []) {
        if (!reference.path) continue;
        const terms = sourceTermsByPath.get(reference.path.replace(/\\/g, "/"));
        if (terms) for (const term of terms) sourceTerms.add(term);
      }
      documents.push({
        resource,
        record,
        literalName: recordName(record).toLowerCase(),
        literalId: record.id.toLowerCase(),
        literalPath: typeof record.path === "string" ? record.path.toLowerCase() : "",
        name,
        id,
        path: recordPath,
        symbol,
        description: normalizedField(record, "description"),
        content: normalizeSearchText(searchableText(record)),
        nameTerms: termSet(name),
        idTerms: termSet(id),
        pathTerms: termSet(recordPath),
        symbolTerms: termSet(symbol),
        tagTerms: termSet(normalizeSearchText(Array.isArray(record.tags) ? record.tags.join(" ") : "")),
        sourceTerms,
        relationshipText: (relatedTextById.get(record.id) || []).join(" "),
      });
    }
  }
  SNAPSHOT_SEARCH_CACHE.set(snapshot, documents);
  return documents;
}

export function searchSnapshot(snapshot: PublicSnapshot, rawQuery: string, rawMode?: string | null): SearchMatch[] {
  const literalQuery = rawQuery.trim().toLowerCase();
  if (!literalQuery) return [];
  const query = normalizeSearchText(rawQuery);
  const mode = normalizeSearchMode(rawMode);
  const terms = semanticTerms(query);
  const results: SearchMatch[] = [];
  for (const document of indexedRecords(snapshot)) {
      const { resource, record, literalName, literalId, literalPath, name, id, path, symbol, description, content, relationshipText } = document;
      const matchedFields: string[] = [];

      if (mode === "exact") {
        if (literalId === literalQuery || id === query) matchedFields.push("id");
        if (literalName === literalQuery || name === query) matchedFields.push("name");
        if (literalPath === literalQuery || path === query) matchedFields.push("path");
        if (matchedFields.length) {
          const exactScore = matchedFields.includes("id") ? 120
            : matchedFields.includes("name") ? 110
              : resource === "sources" ? 105
                : resource === "symbols" ? 80
                  : 100;
          results.push(resultFor(resource, record, mode, exactScore, matchedFields, `Exact query matched ${matchedFields.join(" and ")} on this record.`));
        }
        continue;
      }

      if (mode === "symbol") {
        if (resource !== "symbols" && !record.symbol) continue;
        if (literalId === literalQuery || id === query) matchedFields.push("id");
        if (literalName === literalQuery || name === query || symbol === query) matchedFields.push("symbol");
        if (!matchedFields.length && (name.includes(query) || symbol.includes(query))) {
          matchedFields.push("symbol");
        }
        if (matchedFields.length) results.push(resultFor(resource, record, mode, matchedFields.includes("id") ? 100 : 80, matchedFields, `Symbol query matched ${matchedFields.join(" and ")} in the indexed symbol inventory.`));
        continue;
      }

      if (mode === "relationship") {
        const related = relationshipText.includes(query);
        if (related) {
          matchedFields.push("relationship");
          results.push(resultFor(resource, record, mode, 70, matchedFields, `This record participates in a relationship containing “${rawQuery.trim()}”.`));
        }
        continue;
      }

      if (terms.length === 0) continue;
      let score = 0;
      const matchedTerms = new Set<string>();
      if (id === query) {
        score += 120;
        matchedFields.push("id");
      }
      if (name === query) {
        score += 110;
        matchedFields.push("name");
      } else if (terms.length > 1 && name.includes(query)) {
        score += 65;
        matchedFields.push("name");
      }
      if (path === query) {
        score += 100;
        matchedFields.push("path");
      } else if (terms.length > 1 && path.includes(query)) {
        score += 45;
        matchedFields.push("path");
      }
      for (const term of terms) {
        let termMatched = false;
        if (document.idTerms.has(term)) {
          score += 12;
          matchedFields.push("id");
          termMatched = true;
        }
        if (document.nameTerms.has(term)) {
          score += 16;
          matchedFields.push("name");
          termMatched = true;
        }
        if (symbol && document.symbolTerms.has(term)) {
          score += 18;
          matchedFields.push("symbol");
          termMatched = true;
        }
        if (document.pathTerms.has(term)) {
          score += 9;
          matchedFields.push("path");
          termMatched = true;
        }
        if (document.tagTerms.has(term)) {
          score += 10;
          matchedFields.push("tags");
          termMatched = true;
        }
        if (description.includes(term)) {
          score += 6;
          matchedFields.push("description");
          termMatched = true;
        } else if (content.includes(term)) {
          score += 3;
          matchedFields.push("content");
          termMatched = true;
        }
        if (document.sourceTerms.has(term)) {
          score += 2;
          matchedFields.push("source");
          termMatched = true;
        }
        if (relationshipText.includes(term)) {
          score += 1;
          matchedFields.push("relationship");
          termMatched = true;
        }
        if (termMatched) matchedTerms.add(term);
      }
      if (matchedTerms.size === terms.length) score += 30;
      score += 12 * (matchedTerms.size / terms.length);
      if (matchedTerms.size === 0) continue;
      if (["components", "capabilities", "contracts", "decisions"].includes(resource)) score += 20;
      else if (resource === "stack_nodes") score += 16;
      else if (["tests", "benchmarks", "gaps", "releases"].includes(resource)) score += 10;
      if (resource === "symbols" && !(symbol === query || name === query)) score -= 12;
      if (score > 0) results.push(resultFor(resource, record, mode, score, [...new Set(matchedFields)], `Semantic query matched ${[...new Set(matchedFields)].join(", ")} across the complete public record and relationship index.`));
  }

  return results.sort((left, right) => right.score - left.score || left.name.localeCompare(right.name));
}
