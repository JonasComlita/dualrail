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
  return [
    record.id,
    record.name,
    record.title,
    record.description,
    record.slug,
    record.path,
    record.symbol,
    record.role,
    record.capability_class,
    record.component_kind,
    record.contract_kind,
    ...(Array.isArray(record.tags) ? record.tags : []),
  ]
    .filter((value): value is string => typeof value === "string")
    .join(" ")
    .toLowerCase();
}

function recordName(record: PublicRecord): string {
  return String(record.name || record.title || record.symbol || record.path || record.id);
}

function entityTypeFor(resource: PublicResource): string {
  return resource.endsWith("s") ? resource.slice(0, -1) : resource;
}

function provenanceFor(record: PublicRecord): SourceRef[] {
  return [...(record.source_refs || []), ...(record.evidence_refs || [])].slice(0, 6);
}

function resultFor(
  resource: PublicResource,
  record: PublicRecord,
  matchType: SearchMode,
  score: number,
  matchedFields: string[],
): SearchMatch {
  return {
    entity_type: record.entity_type || entityTypeFor(resource),
    entity_id: record.id,
    name: recordName(record),
    match_type: matchType,
    score: Math.round(score * 100) / 100,
    matched_fields: matchedFields,
    provenance: provenanceFor(record),
  };
}

export function searchSnapshot(snapshot: PublicSnapshot, rawQuery: string, rawMode?: string | null): SearchMatch[] {
  const query = rawQuery.trim().toLowerCase();
  if (!query) return [];
  const mode = normalizeSearchMode(rawMode);
  const terms = query.split(/\s+/).filter(Boolean);
  const results: SearchMatch[] = [];

  for (const resource of Object.keys(RESOURCE_KEYS) as PublicResource[]) {
    const records = collectionFor(snapshot, resource) || [];
    for (const record of records) {
      const name = recordName(record).toLowerCase();
      const id = record.id.toLowerCase();
      const path = typeof record.path === "string" ? record.path.toLowerCase() : "";
      const text = searchableText(record);
      const matchedFields: string[] = [];

      if (mode === "exact") {
        if (id === query) matchedFields.push("id");
        if (name === query) matchedFields.push("name");
        if (path === query) matchedFields.push("path");
        if (matchedFields.length) results.push(resultFor(resource, record, mode, 100, matchedFields));
        continue;
      }

      if (mode === "symbol") {
        if (resource !== "symbols" && !record.symbol) continue;
        if (id === query) matchedFields.push("id");
        if (name === query || String(record.symbol || "").toLowerCase() === query) matchedFields.push("symbol");
        if (!matchedFields.length && (name.includes(query) || String(record.symbol || "").toLowerCase().includes(query))) {
          matchedFields.push("symbol");
        }
        if (matchedFields.length) results.push(resultFor(resource, record, mode, matchedFields.includes("id") ? 100 : 80, matchedFields));
        continue;
      }

      if (mode === "relationship") {
        const related = snapshot.relations.some((relation) => {
          const relationText = `${relation.from} ${relation.type} ${relation.to}`.toLowerCase();
          return (relation.from === record.id || relation.to === record.id) && relationText.includes(query);
        });
        if (related) {
          matchedFields.push("relationship");
          results.push(resultFor(resource, record, mode, 70, matchedFields));
        }
        continue;
      }

      let score = 0;
      for (const term of terms) {
        if (id === term) {
          score += 10;
          matchedFields.push("id");
        } else if (name === term) {
          score += 8;
          matchedFields.push("name");
        } else if (text.includes(term)) {
          score += 3;
          matchedFields.push("content");
        }
      }
      if (score > 0) results.push(resultFor(resource, record, mode, score, [...new Set(matchedFields)]));
    }
  }

  return results
    .sort((left, right) => right.score - left.score || left.name.localeCompare(right.name))
    .slice(0, 100);
}
