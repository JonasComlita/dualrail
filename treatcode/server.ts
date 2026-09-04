import express, { Request, Response } from "express";
import cors from "cors";
import fs from "fs";
import path from "path";
import {
  PUBLIC_API_SCHEMA_VERSION,
  PublicRecord,
  PublicSnapshot,
  collectionFor,
  normalizeSearchMode,
  searchSnapshot,
} from "./src/publicApi";
import {
  WORKSPACE_API_SCHEMA_VERSION,
  WorkspaceServiceError,
  workspaceService,
  type WorkspaceActor,
} from "./src/workspaceService";
import type { WorkspaceScope } from "./src/workspaceApi";
import {
  EXECUTION_REQUEST_SCHEMA,
  SecureExecutionQueue,
  type ExecutionResult,
  type RunnerEngine,
} from "./src/runner/secure-runner";
import {
  CONTRIBUTION_ACTIONS,
  ContributionAction,
  ContributionActor,
  ContributionError,
  ContributionService,
  InMemoryGitHubApp,
} from "./src/contributionService";
import { OperationActor, OperationsBackup, OperationsStore, TaskInput } from "./operationsStore";
import {
  AUTH_API_SCHEMA_VERSION,
  DEFAULT_PROJECT_ID,
  PERMISSION_ACTIONS,
  AuthStore,
  actionCatalog,
  bearerToken,
  createDefaultAuthStore,
  type AuthorizationDenial,
  type PermissionAction,
} from "./src/auth";
import {
  COMMUNITY_API_SCHEMA_VERSION,
  CommunityStore,
  CommunityStoreError,
} from "./src/communityStore";
import {
  INTELLIGENCE_TASK_ID,
  INTELLIGENCE_MANIFEST_SCHEMA,
  IntelligenceServiceError,
  intelligenceService,
} from "./src/intelligenceService";
import { intelligenceV3CatalogService } from "./src/intelligenceV3Catalog";
import { intelligenceV31CatalogService } from "./src/intelligenceV31Catalog";

export const app = express();
const PORT = process.env.PORT || 3000;

app.use(cors());
app.use(express.json({ limit: "8mb" }));

// In production, Vite builds static files to 'dist'. Serve them.
const distPath = path.join(__dirname, "dist");
if (fs.existsSync(distPath)) {
  // Render the public reading routes from the same snapshot that powers the
  // JSON API. This keeps direct links useful to crawlers and clients with
  // JavaScript disabled while leaving the editor, runner, and operator routes
  // on their existing static entry points.
  app.get("/", (req: Request, res: Response) => sendPublicPage(req, res, "home"));
  app.get("/stack", (req: Request, res: Response) => sendPublicPage(req, res, "stack"));
  app.get("/stack/:slug", (req: Request, res: Response) => sendPublicPage(req, res, "stack-detail"));
  app.get("/resources/:resource/:id", (req: Request, res: Response) => sendPublicPage(req, res, "resource-detail"));
  app.get("/resources/:resource", (req: Request, res: Response) => sendPublicPage(req, res, "resource"));
  app.get("/evidence/:id", (req: Request, res: Response) => sendPublicPage(req, res, "evidence-detail"));
  app.get("/evidence", (req: Request, res: Response) => sendPublicPage(req, res, "evidence"));
  app.get("/search", (req: Request, res: Response) => sendPublicPage(req, res, "search"));
  app.get("/operations", (_req: Request, res: Response) => res.sendFile(path.join(distPath, "operations", "index.html")));
  app.get("/intelligence", (_req: Request, res: Response) => res.sendFile(path.join(distPath, "intelligence", "index.html")));
  app.get("/practice", (_req: Request, res: Response) => res.sendFile(path.join(distPath, "practice", "index.html")));
  app.get("/arena", (_req: Request, res: Response) => res.sendFile(path.join(distPath, "arena", "index.html")));
  app.use(express.static(distPath));
}

// The public platform surface is intentionally read-only and snapshot-backed.
// It is registered before the legacy challenge endpoints so public reading
// routes never need to load the compiler, editor, or runner state.
const publicSnapshotCandidates = [
  path.join(__dirname, "public", "api", "v1", "snapshot.json"),
  path.join(process.cwd(), "public", "api", "v1", "snapshot.json"),
  path.join(process.cwd(), "treatcode", "public", "api", "v1", "snapshot.json"),
];

function loadPublicSnapshot(): PublicSnapshot {
  for (const candidate of publicSnapshotCandidates) {
    try {
      const parsed = JSON.parse(fs.readFileSync(candidate, "utf8")) as PublicSnapshot;
      if (parsed.schema_version === "treatcode.public.snapshot.v1" && parsed.snapshot?.commit) return parsed;
    } catch {
      // Try the next source location; the static route still has a useful fallback.
    }
  }
  return {
    schema_version: "treatcode.public.snapshot.v1",
    snapshot: {
      id: "tc:snapshot:unavailable",
      repository: "https://github.com/JonasComlita/dualrail",
      commit: "unknown",
      generated_at: new Date(0).toISOString(),
      source: "snapshot unavailable",
    },
    projects: [], stack_nodes: [], components: [], capabilities: [], contracts: [], decisions: [], sources: [], symbols: [], tests: [], benchmarks: [], runs: [], releases: [], gaps: [], relations: [], statistics: {},
  };
}

const publicSnapshot = loadPublicSnapshot();
function loadPublicRelationshipIndex(snapshot: PublicSnapshot): Record<string, unknown> {
  const candidates = [
    path.join(__dirname, "public", "api", "v1", "relationship-index.json"),
    path.join(process.cwd(), "public", "api", "v1", "relationship-index.json"),
    path.join(process.cwd(), "treatcode", "public", "api", "v1", "relationship-index.json"),
  ];
  for (const candidate of candidates) {
    try {
      const parsed = JSON.parse(fs.readFileSync(candidate, "utf8")) as Record<string, unknown>;
      if (parsed.schema === "treatcode.public.relationship-index.v1" && Array.isArray(parsed.edges)) return parsed;
    } catch {
      // Try the next workspace layout used by Bun, node, and the built server.
    }
  }
  const edges = Array.isArray(snapshot.relations) ? snapshot.relations : [];
  return {
    schema: "treatcode.public.relationship-index.v1",
    snapshot: snapshot.snapshot,
    edges,
    unresolved_external_edges: [],
    counts: { public_edges: edges.length, unresolved_external_edges: 0, by_type: {} },
    complete: false,
  };
}
const publicRelationshipIndex = loadPublicRelationshipIndex(publicSnapshot);
const publicResources = new Set([
  "projects", "stack-nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps",
]);

function isPublicResource(resource: string): boolean {
  return publicResources.has(resource) || publicResources.has(resource.replace(/_/g, "-"));
}

function publicEnvelope<T>(data: T, resource: string, prefix: string, meta: Record<string, unknown> = {}) {
  return {
    schema_version: PUBLIC_API_SCHEMA_VERSION,
    snapshot: publicSnapshot.snapshot,
    data,
    meta: { resource, ...meta },
    links: { self: `${prefix}/${resource}` },
  };
}

function publicError(res: Response, status: number, code: string, message: string, prefix: string) {
  res.status(status).json({
    schema_version: PUBLIC_API_SCHEMA_VERSION,
    snapshot: publicSnapshot.snapshot,
    error: { code, message },
    links: { api: prefix },
  });
}

function publicEntityById(id: string): PublicRecord | null {
  for (const resource of ["projects", "stack_nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps"] as const) {
    const record = publicSnapshot[resource].find((candidate) => candidate.id === id);
    if (record) return record;
  }
  return null;
}

const publicCollectionNames = ["projects", "stack_nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps"] as const;
const publicHtmlPageSize = 50;

function escapePublicHtml(value: unknown): string {
  return String(value ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

function publicRecordName(record: PublicRecord): string {
  return String(record.name || record.title || record.symbol || record.path || record.id);
}

function publicResourceForRecord(record: PublicRecord): string | null {
  const explicit = String(record.public_resource || "");
  if ((publicCollectionNames as readonly string[]).includes(explicit)) return explicit;
  const entityType = String(record.entity_type || "");
  const derived = entityType === "stack_node" ? "stack_nodes" : `${entityType}s`;
  return (publicCollectionNames as readonly string[]).includes(derived) ? derived : null;
}

function publicHtmlRecordHref(record: PublicRecord): string {
  if (record.entity_type === "stack_node") return `/stack/${encodeURIComponent(String(record.slug || record.id.split(":").slice(2).join(":") || record.id))}`;
  const resource = publicResourceForRecord(record);
  return resource ? `/resources/${resource}/${encodeURIComponent(record.id)}` : `/evidence/${encodeURIComponent(record.id)}`;
}

function publicHtmlEvidenceHref(record: PublicRecord): string {
  return `/evidence/${encodeURIComponent(record.id)}`;
}

function publicHtmlSourceHref(reference: Record<string, unknown>): string | null {
  const repository = typeof reference.repository === "string" ? reference.repository : publicSnapshot.snapshot.repository;
  const commit = typeof reference.commit === "string" ? reference.commit : publicSnapshot.snapshot.commit;
  const sourcePath = typeof reference.path === "string" ? reference.path : "";
  if (!repository || !commit || commit === "unknown" || !sourcePath || sourcePath.startsWith("build/") || sourcePath.includes("\\") || sourcePath.includes("..")) return null;
  const span = reference.source_span && typeof reference.source_span === "object" ? `#L${Number((reference.source_span as Record<string, unknown>).start_line || 1)}` : "";
  return `${repository}/blob/${encodeURIComponent(commit)}/${sourcePath.split("/").map((part) => encodeURIComponent(part)).join("/")}${span}`;
}

function publicHtmlCollectionName(value: string): string | null {
  const normalized = value.replace(/-/g, "_");
  return (publicCollectionNames as readonly string[]).includes(normalized) ? normalized : null;
}

function publicHtmlValue(value: unknown, depth = 0): string {
  if (value === null || value === undefined) return "<span class=\"tc-muted\">—</span>";
  if (depth > 3) return escapePublicHtml(value);
  if (Array.isArray(value)) return value.length ? `<ul>${value.map((item) => `<li>${publicHtmlValue(item, depth + 1)}</li>`).join("")}</ul>` : "<span class=\"tc-muted\">none</span>";
  if (typeof value === "object") return `<dl>${Object.entries(value as Record<string, unknown>).map(([key, item]) => `<div><dt>${escapePublicHtml(key)}</dt><dd>${publicHtmlValue(item, depth + 1)}</dd></div>`).join("")}</dl>`;
  if (typeof value === "string") {
    const record = value.startsWith("tc:") ? publicEntityById(value) : null;
    return record ? `<a href=\"${escapePublicHtml(publicHtmlRecordHref(record))}\">${escapePublicHtml(value)}</a>` : escapePublicHtml(value);
  }
  return escapePublicHtml(value);
}

function publicHtmlSourceRefs(refs: unknown): string {
  const references = Array.isArray(refs) ? refs : [];
  if (!references.length) return "<p class=\"tc-empty\">No provenance recorded.</p>";
  return `<ul>${references.map((item, index) => {
    const reference = (item && typeof item === "object" ? item : {}) as Record<string, unknown>;
    const label = `${String(reference.path || reference.artifact_hash || "immutable evidence")}${reference.source_span && typeof reference.source_span === "object" ? `:${String((reference.source_span as Record<string, unknown>).start_line || "")}` : ""}`;
    const href = publicHtmlSourceHref(reference);
    const link = href ? `<a href=\"${escapePublicHtml(href)}\" target=\"_blank\" rel=\"noreferrer\">${escapePublicHtml(label)}</a>` : escapePublicHtml(label);
    return `<li>${link} <span class=\"tc-citation\">${escapePublicHtml(reference.role || reference.status || reference.reason || "")}</span></li>`;
  }).join("")}</ul>`;
}

function publicHtmlRecordSummary(record: PublicRecord): string {
  const summary = record.description || record.title || record.path || record.status || record.result || "Repository-backed public record.";
  return `<article class=\"tc-card\"><span class=\"tc-eyebrow\">${escapePublicHtml(record.entity_type || "entity")}</span><h2><a href=\"${escapePublicHtml(publicHtmlRecordHref(record))}\">${escapePublicHtml(publicRecordName(record))}</a></h2><p>${escapePublicHtml(summary)}</p><p class=\"tc-citation\">${escapePublicHtml(record.id)} · <a href=\"${escapePublicHtml(publicHtmlEvidenceHref(record))}\">evidence</a></p></article>`;
}

function publicHtmlRelations(record: PublicRecord): string {
  const relations = (publicSnapshot.relations || []).filter((relation) => relation.from === record.id || relation.to === record.id);
  if (!relations.length) return "<p class=\"tc-empty\">No relationship edge is recorded for this entity.</p>";
  return `<ul>${relations.map((relation) => {
    const outgoing = relation.from === record.id;
    const otherId = outgoing ? relation.to : relation.from;
    const other = publicEntityById(otherId);
    const label = other ? `<a href=\"${escapePublicHtml(publicHtmlRecordHref(other))}\">${escapePublicHtml(publicRecordName(other))}</a>` : escapePublicHtml(otherId);
    return `<li><span class=\"tc-citation\">${escapePublicHtml(outgoing ? relation.type : `inverse ${relation.type}`)}</span> ${label} <span class=\"tc-citation\">· ${escapePublicHtml(relation.origin || "public registry")}</span></li>`;
  }).join("")}</ul>`;
}

function publicHtmlRecordDetail(record: PublicRecord, evidenceMode = false): string {
  const fields = Object.entries(record)
    .filter(([key]) => !["id", "entity_type", "name", "title", "source_refs", "evidence_refs"].includes(key))
    .sort(([left], [right]) => left.localeCompare(right));
  const resource = publicResourceForRecord(record);
  return `<article class=\"tc-detail\"><header class=\"tc-detail-header\"><span class=\"tc-eyebrow\">${escapePublicHtml(evidenceMode ? "Evidence record" : `${resource || "Public record"} detail`)}</span><h1>${escapePublicHtml(publicRecordName(record))}</h1><p class=\"tc-lede\">${escapePublicHtml(record.description || record.title || "Repository-backed public record.")}</p><span class=\"tc-detail-id\">${escapePublicHtml(record.id)} · ${escapePublicHtml(record.entity_type || "entity")}</span><p><a href=\"${escapePublicHtml(publicHtmlRecordHref(record))}\">Canonical resource route</a> · <a href=\"${escapePublicHtml(publicHtmlEvidenceHref(record))}\">Evidence view</a></p></header><section class=\"tc-panel\"><h2>Record fields</h2><dl>${fields.map(([key, value]) => `<div><dt>${escapePublicHtml(key)}</dt><dd>${publicHtmlValue(value)}</dd></div>`).join("")}</dl></section><section class=\"tc-panel\"><h2>Source provenance</h2>${publicHtmlSourceRefs(record.source_refs)}</section><section class=\"tc-panel\"><h2>Evidence provenance</h2>${publicHtmlSourceRefs(record.evidence_refs)}</section><section class=\"tc-panel\"><h2>Relationships (${(publicSnapshot.relations || []).filter((relation) => relation.from === record.id || relation.to === record.id).length})</h2>${publicHtmlRelations(record)}</section></article>`;
}

function publicHtmlPageChrome(title: string, body: string): string {
  let stylesheet = "";
  let script = "";
  try {
    const indexHtml = fs.readFileSync(path.join(distPath, "index.html"), "utf8");
    const stylesheetMatch = indexHtml.match(/<link[^>]+rel=[\"']stylesheet[\"'][^>]+href=[\"']([^\"']+)[\"']/i);
    const scriptMatch = indexHtml.match(/<script[^>]+type=[\"']module[\"'][^>]+src=[\"']([^\"']+)[\"']/i);
    if (stylesheetMatch) stylesheet = `<link rel=\"stylesheet\" href=\"${escapePublicHtml(stylesheetMatch[1])}\">`;
    if (scriptMatch) script = `<script type=\"module\" crossorigin src=\"${escapePublicHtml(scriptMatch[1])}\"></script>`;
  } catch {
    // The API and direct static shell remain usable during an unbuilt checkout.
  }
  return `<!doctype html><html lang=\"en\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"><title>${escapePublicHtml(title)} · TREATCODE</title>${stylesheet}</head><body><div id=\"root\"><div class=\"tc-shell\"><header class=\"tc-topbar\"><a class=\"tc-brand\" href=\"/\">TREATCODE</a><nav class=\"tc-nav\" aria-label=\"Primary navigation\"><a href=\"/\">Overview</a><a href=\"/stack\">Stack Explorer</a><a href=\"/learn\">Learn</a><a href=\"/evidence\">Evidence</a><a href=\"/practice\">Practice</a><a href=\"/arena\">Implementation Arena</a><a href=\"/intelligence\">Intelligence Benchmark</a><a href=\"/api/public/v1/openapi.json\">API</a></nav></header><main class=\"tc-main\">${body}</main><footer class=\"tc-footer\">Snapshot ${escapePublicHtml(publicSnapshot.snapshot.id)} · commit ${escapePublicHtml(publicSnapshot.snapshot.commit)} · Read-only public route · <a href=\"/api/public/v1/snapshot.json\">Static snapshot</a></footer></div></div>${script}</body></html>`;
}

function publicHtmlPagination(baseHref: string, page: number, total: number): string {
  const pages = Math.max(1, Math.ceil(total / publicHtmlPageSize));
  const makeHref = (targetPage: number): string => {
    const [pathname, query = ""] = baseHref.split("?", 2);
    const params = new URLSearchParams(query);
    if (targetPage <= 1) params.delete("page"); else params.set("page", String(targetPage));
    const serialized = params.toString();
    return serialized ? `${pathname}?${serialized}` : pathname;
  };
  const previous = page > 1 ? `<a href=\"${escapePublicHtml(makeHref(page - 1))}\">← Previous</a>` : "<span aria-hidden=\"true\">← Previous</span>";
  const next = page < pages ? `<a href=\"${escapePublicHtml(makeHref(page + 1))}\">Next →</a>` : "<span aria-hidden=\"true\">Next →</span>";
  return `<nav class=\"tc-pagination\" aria-label=\"Pagination\"><span>Page ${Math.min(page, pages)} of ${pages} · ${total} total records</span><span>${previous} ${next}</span></nav>`;
}

function publicHtmlCollection(resource: string, requestedPage: number): string {
  const normalized = publicHtmlCollectionName(resource);
  if (!normalized) return `<section class=\"tc-empty\"><h1>Unknown public resource</h1><p>Choose a versioned public collection from the <a href=\"/evidence\">evidence index</a>.</p></section>`;
  const collection = collectionFor(publicSnapshot, normalized) || [];
  const page = Math.max(1, requestedPage);
  const start = (page - 1) * publicHtmlPageSize;
  const records = collection.filter((_record, index) => index >= start && index < start + publicHtmlPageSize);
  const cards = records.length ? `<div class=\"tc-card-grid\">${records.map(publicHtmlRecordSummary).join("")}</div>` : "<p class=\"tc-empty\">This authoritative collection is empty.</p>";
  return `<div class=\"tc-hero-row\"><div><span class=\"tc-eyebrow\">Public resource collection</span><h1>${escapePublicHtml(normalized)}</h1><p class=\"tc-lede\">Every record in this collection is reachable through a deterministic, read-only route and carries source and evidence provenance.</p></div><div><p class=\"tc-citation\">Snapshot ${escapePublicHtml(publicSnapshot.snapshot.id)}</p><p class=\"tc-citation\">${collection.length} total records · page size ${publicHtmlPageSize}</p></div></div>${publicHtmlPagination(`/resources/${normalized}`, page, collection.length)}<section class=\"tc-panel\"><h2>${escapePublicHtml(normalized)} records</h2>${cards}</section>`;
}

function publicHtmlStackIndex(): string {
  const nodes = [...publicSnapshot.stack_nodes].sort((left, right) => Number(left.ordinal || 0) - Number(right.ordinal || 0));
  return `<div class=\"tc-hero-row\"><div><span class=\"tc-eyebrow\">Public stack explorer</span><h1>Trace every dependency.</h1><p class=\"tc-lede\">All ${nodes.length} declared phases are directly linked to their phase detail and evidence records.</p></div><div><p class=\"tc-citation\">${nodes.length} phases · snapshot ${escapePublicHtml(publicSnapshot.snapshot.commit)}</p></div></div><section class=\"tc-panel\"><h2>Stack phases (${nodes.length})</h2><div class=\"tc-card-grid\">${nodes.map((node) => `<article class=\"tc-card\"><span class=\"tc-eyebrow\">Phase ${escapePublicHtml(String(node.ordinal).padStart(2, "0"))}</span><h2><a href=\"${escapePublicHtml(publicHtmlRecordHref(node))}\">${escapePublicHtml(publicRecordName(node))}</a></h2><p>${escapePublicHtml(node.problem || node.description || "Repository-backed stack phase.")}</p><p class=\"tc-citation\">${escapePublicHtml(node.implementation_status || "unrecorded")} · <a href=\"${escapePublicHtml(publicHtmlEvidenceHref(node))}\">evidence</a></p></article>`).join("")}</div></section>`;
}

function publicHtmlStackDetail(slug: string): string {
  const node = publicSnapshot.stack_nodes.find((candidate) => String(candidate.slug || "") === slug || candidate.id.endsWith(`:${slug}`));
  if (!node) return `<section class=\"tc-empty\"><h1>Stack phase not found</h1><p><a href=\"/stack\">Return to the complete stack explorer.</a></p></section>`;
  const dependents = Array.isArray(node.dependent_ids) ? node.dependent_ids : [];
  const dependencies = Array.isArray(node.depends_on) ? node.depends_on : [];
  return `<div class=\"tc-detail\"><header class=\"tc-detail-header\"><span class=\"tc-eyebrow\">Phase ${escapePublicHtml(String(node.ordinal).padStart(2, "0"))}</span><h1>${escapePublicHtml(publicRecordName(node))}</h1><p class=\"tc-lede\">${escapePublicHtml(node.problem || node.description || "No problem statement recorded.")}</p><span class=\"tc-detail-id\">${escapePublicHtml(node.id)} · implementation ${escapePublicHtml(node.implementation_status || "unrecorded")}</span></header><section class=\"tc-panel\"><h2>Phase contract</h2><dl>${[["Problem", node.problem || node.description], ["Inputs", node.inputs], ["Outputs", node.outputs], ["Implementation status", node.implementation_status]].map(([key, value]) => `<div><dt>${escapePublicHtml(key)}</dt><dd>${publicHtmlValue(value)}</dd></div>`).join("")}</dl></section><section class=\"tc-panel\"><h2>Dependencies (${dependencies.length})</h2>${dependencies.length ? `<ul>${dependencies.map((id) => { const target = publicEntityById(String(id)); return `<li>${target ? `<a href=\"${escapePublicHtml(publicHtmlRecordHref(target))}\">${escapePublicHtml(publicRecordName(target))}</a>` : escapePublicHtml(id)}</li>`; }).join("")}</ul>` : "<p class=\"tc-empty\">This is the root phase.</p>"}</section><section class=\"tc-panel\"><h2>Dependent phases (${dependents.length})</h2>${dependents.length ? `<ul>${dependents.map((id) => { const target = publicEntityById(String(id)); return `<li>${target ? `<a href=\"${escapePublicHtml(publicHtmlRecordHref(target))}\">${escapePublicHtml(publicRecordName(target))}</a>` : escapePublicHtml(id)}</li>`; }).join("")}</ul>` : "<p class=\"tc-empty\">No later phase declares this phase as a dependency.</p>"}</section><section class=\"tc-panel\"><h2>Phase records</h2><dl>${["component_ids", "capability_ids", "contract_ids", "decision_ids", "test_ids", "benchmark_ids", "gap_ids", "release_ids"].map((key) => `<div><dt>${key}</dt><dd>${publicHtmlValue(node[key])}</dd></div>`).join("")}</dl></section><section class=\"tc-panel\"><h2>Source provenance</h2>${publicHtmlSourceRefs(node.source_refs)}</section><section class=\"tc-panel\"><h2>Evidence provenance</h2>${publicHtmlSourceRefs(node.evidence_refs)}</section></div>`;
}

function publicHtmlEvidenceIndex(): string {
  return `<div class=\"tc-hero-row\"><div><span class=\"tc-eyebrow\">Evidence index</span><h1>Follow every public claim to its source.</h1><p class=\"tc-lede\">Browse complete versioned collections, then open any record's evidence route for source spans, artifact hashes, and relationship context.</p></div><div><p class=\"tc-citation\">${publicCollectionNames.reduce((sum, resource) => sum + (publicSnapshot[resource] || []).length, 0)} public records · ${(publicSnapshot.relations || []).length} relationships</p></div></div><div class=\"tc-card-grid\">${publicCollectionNames.map((resource) => `<article class=\"tc-card\"><span class=\"tc-eyebrow\">${resource}</span><h2>${publicSnapshot[resource].length} records</h2><p>Every record has a deterministic collection route and evidence route.</p><p><a href=\"/resources/${resource}\">Browse ${resource} →</a></p></article>`).join("")}</div>`;
}

function publicHtmlSearch(req: Request): string {
  const query = String(req.query.q || "").trim();
  const mode = normalizeSearchMode(String(req.query.mode || "semantic"));
  const requestedPage = Number.parseInt(String(req.query.page || "1"), 10);
  const page = Number.isFinite(requestedPage) ? Math.max(1, requestedPage) : 1;
  const results = query ? searchSnapshot(publicSnapshot, query, mode) : [];
  const start = (page - 1) * publicHtmlPageSize;
  const visible = results.filter((_result, index) => index >= start && index < start + publicHtmlPageSize);
  const cards = visible.map((result) => {
    const record = publicEntityById(result.entity_id);
    const href = record ? publicHtmlRecordHref(record) : `/search?q=${encodeURIComponent(result.entity_id)}&mode=exact`;
    return `<article class=\"tc-card\"><span class=\"tc-eyebrow\">${escapePublicHtml(result.entity_type)} · score ${escapePublicHtml(result.score)}</span><h2><a href=\"${escapePublicHtml(href)}\">${escapePublicHtml(result.name)}</a></h2><p>${escapePublicHtml(result.matched_fields.join(", "))} match · ${escapePublicHtml(result.match_type)} search</p><p>${escapePublicHtml(result.match_reason)}</p>${publicHtmlSourceRefs(result.provenance)}</article>`;
  }).join("");
  return `<div class=\"tc-hero-row\"><div><span class=\"tc-eyebrow\">Provenance search</span><h1>Find the exact boundary.</h1><p class=\"tc-lede\">Search IDs, paths, symbols, relationships, and semantic concepts across the complete public inventory.</p></div><div><form class=\"tc-search\" method=\"get\" action=\"/search\"><label for=\"public-search\">Search the public graph</label><input id=\"public-search\" name=\"q\" value=\"${escapePublicHtml(query)}\" placeholder=\"Search source, symbol, contract, gap…\"><select name=\"mode\"><option value=\"semantic\"${mode === "semantic" ? " selected" : ""}>Semantic</option><option value=\"exact\"${mode === "exact" ? " selected" : ""}>Exact</option><option value=\"symbol\"${mode === "symbol" ? " selected" : ""}>Symbol</option><option value=\"relationship\"${mode === "relationship" ? " selected" : ""}>Relationship</option></select><button type=\"submit\">Search</button></form></div></div><div class=\"tc-section-heading\"><h2>${query ? `Results for “${escapePublicHtml(query)}”` : "Search the public graph"}</h2><span class=\"tc-muted\">${results.length} total matches · ${escapePublicHtml(mode)}</span></div>${query ? publicHtmlPagination(`/search?q=${encodeURIComponent(query)}&mode=${mode}`, page, results.length) : ""}${cards ? `<div class=\"tc-card-grid\">${cards}</div>` : `<p class=\"tc-empty\">${query ? "No authoritative records matched this query." : "Enter a query to search the snapshot."}</p>`}`;
}

function sendPublicPage(req: Request, res: Response, kind: "home" | "stack" | "stack-detail" | "resource" | "resource-detail" | "evidence" | "evidence-detail" | "search"): void {
  if (publicSnapshot.snapshot.commit === "unknown" || !publicSnapshot.stack_nodes.length) {
    res.status(503).type("html").send(publicHtmlPageChrome("Snapshot unavailable", `<section class=\"tc-error\"><h1>Public snapshot unavailable</h1><p>The repository-backed public snapshot has not been generated.</p></section>`));
    return;
  }
  let title = "Overview";
  let body = "";
  if (kind === "home") {
    title = "Overview";
    body = `<div class=\"tc-hero-row\"><div><span class=\"tc-eyebrow\">Public platform knowledge</span><h1>Understand the stack. Follow the evidence.</h1><p class=\"tc-lede\">A static-first map of Trit from silicon to user surfaces. Every layer stays connected to the source, contract, test, benchmark, decision, release, and gap that qualify its claims.</p></div><div><p class=\"tc-citation\">Snapshot ${escapePublicHtml(publicSnapshot.snapshot.id)} · commit ${escapePublicHtml(publicSnapshot.snapshot.commit)}</p></div></div><div class=\"tc-metrics\" aria-label=\"Snapshot metrics\"><div class=\"tc-metric\"><strong>${publicSnapshot.stack_nodes.length}</strong><span>stack phases</span></div><div class=\"tc-metric\"><strong>${publicSnapshot.capabilities.length}</strong><span>capabilities</span></div><div class=\"tc-metric\"><strong>${publicSnapshot.contracts.length}</strong><span>contracts</span></div><div class=\"tc-metric\"><strong>${publicSnapshot.sources.length}</strong><span>source records</span></div></div><section class=\"tc-panel\"><h2>Complete public graph</h2><p>All public collections, relationship edges, and evidence records are available from the <a href=\"/evidence\">evidence index</a>.</p><p><a href=\"/stack\">Open all stack phases →</a> · <a href=\"/search\">Search the complete inventory →</a></p></section>`;
  } else if (kind === "stack") {
    title = "Stack Explorer";
    body = publicHtmlStackIndex();
  } else if (kind === "stack-detail") {
    title = "Stack phase";
    body = publicHtmlStackDetail(String(req.params.slug || ""));
  } else if (kind === "resource" || kind === "resource-detail") {
    const resource = String(req.params.resource || "");
    if (kind === "resource-detail") {
      const collection = collectionFor(publicSnapshot, resource) || [];
      const record = collection.find((candidate) => candidate.id === String(req.params.id || "")) || null;
      title = record ? publicRecordName(record) : "Record not found";
      body = record ? publicHtmlRecordDetail(record) : `<section class=\"tc-error\"><h1>Public record not found</h1><p><a href=\"/resources/${escapePublicHtml(resource)}\">Return to the collection.</a></p></section>`;
      if (!record) res.status(404);
    } else {
      title = resource;
      body = publicHtmlCollection(resource, Number.parseInt(String(req.query.page || "1"), 10) || 1);
    }
  } else if (kind === "evidence" || kind === "evidence-detail") {
    if (kind === "evidence-detail") {
      const record = publicEntityById(String(req.params.id || ""));
      title = record ? `Evidence · ${publicRecordName(record)}` : "Evidence not found";
      body = record ? publicHtmlRecordDetail(record, true) : `<section class=\"tc-error\"><h1>Evidence record not found</h1><p><a href=\"/evidence\">Return to the evidence index.</a></p></section>`;
      if (!record) res.status(404);
    } else {
      title = "Evidence";
      body = publicHtmlEvidenceIndex();
    }
  } else {
    title = "Search";
    body = publicHtmlSearch(req);
  }
  res.setHeader("Cache-Control", "public, max-age=60");
  res.type("html").send(publicHtmlPageChrome(title, body));
}

function registerPublicApi(prefix: string) {
  app.get(prefix, (_req: Request, res: Response) => {
    if (!publicSnapshotAvailable()) {
      publicError(res, 503, "snapshot_unavailable", "The repository-backed public snapshot is unavailable.", prefix);
      return;
    }
    const counts = Object.fromEntries([...publicResources].map((resource) => [resource, collectionFor(publicSnapshot, resource)?.length || 0]));
    res.json(publicEnvelope({ api_schema: PUBLIC_API_SCHEMA_VERSION, snapshot_schema: publicSnapshot.schema_version, resources: counts }, "", prefix, { read_only: true }));
  });

  app.get(`${prefix}/snapshot.json`, (_req: Request, res: Response) => {
    if (!publicSnapshotAvailable()) {
      publicError(res, 503, "snapshot_unavailable", "The repository-backed public snapshot is unavailable.", prefix);
      return;
    }
    res.setHeader("Cache-Control", "public, max-age=60");
    res.json(publicSnapshot);
  });

  for (const artifact of ["coverage", "relationship-index", "freshness"] as const) {
    app.get(`${prefix}/${artifact}.json`, (_req: Request, res: Response) => {
      if (!publicSnapshotAvailable()) {
        publicError(res, 503, "snapshot_unavailable", "The repository-backed public snapshot is unavailable.", prefix);
        return;
      }
      const data = artifact === "coverage" ? publicSnapshot.coverage : artifact === "relationship-index" ? publicRelationshipIndex : publicSnapshot.freshness;
      if (!data) {
        publicError(res, 404, "artifact_unavailable", `The public ${artifact} artifact is unavailable.`, prefix);
        return;
      }
      res.setHeader("Cache-Control", "public, max-age=60");
      res.json(publicEnvelope(data, artifact, prefix, { read_only: true }));
    });
  }

  app.get(`${prefix}/openapi.json`, (_req: Request, res: Response) => {
    const contractCandidates = [
      path.join(__dirname, "public", "api", "v1", "openapi.json"),
      path.join(process.cwd(), "public", "api", "v1", "openapi.json"),
      path.join(process.cwd(), "treatcode", "public", "api", "v1", "openapi.json"),
    ];
    for (const candidate of contractCandidates) {
      try {
        const contract = JSON.parse(fs.readFileSync(candidate, "utf8"));
        res.json({ ...contract, "x-treatcode-schema-version": PUBLIC_API_SCHEMA_VERSION, "x-treatcode-source-snapshot": publicSnapshot.snapshot });
        return;
      } catch {
        // Keep looking for the generated contract.
      }
    }
    publicError(res, 404, "contract_unavailable", "The generated OpenAPI contract is unavailable.", prefix);
  });

  app.get(`${prefix}/search`, (req: Request, res: Response) => {
    if (!publicSnapshotAvailable()) {
      publicError(res, 503, "snapshot_unavailable", "The repository-backed public snapshot is unavailable.", prefix);
      return;
    }
    const query = String(req.query.q || "").trim();
    if (!query) {
      publicError(res, 400, "query_required", "Search requires a non-empty q parameter.", prefix);
      return;
    }
    const mode = normalizeSearchMode(String(req.query.mode || "semantic"));
    const requestedLimit = Number.parseInt(String(req.query.limit || "25"), 10);
    const requestedOffset = Number.parseInt(String(req.query.offset || "0"), 10);
    const limit = Number.isFinite(requestedLimit) ? Math.max(1, Math.min(100, requestedLimit)) : 25;
    const offset = Number.isFinite(requestedOffset) ? Math.max(0, requestedOffset) : 0;
    const allResults = searchSnapshot(publicSnapshot, query, mode);
    const results = allResults.slice(offset, offset + limit);
    res.json({ ...publicEnvelope(results, "search", prefix, { count: results.length, total: allResults.length, query, mode, offset, limit }), links: publicPaginationLinks(prefix, "search", offset, limit, allResults.length, { q: query, mode }) });
  });

  app.get(`${prefix}/:resource/:id/relations`, (req: Request, res: Response) => {
    if (!publicSnapshotAvailable()) {
      publicError(res, 503, "snapshot_unavailable", "The repository-backed public snapshot is unavailable.", prefix);
      return;
    }
    const resource = String(req.params.resource);
    if (!isPublicResource(resource)) {
      publicError(res, 404, "resource_not_found", `Unknown public resource: ${resource}.`, prefix);
      return;
    }
    const collection = collectionFor(publicSnapshot, resource) || [];
    const record = collection.find((candidate) => candidate.id === req.params.id) || null;
    if (!record) {
      publicError(res, 404, "entity_not_found", `No ${resource} entity exists for id ${req.params.id}.`, prefix);
      return;
    }
    const relations = (publicSnapshot.relations || []).filter((relation) => relation.from === record.id || relation.to === record.id);
    res.json(publicEnvelope(relations, `${resource}/${encodeURIComponent(record.id)}/relations`, prefix, { count: relations.length, total: relations.length }));
  });

  app.get(`${prefix}/:resource/:id`, (req: Request, res: Response) => {
    if (!publicSnapshotAvailable()) {
      publicError(res, 503, "snapshot_unavailable", "The repository-backed public snapshot is unavailable.", prefix);
      return;
    }
    const resource = String(req.params.resource);
    if (!isPublicResource(resource)) {
      publicError(res, 404, "resource_not_found", `Unknown public resource: ${resource}.`, prefix);
      return;
    }
    const collection = collectionFor(publicSnapshot, resource);
    const record = collection?.find((candidate) => candidate.id === req.params.id) || null;
    if (!record) {
      publicError(res, 404, "entity_not_found", `No ${resource} entity exists for id ${req.params.id}.`, prefix);
      return;
    }
    res.json(publicEnvelope(record, resource, prefix));
  });

  app.get(`${prefix}/:resource`, (req: Request, res: Response, next) => {
    if (!publicSnapshotAvailable()) {
      publicError(res, 503, "snapshot_unavailable", "The repository-backed public snapshot is unavailable.", prefix);
      return;
    }
    const resource = String(req.params.resource);
    if (resource.endsWith(".json")) {
      next();
      return;
    }
    if (!isPublicResource(resource)) {
      publicError(res, 404, "resource_not_found", `Unknown public resource: ${resource}.`, prefix);
      return;
    }
    const collection = collectionFor(publicSnapshot, resource) || [];
    const requestedLimit = Number.parseInt(String(req.query.limit || "25"), 10);
    const requestedOffset = Number.parseInt(String(req.query.offset || "0"), 10);
    const limit = Number.isFinite(requestedLimit) ? Math.max(1, Math.min(100, requestedLimit)) : 25;
    const offset = Number.isFinite(requestedOffset) ? Math.max(0, requestedOffset) : 0;
    const data = collection.slice(offset, offset + limit);
    res.json({ ...publicEnvelope(data, resource, prefix, { count: data.length, total: collection.length, offset, limit }), links: publicPaginationLinks(prefix, resource, offset, limit, collection.length) });
  });
}

// Register the workspace alias before the generic public `/api/v1/:resource`
// matcher so the versioned workspace collection cannot be mistaken for a
// read-only public resource.
registerWorkspaceApi("/api/workspaces/v1");
registerWorkspaceApi("/api/v1/workspaces", "/api/v1/workspaces");
registerPublicApi("/api/public/v1");
registerPublicApi("/api/v1");

// P10 benchmark evidence is read-only and comes from the repository's
// versioned manifest/reference artifact. Candidate execution remains owned by
// the isolated runner surface; this endpoint never executes submitted code.
function loadP10Artifact(relativePath: string): Record<string, unknown> | null {
  const candidates = [
    path.join(process.cwd(), relativePath),
    path.join(process.cwd(), "..", relativePath),
    path.join(__dirname, relativePath),
    path.join(__dirname, "..", relativePath),
  ];
  for (const candidate of candidates) {
    try {
      return JSON.parse(fs.readFileSync(candidate, "utf8")) as Record<string, unknown>;
    } catch {
      // Try the next workspace layout used by Bun, node, and the built server.
    }
  }
  return null;
}

app.get("/api/benchmarks/p10", (_req: Request, res: Response) => {
  const manifest = loadP10Artifact("BENCHMARK_MANIFEST.json");
  const reference = loadP10Artifact(path.join("benchmarks", "reference", "p10-reference.v1.json"));
  if (!manifest || !reference) {
    res.status(503).json({ schema: "treatcode.p10_benchmark_api.v1", error: "P10 benchmark evidence is unavailable." });
    return;
  }
  res.setHeader("Cache-Control", "public, max-age=60");
  res.json({
    schema: "treatcode.p10_benchmark_api.v1",
    manifest,
    reference,
    provenance: {
      manifest: "BENCHMARK_MANIFEST.json",
      protocol: "BENCHMARK_PROTOCOL_SCHEMA.json",
      reference: "benchmarks/reference/p10-reference.v1.json",
      read_only: true,
    },
  });
});

// P07 identity and authorization are shared by the UI-facing action routes and
// the versioned auth API. Credentials are opaque, short-lived, and stored only
// as hashes; the raw value is returned exactly once from login or task issuance.
export const authStore: AuthStore = createDefaultAuthStore({
  audit_path: process.env.TREATCODE_AUTH_AUDIT_PATH || path.resolve(__dirname, "..", "build", "treatcode-auth", "audit.jsonl"),
  identity_path: process.env.TREATCODE_AUTH_STATE_PATH || path.resolve(__dirname, "..", "build", "treatcode-auth", "state.json"),
});

// Participant artifacts are durable and intentionally kept separate from the
// public snapshot and the operator control-plane state.
export const communityStore = new CommunityStore({
  state_path: process.env.TREATCODE_COMMUNITY_STATE_PATH || path.resolve(__dirname, "..", "build", "treatcode-community", "state.json"),
});

function authToken(req: Request): string | null {
  return bearerToken(req.header("authorization"));
}

function requestProject(req: Request, body?: Record<string, unknown>): string {
  return String(req.header("x-treatcode-project") || body?.project_id || DEFAULT_PROJECT_ID);
}

function requestTask(req: Request, body?: Record<string, unknown>): string | null {
  const value = req.header("x-treatcode-task") || body?.task_id;
  return value ? String(value) : null;
}

function requestNonce(req: Request): string | null {
  return req.header("x-action-nonce") || req.header("idempotency-key") || null;
}

function authErrorResponse(res: Response, denial: AuthorizationDenial): void {
  res.status(denial.http_status).json({
    schema_version: AUTH_API_SCHEMA_VERSION,
    error: {
      code: denial.code,
      reason: denial.reason,
      action: denial.action,
      project_id: denial.project_id,
      task_id: denial.task_id,
      audit_event_id: denial.audit_event_id,
      retryable: denial.retryable,
    },
    meta: { policy_version: "treatcode.authz.policy.v1" },
  });
}

function authDataResponse(res: Response, data: unknown, status = 200): void {
  res.status(status).json({
    schema_version: AUTH_API_SCHEMA_VERSION,
    data,
    meta: { policy_version: "treatcode.authz.policy.v1" },
  });
}

function requireAction(
  req: Request,
  res: Response,
  action: PermissionAction,
  options: { project_id?: string; task_id?: string | null; require_nonce?: boolean } = {},
) {
  const decision = authStore.authorize({
    token: authToken(req),
    action,
    project_id: options.project_id || requestProject(req, req.body),
    task_id: options.task_id === undefined ? requestTask(req, req.body) : options.task_id,
    request_nonce: requestNonce(req),
    resource: req.path,
    require_nonce: options.require_nonce,
  });
  if (!decision.allowed) {
    authErrorResponse(res, decision.denial);
    return null;
  }
  return decision;
}

app.get("/api/auth/v1", (_req: Request, res: Response) => {
  authDataResponse(res, {
    api_schema: AUTH_API_SCHEMA_VERSION,
    policy_version: "treatcode.authz.policy.v1",
    actions: actionCatalog(),
    identity_kinds: ["human", "collaborator", "service", "agent"],
    links: {
      login: "/api/auth/v1/login",
      capabilities: "/api/auth/v1/capabilities",
      openapi: "/api/auth/v1/openapi.json",
      audit: "/api/auth/v1/audit",
    },
  });
});

app.get("/api/auth/v1/openapi.json", (_req: Request, res: Response) => {
  const candidates = [
    path.resolve(__dirname, "..", "docs", "11_TreatCode_Platform", "schemas", "auth_api.v1.openapi.json"),
    path.resolve(process.cwd(), "docs", "11_TreatCode_Platform", "schemas", "auth_api.v1.openapi.json"),
  ];
  for (const candidate of candidates) {
    try {
      res.json(JSON.parse(fs.readFileSync(candidate, "utf8")));
      return;
    } catch {
      // Continue to the next source location.
    }
  }
  res.status(404).json({ schema_version: AUTH_API_SCHEMA_VERSION, error: { code: "contract_unavailable", reason: "The auth API contract is unavailable." } });
});

function participantAuthError(res: Response, error: unknown): void {
  const message = String((error as Error)?.message || "registration_invalid");
  const code = message === "duplicate_handle" ? "duplicate_handle" : message.startsWith("invalid_") || message.endsWith("_invalid") || message.includes("required") ? "registration_invalid" : "registration_invalid";
  const status = message === "duplicate_handle" ? 409 : 400;
  res.status(status).json({
    schema_version: AUTH_API_SCHEMA_VERSION,
    error: { code, reason: code === "duplicate_handle" ? "That participant handle is already registered." : "The participant account details are invalid." },
    meta: { policy_version: "treatcode.authz.policy.v1" },
  });
}

function publicSnapshotAvailable(): boolean {
  return publicSnapshot.snapshot.commit !== "unknown" && publicSnapshot.stack_nodes.length > 0;
}

function publicRequestHref(prefix: string, resource: string, query: Record<string, string | number | undefined> = {}): string {
  const params = new URLSearchParams();
  for (const [key, value] of Object.entries(query)) if (value !== undefined) params.set(key, String(value));
  const serialized = params.toString();
  return `${prefix}/${resource}${serialized ? `?${serialized}` : ""}`;
}

function publicPaginationLinks(prefix: string, resource: string, offset: number, limit: number, total: number, query: Record<string, string | number | undefined> = {}): Record<string, string> {
  const links: Record<string, string> = { self: publicRequestHref(prefix, resource, { ...query, offset, limit }) };
  if (offset > 0) links.previous = publicRequestHref(prefix, resource, { ...query, offset: Math.max(0, offset - limit), limit });
  if (offset + limit < total) links.next = publicRequestHref(prefix, resource, { ...query, offset: offset + limit, limit });
  return links;
}

function loginHandler(req: Request, res: Response): void {
  const body = (req.body || {}) as Record<string, unknown>;
  const result = typeof body.handle === "string"
    ? authStore.login({ handle: body.handle, password: typeof body.password === "string" ? body.password : "" })
    : authStore.login(String(body.identity_id || ""), typeof body.access_key === "string" ? body.access_key : "");
  if (!result.ok) {
    authErrorResponse(res, result.denial);
    return;
  }
  res.setHeader("Cache-Control", "no-store");
  authDataResponse(res, { identity: result.identity, credential: result.credential });
}

function registerParticipantHandler(req: Request, res: Response): void {
  const body = (req.body || {}) as Record<string, unknown>;
  if (typeof body.handle !== "string" || typeof body.password !== "string") {
    participantAuthError(res, new Error("registration_invalid"));
    return;
  }
  try {
    const result = authStore.registerAndLoginParticipant({
      handle: body.handle,
      password: body.password,
      display_name: typeof body.display_name === "string" ? body.display_name : undefined,
    });
    res.setHeader("Cache-Control", "no-store");
    authDataResponse(res, { identity: result.identity, participant: result.identity, credential: result.credential }, 201);
  } catch (error) {
    participantAuthError(res, error);
  }
}

app.post("/api/auth/v1/login", loginHandler);
app.post("/api/auth/v1/register", registerParticipantHandler);
app.post("/api/auth/v1/participants/login", loginHandler);
app.post("/api/auth/v1/participants/register", registerParticipantHandler);
app.post("/api/intelligence/v1/accounts/login", loginHandler);
app.post("/api/intelligence/v1/accounts/register", registerParticipantHandler);
app.post("/api/intelligence/accounts/login", loginHandler);
app.post("/api/intelligence/accounts/register", registerParticipantHandler);

app.get("/api/auth/v1/session", (req: Request, res: Response) => {
  const decision = requireAction(req, res, "read", { require_nonce: false });
  if (!decision) return;
  authDataResponse(res, { identity: decision.actor, credential: decision.credential });
});

app.get("/api/auth/v1/capabilities", (req: Request, res: Response) => {
  const result = authStore.capabilities({ token: authToken(req), project_id: requestProject(req), task_id: requestTask(req) });
  if (!result.ok) {
    authErrorResponse(res, result.denial);
    return;
  }
  authDataResponse(res, result);
});

app.post("/api/auth/v1/check", (req: Request, res: Response) => {
  const rawAction = String(req.body?.action || "");
  if (!(PERMISSION_ACTIONS as readonly string[]).includes(rawAction)) {
    res.status(400).json({ schema_version: AUTH_API_SCHEMA_VERSION, error: { code: "invalid_scope", reason: "action must be one of the published independent permission actions." } });
    return;
  }
  const decision = requireAction(req, res, rawAction as PermissionAction, {
    project_id: requestProject(req, req.body),
    task_id: requestTask(req, req.body),
    require_nonce: false,
  });
  if (!decision) return;
  authDataResponse(res, { allowed: true, action: rawAction, identity: decision.actor, credential: decision.credential });
});

app.post("/api/auth/v1/tasks", (req: Request, res: Response) => {
  const result = authStore.issueTaskCredential({
    requester_token: authToken(req),
    target_identity_id: String(req.body?.target_identity_id || ""),
    project_id: requestProject(req, req.body),
    task_id: requestTask(req, req.body) || "",
    actions: Array.isArray(req.body?.actions) ? req.body.actions : [],
    ttl_seconds: Number(req.body?.ttl_seconds),
    request_nonce: requestNonce(req),
  });
  if (!result.ok) {
    authErrorResponse(res, result.denial);
    return;
  }
  res.setHeader("Cache-Control", "no-store");
  authDataResponse(res, { identity: result.identity, credential: result.credential }, 201);
});

app.post("/api/auth/v1/credentials/:credentialId/revoke", (req: Request, res: Response) => {
  const result = authStore.revokeCredential({
    requester_token: authToken(req),
    credential_id: String(req.params.credentialId),
    project_id: requestProject(req, req.body),
    task_id: requestTask(req, req.body),
    request_nonce: requestNonce(req),
  });
  if (!result.ok) {
    authErrorResponse(res, result.denial);
    return;
  }
  authDataResponse(res, { credential: result.credential });
});

app.get("/api/auth/v1/audit", (req: Request, res: Response) => {
  const result = authStore.auditEvents({ token: authToken(req), project_id: requestProject(req), task_id: requestTask(req) });
  if (!result.ok) {
    authErrorResponse(res, result.denial);
    return;
  }
  authDataResponse(res, { events: result.events });
});

// P14 intelligence and participant community routes.  These adapters keep
// authentication, durable artifacts, and the sealed verifier separate while
// accepting the stable `/v1` contract plus the short aliases used by the UI.
function intelligenceErrorResponse(res: Response, error: unknown): void {
  if (error instanceof IntelligenceServiceError || error instanceof CommunityStoreError) {
    res.status(error.status).json({
      schema_version: error instanceof IntelligenceServiceError ? "treatcode.intelligence.api.v1" : COMMUNITY_API_SCHEMA_VERSION,
      error: { code: error.code, reason: error.message, ...(error instanceof IntelligenceServiceError && error.details ? { details: error.details } : {}) },
    });
    return;
  }
  res.status(500).json({ schema_version: "treatcode.intelligence.api.v1", error: { code: "internal_error", reason: "The intelligence request could not be completed." } });
}

function intelligenceAction(req: Request, res: Response, action: PermissionAction, requireNonce = true) {
  // The public benchmark id is not an auth task-scope identifier (auth task
  // scopes use `tc:task:*`). Participant credentials are project-wide, so the
  // adapter deliberately authorizes these routes with a null task scope.
  return requireAction(req, res, action, { project_id: DEFAULT_PROJECT_ID, task_id: null, require_nonce: requireNonce });
}

function latestParticipantSolution(taskId: string, identityId: string) {
  return communityStore.listSolutionRevisions({ task_id: taskId, owner_identity_id: identityId, limit: 1 })[0] || null;
}

function publicSolutionView(value: ReturnType<typeof latestParticipantSolution>) {
  if (!value) return null;
  return {
    ...value,
    id: value.solution_id,
    version: value.revision,
    updated_at: value.updated_at,
  };
}

function publicPostedSolutionView(value: ReturnType<CommunityStore["listPublicSolutions"]>[number]) {
  return {
    schema_version: COMMUNITY_API_SCHEMA_VERSION,
    id: value.solution_id,
    solution_id: value.solution_id,
    version: value.revision,
    revision: value.revision,
    problem_id: value.problem_id,
    challenge_id: value.challenge_id,
    task_id: value.task_id,
    title: value.title,
    code: value.code,
    language: value.language,
    owner_handle: value.owner_handle,
    created_at: value.created_at,
    updated_at: value.updated_at,
    solved: value.solved,
    upvotes: value.upvotes,
    viewer_has_upvoted: value.viewer_has_upvoted,
    discussion: value.discussion_body,
    metrics: value.metrics,
  };
}

function publicTrialView(trial: Record<string, unknown> | null | undefined, hidden?: Record<string, unknown> | null) {
  const aggregate = hidden && typeof hidden.remaining_trials === "number" && hidden.remaining_trials === 0 && hidden.aggregate && typeof hidden.aggregate === "object" ? hidden.aggregate as Record<string, unknown> : null;
  const score = aggregate && typeof aggregate.score === "number" ? aggregate.score : null;
  return {
    ...(trial || {}),
    trial_id: trial?.id,
    label: trial?.id,
    // A trial is binary, while the aggregate is a run-level score.  Never
    // copy the run percentage onto every trial row (75% must not look like
    // four separate 75% trials).
    score: null,
    aggregate_score: score,
    aggregate_score_scope: "task_trial_reliability",
    sealed: true,
  };
}

function intelligenceLeaderboardView(view: "official" | "self-reported", taskId?: string) {
  const rows = taskId
    ? intelligenceService.taskService(taskId).leaderboard(view)
    : view === "official" ? intelligenceService.getOfficialLeaderboard() : intelligenceService.getSelfReportedLeaderboard();
  return rows.map((row) => {
    const participantId = "participant_id" in row ? row.participant_id : undefined;
    const identity = participantId ? authStore.identity(participantId) : null;
    return { ...row, ...(identity?.handle ? { handle: identity.handle } : {}) };
  });
}

const intelligenceRunByParticipant = new Map<string, string>();

function intelligenceRunKey(participantId: string, taskId: string): string {
  return `${participantId}\u0000${taskId}`;
}

function serviceRunForActor(run: Awaited<ReturnType<typeof intelligenceService.getRun>>, actorId: string, privileged = false): void {
  if (!privileged && run.participant_id !== actorId) {
    throw new IntelligenceServiceError("run_not_found", "Intelligence run not found", 404);
  }
}

function routeAliases(paths: string[], register: (path: string) => void): void {
  for (const route of paths) register(route);
}

// Public benchmark catalog and leaderboard views.
routeAliases(["/api/intelligence/v1/benchmark", "/api/intelligence/benchmark", "/api/intelligence"], (route) => {
  app.get(route, (req: Request, res: Response) => {
    try {
      const requestedTaskId = typeof req.query.task_id === "string" ? req.query.task_id : typeof req.query.taskId === "string" ? req.query.taskId : INTELLIGENCE_TASK_ID;
      const catalog = intelligenceService.catalog(requestedTaskId);
      const suite = intelligenceService.suiteCatalog();
      const modelSuiteLeaderboard = intelligenceService.getModelSuiteLeaderboard();
      res.setHeader("Cache-Control", "public, max-age=30");
      res.json({
        schema_version: "treatcode.intelligence.api.v1",
        data: {
          suite,
          tasks: suite.tasks,
          task: catalog.task,
          task_id: catalog.task.id,
          public_tests: catalog.public_tests,
          hidden_tests: catalog.hidden_tests,
          public_leaderboard: [],
           official_leaderboard: intelligenceLeaderboardView("official"),
           self_reported_leaderboard: intelligenceLeaderboardView("self-reported"),
           model_suite_leaderboard: modelSuiteLeaderboard,
        },
        suite,
        tasks: suite.tasks,
        task: catalog.task,
        public_tests: catalog.public_tests,
        hidden_tests: catalog.hidden_tests,
         official_leaderboard: intelligenceLeaderboardView("official"),
         self_reported_leaderboard: intelligenceLeaderboardView("self-reported"),
         model_suite_leaderboard: modelSuiteLeaderboard,
        protocol_schema: catalog.protocol_schema,
        manifest_schema: INTELLIGENCE_MANIFEST_SCHEMA,
      });
    } catch (error) {
      intelligenceErrorResponse(res, error);
    }
  });
});

// V3 is intentionally additive. V1/v2 remain the end-to-end regression API;
// this catalog exposes the independent-task protocol and honest authoring
// readiness without presenting candidate briefs as executable model scores.
app.get("/api/intelligence/v3/catalog", (_req: Request, res: Response) => {
  res.setHeader("Cache-Control", "public, max-age=30");
  const catalog = intelligenceV3CatalogService.catalog();
  res.json({ schema_version: "treatcode.intelligence.api.v3", data: { catalog }, catalog });
});

app.get("/api/intelligence/v3/protocol", (_req: Request, res: Response) => {
  res.setHeader("Cache-Control", "public, max-age=30");
  res.json({ schema_version: "treatcode.intelligence.api.v3", data: { protocol: intelligenceV3CatalogService.protocol }, protocol: intelligenceV3CatalogService.protocol });
});

app.get("/api/intelligence/v3/discussion-rubric", (_req: Request, res: Response) => {
  res.setHeader("Cache-Control", "public, max-age=30");
  const rubric = intelligenceV3CatalogService.discussionRubric;
  res.json({ schema_version: "treatcode.intelligence.api.v3", data: { rubric }, rubric });
});

app.get("/api/intelligence/v3.1/catalog", (_req: Request, res: Response) => {
  res.setHeader("Cache-Control", "public, max-age=30");
  const catalog = intelligenceV31CatalogService.catalog();
  res.json({ schema_version: "treatcode.intelligence.api.v3.1", data: { catalog }, catalog });
});

app.get("/api/intelligence/v3.1/protocol", (_req: Request, res: Response) => {
  res.setHeader("Cache-Control", "public, max-age=30");
  const protocol = intelligenceV31CatalogService.protocol;
  res.json({ schema_version: "treatcode.intelligence.api.v3.1", data: { protocol }, protocol });
});

app.get("/api/intelligence/v3.1/comparisons/latest", (_req: Request, res: Response) => {
  res.setHeader("Cache-Control", "public, max-age=30");
  const comparison = intelligenceV31CatalogService.latestComparison();
  res.json({ schema_version: "treatcode.intelligence.api.v3.1", data: { comparison }, comparison });
});

// A stable suite endpoint lets clients discover task-specific fixtures before
// creating a run.  It intentionally returns the same public projections as
// the benchmark catalog; sealed hidden inputs never cross this boundary.
routeAliases(["/api/intelligence/v1/suite", "/api/intelligence/suite"], (route) => {
  app.get(route, (_req: Request, res: Response) => {
    try {
      const suite = intelligenceService.suiteCatalog();
      const modelSuiteLeaderboard = intelligenceService.getModelSuiteLeaderboard();
      res.setHeader("Cache-Control", "public, max-age=30");
      res.json({ schema_version: "treatcode.intelligence.api.v1", data: { suite, tasks: suite.tasks, model_suite_leaderboard: modelSuiteLeaderboard }, suite, tasks: suite.tasks, model_suite_leaderboard: modelSuiteLeaderboard });
    } catch (error) {
      intelligenceErrorResponse(res, error);
    }
  });
});

// Keep the catalog/task names discoverable for clients that use the shorter
// resource-oriented route vocabulary. These projections are public and carry
// only starter files/public metadata; hidden verifier cases never cross this
// adapter.
routeAliases(["/api/intelligence/v1/catalog", "/api/intelligence/catalog"], (route) => {
  app.get(route, (_req: Request, res: Response) => {
    try {
      const suite = intelligenceService.suiteCatalog();
      const modelSuiteLeaderboard = intelligenceService.getModelSuiteLeaderboard();
      res.setHeader("Cache-Control", "public, max-age=30");
      res.json({ schema_version: "treatcode.intelligence.api.v1", data: { suite, tasks: suite.tasks, model_suite_leaderboard: modelSuiteLeaderboard }, suite, tasks: suite.tasks, model_suite_leaderboard: modelSuiteLeaderboard });
    } catch (error) {
      intelligenceErrorResponse(res, error);
    }
  });
});

routeAliases(["/api/intelligence/v1/tasks", "/api/intelligence/tasks"], (route) => {
  app.get(route, (req: Request, res: Response) => {
    try {
      const taskId = typeof req.query.task_id === "string" ? req.query.task_id : typeof req.query.taskId === "string" ? req.query.taskId : undefined;
      const suite = intelligenceService.suiteCatalog();
      const tasks = taskId ? [intelligenceService.catalog(taskId)] : suite.tasks;
      res.setHeader("Cache-Control", "public, max-age=30");
      res.json({ schema_version: "treatcode.intelligence.api.v1", data: { tasks, ...(taskId ? { task: tasks[0], task_id: taskId } : {}) }, tasks, ...(taskId ? { task: tasks[0], task_id: taskId } : {}) });
    } catch (error) {
      intelligenceErrorResponse(res, error);
    }
  });
});

routeAliases(["/api/intelligence/v1/tasks/:taskId", "/api/intelligence/tasks/:taskId"], (route) => {
  app.get(route, (req: Request, res: Response) => {
    try {
      const catalog = intelligenceService.catalog(req.params.taskId);
      res.setHeader("Cache-Control", "public, max-age=30");
      res.json({ schema_version: "treatcode.intelligence.api.v1", data: { task: catalog.task, task_id: catalog.task.id, public_tests: catalog.public_tests, hidden_tests: catalog.hidden_tests }, task: catalog.task, task_id: catalog.task.id, public_tests: catalog.public_tests, hidden_tests: catalog.hidden_tests });
    } catch (error) {
      intelligenceErrorResponse(res, error);
    }
  });
});

routeAliases(["/api/intelligence/v1/leaderboard", "/api/intelligence/leaderboard"], (route) => {
  app.get(route, (req: Request, res: Response) => {
    try {
      const taskId = typeof req.query.task_id === "string" ? req.query.task_id : typeof req.query.taskId === "string" ? req.query.taskId : undefined;
      const official = intelligenceLeaderboardView("official", taskId);
      const selfReported = intelligenceLeaderboardView("self-reported", taskId);
      const modelSuiteLeaderboard = intelligenceService.getModelSuiteLeaderboard();
      res.json({
        schema_version: "treatcode.intelligence.api.v1",
        data: { official_leaderboard: official, self_reported_leaderboard: selfReported, model_suite_leaderboard: modelSuiteLeaderboard, ...(taskId ? { task_id: taskId } : {}) },
        ...(taskId ? { task_id: taskId } : {}),
        official_leaderboard: official,
        self_reported_leaderboard: selfReported,
        model_suite_leaderboard: modelSuiteLeaderboard,
      });
    } catch (error) {
      intelligenceErrorResponse(res, error);
    }
  });
});

// Community discussion and explicitly posted practice-solution reads are
// public. Private benchmark solution reads remain account-scoped; all writes
// use the authenticated decision as the owner and never trust a body-supplied
// username/handle.
routeAliases(["/api/intelligence/v1/discussions", "/api/intelligence/discussions", "/api/community/v1/discussions"], (route) => {
  app.get(route, (req: Request, res: Response) => {
    try {
      const taskId = String(req.query.task_id || req.query.challenge_id || req.query.problem_id || INTELLIGENCE_TASK_ID);
      res.json({ schema_version: COMMUNITY_API_SCHEMA_VERSION, data: { discussions: communityStore.listDiscussions({ task_id: taskId, limit: Number(req.query.limit) || undefined }) }, discussions: communityStore.listDiscussions({ task_id: taskId, limit: Number(req.query.limit) || undefined }) });
    } catch (error) {
      intelligenceErrorResponse(res, error);
    }
  });
  app.post(route, (req: Request, res: Response) => {
    const decision = intelligenceAction(req, res, "artifact");
    if (!decision) return;
    try {
      const body = (req.body || {}) as Record<string, unknown>;
      const post = communityStore.addDiscussion(decision, {
        task_id: String(body.task_id || body.challenge_id || body.problem_id || INTELLIGENCE_TASK_ID),
        solution_id: typeof body.solution_id === "string" ? body.solution_id : undefined,
        parent_id: typeof body.parent_id === "string" ? body.parent_id : undefined,
        title: typeof body.title === "string" ? body.title : undefined,
        body: typeof body.body === "string" ? body.body : typeof body.content === "string" ? body.content : "",
        metadata: body.metadata && typeof body.metadata === "object" ? body.metadata as Record<string, unknown> : undefined,
      });
      res.status(201).json({ schema_version: COMMUNITY_API_SCHEMA_VERSION, data: { discussion: post }, discussion: post });
    } catch (error) {
      intelligenceErrorResponse(res, error);
    }
  });
});

routeAliases(["/api/intelligence/v1/solutions", "/api/intelligence/solutions"], (route) => {
  app.get(route, (req: Request, res: Response) => {
    const taskId = String(req.query.task_id || req.query.challenge_id || req.query.problem_id || INTELLIGENCE_TASK_ID);
    const decision = intelligenceAction(req, res, "read", false);
    if (!decision) return;
    try {
      const revisions = communityStore.listSolutionRevisions({ task_id: taskId, owner_identity_id: decision.actor.id });
      const solution = revisions[0] || null;
      res.json({ schema_version: COMMUNITY_API_SCHEMA_VERSION, data: { solution: publicSolutionView(solution), revisions }, solution: publicSolutionView(solution), revisions });
    } catch (error) {
      intelligenceErrorResponse(res, error);
    }
  });
});

// Practice solutions are public only after the author explicitly posts them.
// This route never returns private benchmark drafts or internal owner ids.
app.get("/api/community/v1/solutions", (req: Request, res: Response) => {
  try {
    const requestedTask = req.query.task_id || req.query.challenge_id || req.query.problem_id;
    const taskId = typeof requestedTask === "string" && requestedTask.trim() ? requestedTask : undefined;
    const requestedLimit = Number(req.query.limit);
    const limit = Number.isFinite(requestedLimit) && requestedLimit > 0 ? requestedLimit : undefined;
    const capabilities = authStore.capabilities({ token: authToken(req), project_id: DEFAULT_PROJECT_ID, task_id: null });
    const viewerIdentityId = capabilities.ok ? capabilities.principal?.id : undefined;
    const solutions = communityStore.listPublicSolutions({ task_id: taskId, limit, viewer_identity_id: viewerIdentityId }).map(publicPostedSolutionView);
    res.setHeader("Cache-Control", viewerIdentityId ? "private, max-age=10" : "public, max-age=10");
    res.json({ schema_version: COMMUNITY_API_SCHEMA_VERSION, data: { solutions }, solutions });
  } catch (error) {
    intelligenceErrorResponse(res, error);
  }
});

app.post("/api/community/v1/solutions/:solutionId/vote", (req: Request, res: Response) => {
  const decision = intelligenceAction(req, res, "artifact");
  if (!decision) return;
  try {
    const vote = communityStore.toggleSolutionVote(decision, req.params.solutionId);
    res.json({ schema_version: COMMUNITY_API_SCHEMA_VERSION, data: vote, ...vote });
  } catch (error) {
    intelligenceErrorResponse(res, error);
  }
});

routeAliases(["/api/intelligence/v1/solutions", "/api/intelligence/solutions", "/api/community/v1/solutions"], (route) => {
  app.post(route, (req: Request, res: Response) => {
    const decision = intelligenceAction(req, res, "artifact");
    if (!decision) return;
    try {
      const body = (req.body || {}) as Record<string, unknown>;
      const visibility = body.visibility === "public" ? "public" : "private";
      if (visibility === "public" && route !== "/api/community/v1/solutions") {
        throw new CommunityStoreError("public_post_route_required", "Public practice solutions must be posted through the community solution route.", 400);
      }
      const input = {
        task_id: String(body.task_id || body.challenge_id || body.problem_id || INTELLIGENCE_TASK_ID),
        solution_id: typeof body.solution_id === "string" ? body.solution_id : undefined,
        title: typeof body.title === "string" ? body.title : undefined,
        code: typeof body.code === "string" ? body.code : typeof body.content === "string" ? body.content : "",
        language: typeof body.language === "string" ? body.language : "trit",
        visibility: visibility as "public" | "private",
        metadata: body.metadata && typeof body.metadata === "object" ? body.metadata as Record<string, unknown> : undefined,
      };
      const published = visibility === "public"
        ? communityStore.publishSolution(decision, {
          ...input,
          explanation: typeof body.explanation === "string" ? body.explanation : "",
          pseudocode: typeof body.pseudocode === "string" ? body.pseudocode : undefined,
        })
        : null;
      const solution = published?.solution || communityStore.saveSolution(decision, input);
      const solutionView = publicSolutionView(solution);
      res.status(201).json({
        schema_version: COMMUNITY_API_SCHEMA_VERSION,
        data: { solution: solutionView, revision: solution, ...(published ? { discussion: published.discussion } : {}) },
        solution: solutionView,
        revision: solution,
        ...(published ? { discussion: published.discussion } : {}),
      });
    } catch (error) {
      intelligenceErrorResponse(res, error);
    }
  });
});

// Run lifecycle: start a four-trial session, edit allowlisted files, run the
// repeatable public gate, and consume exactly one hidden submission per trial.
app.post("/api/intelligence/v1/runs", async (req: Request, res: Response) => {
  const decision = intelligenceAction(req, res, "benchmark");
  if (!decision) return;
  try {
    const body = (req.body || {}) as Record<string, unknown>;
    const requestedTaskId = typeof body.task_id === "string" ? body.task_id : typeof body.taskId === "string" ? body.taskId : undefined;
    const requestedCommit = typeof body.source_commit === "string" ? body.source_commit : typeof body.sourceCommit === "string" ? body.sourceCommit : publicSnapshot.snapshot.commit || undefined;
    const run = await intelligenceService.startRun({
      task_id: requestedTaskId,
      participant_id: decision.actor.id,
      source_commit: requestedCommit,
      evaluation_kind: body.evaluation_kind === "model_rollout" || body.evaluation_kind === "harness_fixture" ? body.evaluation_kind : undefined,
      provider: typeof body.provider === "string" ? body.provider : undefined,
      model: typeof body.model === "string" ? body.model : undefined,
      reasoning_effort: typeof body.reasoning_effort === "string" ? body.reasoning_effort : undefined,
      harness: typeof body.harness === "string" ? body.harness : undefined,
      prompt_hash: typeof body.prompt_hash === "string" ? body.prompt_hash : undefined,
      rollout_ids: Array.isArray(body.rollout_ids) ? body.rollout_ids.filter((item): item is string => typeof item === "string") : undefined,
      artifact_hashes: Array.isArray(body.artifact_hashes) ? body.artifact_hashes.filter((item): item is string => typeof item === "string") : undefined,
    });
    intelligenceRunByParticipant.set(intelligenceRunKey(decision.actor.id, run.task_id), run.run_id);
    res.status(201).json({ schema_version: "treatcode.intelligence.api.v1", data: { run }, run });
  } catch (error) {
    intelligenceErrorResponse(res, error);
  }
});

app.get("/api/intelligence/v1/runs/:runId", async (req: Request, res: Response) => {
  const decision = intelligenceAction(req, res, "read", false);
  if (!decision) return;
  try {
    const run = await intelligenceService.getRun(req.params.runId);
    serviceRunForActor(run, decision.actor.id);
    res.json({ schema_version: "treatcode.intelligence.api.v1", data: { run }, run });
  } catch (error) {
    intelligenceErrorResponse(res, error);
  }
});

app.post("/api/intelligence/v1/runs/:runId/trials/:trialId/files", async (req: Request, res: Response) => {
  const decision = intelligenceAction(req, res, "benchmark");
  if (!decision) return;
  try {
    const run = await intelligenceService.getRun(req.params.runId);
    serviceRunForActor(run, decision.actor.id);
    const body = (req.body || {}) as Record<string, unknown>;
    const result = await intelligenceService.writeFile(req.params.runId, req.params.trialId, String(body.path || ""), typeof body.content === "string" ? body.content : "");
    res.json({ schema_version: "treatcode.intelligence.api.v1", data: result, file: result });
  } catch (error) {
    intelligenceErrorResponse(res, error);
  }
});

app.get("/api/intelligence/v1/runs/:runId/trials/:trialId/files", async (req: Request, res: Response) => {
  const decision = intelligenceAction(req, res, "read", false);
  if (!decision) return;
  try {
    const run = await intelligenceService.getRun(req.params.runId);
    serviceRunForActor(run, decision.actor.id);
    const files = await intelligenceService.trialFiles(req.params.runId, req.params.trialId);
    res.json({ schema_version: "treatcode.intelligence.api.v1", data: { files }, files });
  } catch (error) {
    intelligenceErrorResponse(res, error);
  }
});

app.post("/api/intelligence/v1/runs/:runId/trials/:trialId/public-tests", async (req: Request, res: Response) => {
  const decision = intelligenceAction(req, res, "benchmark");
  if (!decision) return;
  try {
    const run = await intelligenceService.getRun(req.params.runId);
    serviceRunForActor(run, decision.actor.id);
    const report = await intelligenceService.runPublicTests(req.params.runId, req.params.trialId);
    res.json({ schema_version: "treatcode.intelligence.api.v1", data: { report }, report });
  } catch (error) {
    intelligenceErrorResponse(res, error);
  }
});

app.post("/api/intelligence/v1/runs/:runId/trials/:trialId/submit", async (req: Request, res: Response) => {
  const decision = intelligenceAction(req, res, "benchmark");
  if (!decision) return;
  try {
    const run = await intelligenceService.getRun(req.params.runId);
    serviceRunForActor(run, decision.actor.id);
    const result = await intelligenceService.submitTrial(req.params.runId, req.params.trialId);
    const refreshed = await intelligenceService.getRun(req.params.runId);
    res.json({ schema_version: "treatcode.intelligence.api.v1", data: { ...result, run: refreshed }, ...result, run: refreshed, trial: publicTrialView(refreshed.trials.find((trial) => trial.id === req.params.trialId) as unknown as Record<string, unknown>, result.hidden as unknown as Record<string, unknown>) });
  } catch (error) {
    intelligenceErrorResponse(res, error);
  }
});

// The UI's compact one-button adapter starts (or reuses) a participant run,
// writes an optional file map, and returns the public report plus sealed receipt.
app.post("/api/intelligence/v1/trials", async (req: Request, res: Response) => {
  const decision = intelligenceAction(req, res, "benchmark");
  if (!decision) return;
  try {
    const body = (req.body || {}) as Record<string, unknown>;
    const taskId = typeof body.task_id === "string" ? body.task_id : typeof body.taskId === "string" ? body.taskId : INTELLIGENCE_TASK_ID;
    const taskCatalog = intelligenceService.catalog(taskId);
    let runId = typeof body.run_id === "string" ? body.run_id : intelligenceRunByParticipant.get(intelligenceRunKey(decision.actor.id, taskId));
    let run = runId ? await intelligenceService.getRun(runId) : null;
    if (!run || run.task_id !== taskId || run.participant_id !== decision.actor.id || run.status !== "open") {
      run = await intelligenceService.startRun({ task_id: taskId, participant_id: decision.actor.id, source_commit: publicSnapshot.snapshot.commit || undefined });
      runId = run.run_id;
      intelligenceRunByParticipant.set(intelligenceRunKey(decision.actor.id, taskId), runId);
    }
    if (!runId) throw new IntelligenceServiceError("run_not_found", "Intelligence run could not be created", 500);
    const activeRunId = runId;
    const trialIndex = typeof body.trial_id === "string" ? Math.max(0, Number.parseInt(body.trial_id.match(/(?:trial[-_])?(\d+)$/i)?.[1] || "1", 10) - 1) : 0;
    const trial = run.trials[trialIndex] || run.trials[0];
    const files = body.files && typeof body.files === "object" ? body.files as Record<string, unknown> : {};
    for (const descriptor of taskCatalog.task.allowlisted_files) {
      const file = descriptor.path;
      if (typeof files[file] === "string") await intelligenceService.writeFile(activeRunId, trial.id, file, files[file] as string);
    }
    if (Object.keys(files).length === 0 && typeof body.code === "string" && body.code.trim()) {
      // A compact client may send one complete implementation.  Preserve the
      // historical TC-SWE-001 median path; single-file suite tasks use their
      // only allowlisted source file.
      const codePath = taskId === INTELLIGENCE_TASK_ID
        ? "src/median.trit"
        : taskCatalog.task.allowlisted_files.length === 1
          ? taskCatalog.task.allowlisted_files[0].path
          : undefined;
      if (codePath) await intelligenceService.writeFile(activeRunId, trial.id, codePath, body.code);
    }
    const result = await intelligenceService.submitTrial(activeRunId, trial.id);
    const refreshed = await intelligenceService.getRun(activeRunId);
    const refreshedTrial = refreshed.trials.find((item) => item.id === trial.id) || trial;
    res.status(201).json({ schema_version: "treatcode.intelligence.api.v1", data: { ...result, run: refreshed, trial: publicTrialView(refreshedTrial as unknown as Record<string, unknown>, result.hidden as unknown as Record<string, unknown>) }, ...result, run: refreshed, trial: publicTrialView(refreshedTrial as unknown as Record<string, unknown>, result.hidden as unknown as Record<string, unknown>) });
  } catch (error) {
    intelligenceErrorResponse(res, error);
  }
});

app.get("/api/intelligence/v1/runs/:runId/aggregate", async (req: Request, res: Response) => {
  const decision = intelligenceAction(req, res, "read", false);
  if (!decision) return;
  try {
    const run = await intelligenceService.getRun(req.params.runId);
    serviceRunForActor(run, decision.actor.id);
    const aggregate = await intelligenceService.getAggregate(req.params.runId);
    res.json({ schema_version: "treatcode.intelligence.api.v1", data: { aggregate }, aggregate });
  } catch (error) {
    intelligenceErrorResponse(res, error);
  }
});

app.post("/api/intelligence/v1/runs/:runId/attest", async (req: Request, res: Response) => {
  const decision = intelligenceAction(req, res, "benchmark");
  if (!decision) return;
  if (decision.actor.kind !== "service" && !decision.actor.roles.includes("automation")) {
    res.status(403).json({ schema_version: "treatcode.intelligence.api.v1", error: { code: "attestation_rejected", reason: "Only a privileged service identity may attest a model run." } });
    return;
  }
  try {
    const run = await intelligenceService.getRun(req.params.runId);
    const body = (req.body || {}) as Record<string, unknown>;
    const evaluationKind = body.evaluation_kind === "model_rollout" || body.evaluation_kind === "harness_fixture" ? body.evaluation_kind : undefined;
    if (!evaluationKind) {
      res.status(400).json({ schema_version: "treatcode.intelligence.api.v1", error: { code: "invalid_request", reason: "evaluation_kind is required; model scores must declare model_rollout or harness_fixture provenance." } });
      return;
    }
    const record = await intelligenceService.attest({
      run_id: run.run_id,
      principal: decision.actor.id,
      evaluation_kind: evaluationKind,
      model: typeof body.model === "string" ? body.model : evaluationKind === "harness_fixture" ? "bounded-suite-proof" : "",
      provider: typeof body.provider === "string" ? body.provider : undefined,
      reasoning_effort: typeof body.reasoning_effort === "string" ? body.reasoning_effort : undefined,
      harness: typeof body.harness === "string" ? body.harness : undefined,
      prompt_hash: typeof body.prompt_hash === "string" ? body.prompt_hash : undefined,
      rollout_ids: Array.isArray(body.rollout_ids) ? body.rollout_ids.filter((item): item is string => typeof item === "string") : undefined,
      artifact_hashes: Array.isArray(body.artifact_hashes) ? body.artifact_hashes.filter((item): item is string => typeof item === "string") : undefined,
      model_configuration: typeof body.model_configuration === "string" ? body.model_configuration : "max",
      tested_commit: typeof body.tested_commit === "string" ? body.tested_commit : run.source_commit,
      evidence_hashes: Array.isArray(body.evidence_hashes) ? body.evidence_hashes.filter((item): item is string => typeof item === "string") : [],
    });
    res.json({ schema_version: "treatcode.intelligence.api.v1", data: { attestation: record }, attestation: record });
  } catch (error) {
    intelligenceErrorResponse(res, error);
  }
});

// P08 workspace routes are versioned and isolated from the public snapshot
// routes above. The service provisions from `git archive`, so creating or
// editing a workspace never writes to the authoritative checkout.
function workspaceRequestId(req: Request): string {
  const supplied = String(req.header("x-request-id") || "").trim();
  return supplied && supplied.length <= 120 ? supplied : `tc:req:${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

function workspaceJson<T>(req: Request, data: T, meta: Record<string, unknown> = {}, links: Record<string, string> = {}) {
  return {
    schema_version: WORKSPACE_API_SCHEMA_VERSION,
    request_id: workspaceRequestId(req),
    data,
    meta,
    links,
  };
}

function workspaceErrorResponse(req: Request, res: Response, error: unknown, workspaceId?: string): void {
  const serviceFailure = error instanceof WorkspaceServiceError
    ? error
    : new WorkspaceServiceError({ status: 500, code: "workspace_internal_error", message: "The workspace operation could not be completed." });
  if (workspaceId && serviceFailure.status >= 400) {
    try {
      workspaceService.recordDenied(workspaceId, undefined, serviceFailure.code, { status: serviceFailure.status });
    } catch {
      // A failed audit write must not replace the structured API error.
    }
  }
  res.status(serviceFailure.status).json({
    schema_version: WORKSPACE_API_SCHEMA_VERSION,
    request_id: workspaceRequestId(req),
    error: {
      code: serviceFailure.code,
      message: serviceFailure.message,
      ...(Object.keys(serviceFailure.details).length ? { details: serviceFailure.details } : {}),
    },
  });
}

function workspacePermissionAction(scope: WorkspaceScope): PermissionAction {
  if (scope === "workspace:read" || scope === "workspace:context" || scope === "task:read") return "read";
  if (scope === "workspace:edit") return "edit";
  if (scope === "workspace:test") return "test";
  if (scope === "task:create") return "workspace";
  return "workspace";
}

function workspaceActorFromDecision(decision: ReturnType<AuthStore["authorize"]>): WorkspaceActor {
  if (!decision.allowed || !decision.actor || !decision.credential) {
    throw new Error("workspace authorization did not return an actor");
  }
  const actionScopes: Record<PermissionAction, WorkspaceScope[]> = {
    read: ["workspace:read", "workspace:context", "task:read"],
    workspace: ["workspace:create", "workspace:handoff", "workspace:destroy", "task:create"],
    edit: ["workspace:edit"],
    test: ["workspace:test"],
    benchmark: ["workspace:test"],
    artifact: ["workspace:context"],
    branch: ["workspace:edit"],
    commit: ["workspace:edit"],
    draft_pr: ["workspace:edit"],
    push: ["workspace:edit"],
    review: ["workspace:read"],
    merge: ["workspace:destroy"],
  };
  const scopes = [...new Set(decision.credential.actions.flatMap((action) => actionScopes[action] || []))];
  return {
    actor_id: decision.actor.id,
    kind: decision.actor.kind,
    scopes,
  };
}

function workspaceAuthorization(req: Request, res: Response, scope: WorkspaceScope, workspaceId?: string): { actor: WorkspaceActor; record?: ReturnType<typeof workspaceService.get> } | null {
  const decision = authStore.authorize({
    token: authToken(req),
    action: workspacePermissionAction(scope),
    project_id: requestProject(req, req.body),
    task_id: requestTask(req, req.body),
    request_nonce: requestNonce(req),
    resource: req.path,
    require_nonce: false,
  });
  if (!decision.allowed) {
    res.status(decision.denial.http_status).json({
      schema_version: WORKSPACE_API_SCHEMA_VERSION,
      request_id: workspaceRequestId(req),
      error: {
        code: decision.denial.code,
        message: decision.denial.reason,
        action: decision.denial.action,
        audit_event_id: decision.denial.audit_event_id,
        retryable: decision.denial.retryable,
      },
    });
    return null;
  }
  try {
    const actor = workspaceActorFromDecision(decision);
    if (workspaceId) {
      const record = workspaceService.get(workspaceId);
      workspaceService.requireAccess(record, actor, scope);
      return { actor, record };
    }
    return { actor };
  } catch (error) {
    if (workspaceId) {
      try { workspaceService.recordDenied(workspaceId, decision.actor?.id, error instanceof WorkspaceServiceError ? error.code : "authorization_failed", { required_scope: scope }); } catch { /* Best-effort audit. */ }
    }
    workspaceErrorResponse(req, res, error, workspaceId);
    return null;
  }
}

function workspaceAccess(req: Request, res: Response, workspaceId: string, scope: WorkspaceScope): { actor: WorkspaceActor; record: ReturnType<typeof workspaceService.get> } | null {
  return workspaceAuthorization(req, res, scope, workspaceId) as { actor: WorkspaceActor; record: ReturnType<typeof workspaceService.get> } | null;
}

function registerWorkspaceApi(prefix: string, collectionPrefix = `${prefix}/workspaces`): void {
  if (prefix !== collectionPrefix) {
    app.get(prefix, (req: Request, res: Response) => {
      res.json(workspaceJson(req, workspaceService.capabilities(), { public: false }));
    });
  }

  app.get(`${prefix}/capabilities`, (req: Request, res: Response) => {
    res.json(workspaceJson(req, workspaceService.capabilities(), { public: false }));
  });

  app.get(`${prefix}/openapi.json`, (_req: Request, res: Response) => {
    const candidates = [
      path.resolve(__dirname, "..", "docs", "11_TreatCode_Platform", "schemas", "workspace_api.v1.openapi.json"),
      path.resolve(process.cwd(), "docs", "11_TreatCode_Platform", "schemas", "workspace_api.v1.openapi.json"),
    ];
    for (const candidate of candidates) {
      try {
        res.json(JSON.parse(fs.readFileSync(candidate, "utf8")));
        return;
      } catch {
        // Continue to the next source location.
      }
    }
    res.status(404).json({ schema_version: WORKSPACE_API_SCHEMA_VERSION, error: { code: "contract_unavailable", message: "The workspace API contract is unavailable." } });
  });

  app.get(collectionPrefix, (req: Request, res: Response) => {
    const authorization = workspaceAuthorization(req, res, "workspace:read");
    if (!authorization) return;
    try {
      const workspaces = workspaceService.list(authorization.actor);
      res.json(workspaceJson(req, workspaces, { count: workspaces.length }, { self: collectionPrefix }));
    } catch (error) {
      workspaceErrorResponse(req, res, error);
    }
  });

  app.post(collectionPrefix, (req: Request, res: Response) => {
    const authorization = workspaceAuthorization(req, res, "workspace:create");
    if (!authorization) return;
    try {
      const workspace = workspaceService.createWorkspace((req.body || {}) as Record<string, unknown>, authorization.actor);
      res.status(201).json(workspaceJson(req, workspace, { operation: "create" }, { self: `${collectionPrefix}/${encodeURIComponent(workspace.id)}` }));
    } catch (error) {
      workspaceErrorResponse(req, res, error);
    }
  });

  app.get(`${collectionPrefix}/:workspaceId`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:read");
    if (!access) return;
    res.json(workspaceJson(req, workspaceService.publicRecord(access.record), { operation: "inspect" }, { self: `${collectionPrefix}/${encodeURIComponent(access.record.id)}` }));
  });

  app.get(`${collectionPrefix}/:workspaceId/files`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:read");
    if (!access) return;
    try {
      const requestedPath = String(req.query.path || "").trim();
      const data = requestedPath ? workspaceService.readFile(access.record, requestedPath) : workspaceService.listFiles(access.record);
      res.json(workspaceJson(req, data, { operation: requestedPath ? "read-file" : "list-files" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/files`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:edit");
    if (!access) return;
    try {
      const body = (req.body || {}) as Record<string, unknown>;
      const relativePath = String(body.path || "");
      const content = typeof body.content === "string" ? body.content : "";
      const result = workspaceService.updateFile(access.record, access.actor, relativePath, content, typeof body.expected_version === "number" ? body.expected_version : undefined);
      res.json(workspaceJson(req, result, { operation: "write-file" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.get(`${collectionPrefix}/:workspaceId/snapshots`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:read");
    if (!access) return;
    res.json(workspaceJson(req, access.record.snapshots, { count: access.record.snapshots.length }));
  });

  app.post(`${collectionPrefix}/:workspaceId/snapshots`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:edit");
    if (!access) return;
    try {
      const snapshot = workspaceService.createSnapshot(access.record, access.actor, (req.body || {}).label);
      res.status(201).json(workspaceJson(req, snapshot, { operation: "snapshot" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/snapshots/:snapshotId/restore`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:edit");
    if (!access) return;
    try {
      const snapshot = workspaceService.restoreSnapshot(access.record, access.actor, req.params.snapshotId);
      res.json(workspaceJson(req, snapshot, { operation: "restore" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/disconnect`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:read");
    if (!access) return;
    try {
      const body = (req.body || {}) as Record<string, unknown>;
      const checkpoint = body.checkpoint ? workspaceService.createSnapshot(access.record, access.actor, body.label || "Disconnect checkpoint") : null;
      const workspace = workspaceService.disconnect(access.record, access.actor);
      res.json(workspaceJson(req, { workspace, checkpoint }, { operation: "disconnect", persistent_state: true }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/resume`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:read");
    if (!access) return;
    try {
      const workspace = workspaceService.resume(access.record, access.actor);
      res.json(workspaceJson(req, workspace, { operation: "resume", recovered: true }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.get(`${collectionPrefix}/:workspaceId/export`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:read");
    if (!access) return;
    try {
      res.json(workspaceJson(req, workspaceService.exportWorkspace(access.record, access.actor), { operation: "export" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/export`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:read");
    if (!access) return;
    try {
      res.json(workspaceJson(req, workspaceService.exportWorkspace(access.record, access.actor), { operation: "export" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/checks`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:test");
    if (!access) return;
    try {
      const body = (req.body || {}) as Record<string, unknown>;
      res.json(workspaceJson(req, workspaceService.runCheck(access.record, access.actor, String(body.kind || "doctor")), { operation: "check" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/handoff`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:handoff");
    if (!access) return;
    try {
      const body = (req.body || {}) as Record<string, unknown>;
      res.json(workspaceJson(req, workspaceService.handoff(access.record, access.actor, String(body.to_actor_id || body.recipient_id || body.target_actor_id || ""), body.scopes), { operation: "handoff" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.delete(`${collectionPrefix}/:workspaceId/collaborators/:actorId`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:handoff");
    if (!access) return;
    try {
      res.json(workspaceJson(req, workspaceService.revokeCollaborator(access.record, access.actor, req.params.actorId), { operation: "handoff-revoke" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.get(`${collectionPrefix}/:workspaceId/audit`, (req: Request, res: Response) => {
    const authorization = workspaceAuthorization(req, res, "workspace:read");
    if (!authorization) return;
    try {
      const record = workspaceService.get(req.params.workspaceId);
      workspaceService.requireAuditAccess(record, authorization.actor);
      const audit = workspaceService.auditForWorkspace(record.id);
      res.json(workspaceJson(req, audit, { count: audit.length }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, req.params.workspaceId);
    }
  });

  app.get(`${collectionPrefix}/:workspaceId/tasks`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "task:read");
    if (!access) return;
    res.json(workspaceJson(req, access.record.tasks, { count: access.record.tasks.length }));
  });

  app.post(`${collectionPrefix}/:workspaceId/tasks`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "task:create");
    if (!access) return;
    try {
      res.status(201).json(workspaceJson(req, workspaceService.createTask(access.record, access.actor, (req.body || {}) as Record<string, unknown>), { operation: "task-create" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.get(`${collectionPrefix}/:workspaceId/tasks/:taskId`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "task:read");
    if (!access) return;
    try {
      res.json(workspaceJson(req, workspaceService.task(access.record, req.params.taskId), { operation: "task-inspect" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/tasks/:taskId/approve`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "task:create");
    if (!access) return;
    try {
      res.json(workspaceJson(req, workspaceService.approveTask(access.record, access.actor, req.params.taskId), { operation: "task-approve" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/tasks/:taskId/context-package`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:context");
    if (!access) return;
    try {
      workspaceService.task(access.record, req.params.taskId);
      const body = (req.body || {}) as Record<string, unknown>;
      res.json(workspaceJson(req, workspaceService.contextPackage(access.record, access.actor, body.scopes || body.scope, req.params.taskId, body.limits as Record<string, unknown> | undefined), { operation: "context-package" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/context-package`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:context");
    if (!access) return;
    try {
      const body = (req.body || {}) as Record<string, unknown>;
      res.json(workspaceJson(req, workspaceService.contextPackage(access.record, access.actor, body.scopes || body.scope, typeof body.task_id === "string" ? body.task_id : undefined, body.limits as Record<string, unknown> | undefined), { operation: "context-package" }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.post(`${collectionPrefix}/:workspaceId/destroy`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:destroy");
    if (!access) return;
    try {
      res.json(workspaceJson(req, workspaceService.destroy(access.record, access.actor), { operation: "destroy", access_revoked: true }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });

  app.delete(`${collectionPrefix}/:workspaceId`, (req: Request, res: Response) => {
    const access = workspaceAccess(req, res, req.params.workspaceId, "workspace:destroy");
    if (!access) return;
    try {
      res.json(workspaceJson(req, workspaceService.destroy(access.record, access.actor), { operation: "destroy", access_revoked: true }));
    } catch (error) {
      workspaceErrorResponse(req, res, error, access.record.id);
    }
  });
}


// Operations is a private, stateful control plane separate from the public snapshot API.
const operationsStatePath = process.env.TREATCODE_OPERATIONS_STATE || null;
const operationsStore = new OperationsStore({ filePath: operationsStatePath });

function operationsActor(req: Request): OperationActor {
  const bodyActor = req.body && typeof req.body.actor === "object" && req.body.actor ? req.body.actor : {};
  return {
    id: String(bodyActor.id || req.header("x-treatcode-actor") || "operations-console"),
    permission: String(bodyActor.permission || req.header("x-treatcode-permission") || "operations:write"),
    commit: String(bodyActor.commit || req.header("x-treatcode-commit") || "working-tree"),
  };
}

function operationsError(res: Response, error: unknown): void {
  const code = String((error as Error)?.message || "OPERATIONS_ERROR");
  const status = code === "TASK_NOT_FOUND" || code === "NOTIFICATION_NOT_FOUND" ? 404 : code === "APPROVAL_REQUIRED" ? 409 : 400;
  res.status(status).json({ schemaVersion: "treatcode.operations.v1", error: { code, message: code.replace(/_/g, " ").toLowerCase() } });
}

function registerOperationsApi(): void {
  app.get("/api/operations", (_req: Request, res: Response) => {
    res.json({ schemaVersion: "treatcode.operations.v1", service: "operations", links: { overview: "/api/operations/overview", events: "/api/operations/events", health: "/api/operations/health" } });
  });

  app.get("/api/operations/overview", (_req: Request, res: Response) => {
    res.json(operationsStore.overview());
  });

  app.get("/api/operations/health", (_req: Request, res: Response) => {
    const health = operationsStore.health();
    res.status(health.ok ? 200 : 503).json(health);
  });

  app.get("/api/operations/policies", (_req: Request, res: Response) => {
    res.json(operationsStore.overview().policies);
  });

  app.get("/api/operations/tasks", (req: Request, res: Response) => {
    const status = String(req.query.status || "").trim();
    const tasks = operationsStore.listTasks().filter((task) => !status || task.status === status);
    res.json({ schemaVersion: "treatcode.operations.v1", data: tasks, meta: { count: tasks.length } });
  });

  app.post("/api/operations/tasks", (req: Request, res: Response) => {
    try {
      const task = operationsStore.createTask((req.body || {}) as TaskInput, operationsActor(req));
      res.status(201).json(task);
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.get("/api/operations/tasks/:taskId", (req: Request, res: Response) => {
    try {
      res.json(operationsStore.getTask(req.params.taskId));
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.post("/api/operations/tasks/:taskId/actions", (req: Request, res: Response) => {
    try {
      const action = String(req.body?.action || "").trim();
      if (!action) {
        res.status(400).json({ schemaVersion: "treatcode.operations.v1", error: { code: "ACTION_REQUIRED", message: "an action is required" } });
        return;
      }
      res.json(operationsStore.action(req.params.taskId, action, operationsActor(req)));
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.post("/api/operations/sessions", (req: Request, res: Response) => {
    try {
      const task = operationsStore.connectClient(req.body?.clientId, req.body?.taskId, operationsActor(req));
      res.json({ connected: true, task });
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.delete("/api/operations/sessions/:clientId", (req: Request, res: Response) => {
    try {
      res.json({ connected: false, task: operationsStore.disconnectClient(req.params.clientId, operationsActor(req)) });
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.post("/api/operations/sessions/:clientId/resume", (req: Request, res: Response) => {
    try {
      res.json({ connected: true, resumed: true, task: operationsStore.resumeClient(req.params.clientId, req.body?.taskId, operationsActor(req)) });
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.get("/api/operations/events", (req: Request, res: Response) => {
    res.json({ schemaVersion: "treatcode.operations.v1", data: operationsStore.eventsSince(req.query.since), meta: { since: Number(req.query.since || 0) || 0 } });
  });

  app.get("/api/operations/stream", (req: Request, res: Response) => {
    res.status(200).set({ "Content-Type": "text/event-stream", "Cache-Control": "no-cache", Connection: "keep-alive" });
    let sequence = Number(req.query.since || 0) || 0;
    const writeEvents = () => {
      const events = operationsStore.eventsSince(sequence);
      for (const event of events) {
        sequence = event.sequence;
        res.write(`event: operations\\ndata: ${JSON.stringify(event)}\\n\\n`);
      }
      res.write(`event: heartbeat\\ndata: ${JSON.stringify({ at: new Date().toISOString(), sequence })}\\n\\n`);
    };
    writeEvents();
    const timer = setInterval(writeEvents, 5000);
    req.on("close", () => clearInterval(timer));
  });

  app.get("/api/operations/notifications", (req: Request, res: Response) => {
    const notifications = operationsStore.overview().notifications.filter((notification) => req.query.unread === "true" ? !notification.read : true);
    res.json({ schemaVersion: "treatcode.operations.v1", data: notifications, meta: { count: notifications.length } });
  });

  app.post("/api/operations/notifications/:notificationId/read", (req: Request, res: Response) => {
    try {
      res.json(operationsStore.markNotificationRead(req.params.notificationId, operationsActor(req)));
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.patch("/api/operations/notifications/preferences", (req: Request, res: Response) => {
    try {
      res.json(operationsStore.setNotificationPreferences(req.body || {}, operationsActor(req)));
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.get("/api/operations/audit", (req: Request, res: Response) => {
    const limit = Math.max(1, Math.min(100, Number(req.query.limit || 50) || 50));
    res.json({ schemaVersion: "treatcode.operations.v1", data: operationsStore.overview().audit.slice(0, limit), meta: { count: limit } });
  });

  app.post("/api/operations/backup", (req: Request, res: Response) => {
    try {
      res.status(201).json(operationsStore.backup(operationsActor(req)));
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.post("/api/operations/restore", (req: Request, res: Response) => {
    try {
      res.json(operationsStore.restore((req.body?.backup || req.body) as OperationsBackup, operationsActor(req)));
    } catch (error) {
      operationsError(res, error);
    }
  });

  app.post("/api/operations/recovery-exercise", (_req: Request, res: Response) => {
    const report = operationsStore.disasterRecoveryExercise();
    res.status(report.ok ? 200 : 503).json(report);
  });
}

registerOperationsApi();
const operationsTicker = setInterval(() => operationsStore.tick(), 1000);
operationsTicker.unref?.();

const executionQueue = new SecureExecutionQueue({
  artifactRoot: process.env.TREATCODE_RUNNER_ARTIFACT_ROOT || path.resolve(process.cwd(), "..", "build", "treatcode-runner-evidence"),
  repositoryRoot: path.resolve(__dirname, ".."),
  workerPath: path.join(__dirname, "src", "runner", "worker.ts"),
  concurrency: 2,
  maxRetries: 1,
});

// Contribution intake is deliberately separate from the public snapshot API.
// Upload bytes stay in the service's quarantine/workspace boundary and the
// GitHub adapter receives only validated file metadata and immutable evidence.
const contributionGitHub = new InMemoryGitHubApp();
const contributionService = new ContributionService({ github: contributionGitHub });

function contributionActor(req: Request): ContributionActor {
  const rawScopes = String(req.header("x-treatcode-scopes") || "").split(",").map((scope) => scope.trim()).filter(Boolean);
  const scopes = rawScopes.filter((scope): scope is ContributionAction => (CONTRIBUTION_ACTIONS as readonly string[]).includes(scope));
  const actorType = String(req.header("x-treatcode-actor-type") || "agent");
  return {
    actorId: String(req.header("x-treatcode-actor") || "anonymous"),
    actorType: actorType === "human" || actorType === "service" ? actorType : "agent",
    scopes,
    taskId: req.header("x-treatcode-task") || undefined,
    projectId: req.header("x-treatcode-project") || undefined,
    tokenId: req.header("x-treatcode-token") || undefined,
    expiresAt: req.header("x-treatcode-expires-at") || undefined,
  };
}

function contributionErrorResponse(res: Response, error: unknown): void {
  if (error instanceof ContributionError) {
    res.status(error.status).json({ schema_version: "treatcode.contribution.v1", error: { code: error.code, message: error.message, details: error.details } });
    return;
  }
  res.status(500).json({ schema_version: "treatcode.contribution.v1", error: { code: "CONTRIBUTION_INTERNAL_ERROR", message: "Contribution request failed." } });
}

function contributionHandler(handler: (req: Request, res: Response) => void | Promise<void>) {
  return (req: Request, res: Response) => {
    Promise.resolve(handler(req, res)).catch((error) => contributionErrorResponse(res, error));
  };
}

app.get("/api/contributions/capabilities", (_req: Request, res: Response) => {
  res.json(contributionService.capabilities());
});

app.post("/api/contributions/uploads", contributionHandler((req, res) => {
  const session = contributionService.createUploadSession(contributionActor(req), req.body || {});
  res.status(201).json(session);
}));

app.get("/api/contributions/uploads/:uploadId", contributionHandler((req, res) => {
  res.json(contributionService.getUploadSession(contributionActor(req), req.params.uploadId));
}));

app.put("/api/contributions/uploads/:uploadId/chunks", contributionHandler((req, res) => {
  const encoded = String(req.body?.dataBase64 || "");
  if (!/^[A-Za-z0-9+/]*={0,2}$/.test(encoded) || encoded.length === 0) throw new ContributionError("INVALID_CHUNK", "Chunk dataBase64 is required.", 422);
  const bytes = Buffer.from(encoded, "base64");
  const session = contributionService.putUploadChunk(contributionActor(req), req.params.uploadId, Number(req.body?.offset), bytes);
  res.json(session);
}));

app.post("/api/contributions/uploads/:uploadId/finalize", contributionHandler((req, res) => {
  res.json(contributionService.finalizeUpload(contributionActor(req), req.params.uploadId));
}));

app.post("/api/contributions/workspaces", contributionHandler((req, res) => {
  const workspace = contributionService.createWorkspace(contributionActor(req), req.body || {});
  res.status(201).json(workspace);
}));

app.get("/api/contributions/workspaces/:workspaceId", contributionHandler((req, res) => {
  res.json(contributionService.getWorkspace(contributionActor(req), req.params.workspaceId));
}));

app.post("/api/contributions/workspaces/:workspaceId/uploads/:uploadId", contributionHandler((req, res) => {
  res.json(contributionService.materializeUpload(contributionActor(req), req.params.workspaceId, req.params.uploadId, req.body?.targetPath));
}));

app.post("/api/contributions/workspaces/:workspaceId/validate", contributionHandler((req, res) => {
  res.json(contributionService.validateWorkspace(contributionActor(req), req.params.workspaceId));
}));

app.post("/api/contributions/workspaces/:workspaceId/evidence", contributionHandler((req, res) => {
  res.status(201).json(contributionService.attachEvidence(contributionActor(req), req.params.workspaceId, req.body || {}));
}));

app.post("/api/contributions/workspaces/:workspaceId/approval", contributionHandler((req, res) => {
  res.status(201).json(contributionService.recordHumanApproval(contributionActor(req), req.params.workspaceId, req.body || {}));
}));

app.post("/api/contributions/workspaces/:workspaceId/draft-pr", contributionHandler(async (req, res) => {
  res.status(201).json(await contributionService.submitDraftPullRequest(contributionActor(req), req.params.workspaceId, req.body || {}));
}));

// In-memory databases
interface LeaderboardEntry {
  id: number;
  problemId: string;
  name: string;
  engine: "native" | "bootstrap";
  cycles: number;
  date: string;
}

// Leaderboard state is populated only by accepted submissions in this server
// process.  Keeping the initial collection empty prevents demo records from
// being presented as authoritative public performance statistics.
const leaderboard: LeaderboardEntry[] = [];

interface TestCase {
  arg: any;
  expectedOutput?: string;
  expectedR13?: number;
}

interface ChallengeTestCase {
  arg: any;
  expected_output?: string;
  expected_r13?: number;
}

interface Challenge {
  id: string;
  title: string;
  lifecycle: "published" | "draft" | "retired";
  difficulty: "easy" | "medium" | "hard";
  category: string;
  tags: string[];
  facets: Record<string, string[]>;
  description: string;
  signature: string;
  template: string;
  stats: { solved: boolean; submissions: number; acceptance: number; points: number };
  execution: {
    mode: "verified" | "compile-only";
    runner: string;
    limits: { time_ms: number; memory_kib: number; max_cycles: number; max_output_bytes: number };
    correctness?: {
      kind: string;
      test_cases: ChallengeTestCase[];
      wrapper: { kind: "call" | "string-buffer"; template: string };
    };
    cost_probe?: Record<string, unknown>;
  };
  source_refs: Array<{ path: string; symbol: string }>;
}

interface GeneratedChallengeData {
  schema: string;
  manifest_schema: string;
  manifest_version: number;
  source_of_truth: string;
  facet_dimensions: Record<string, string[]>;
  challenges: Challenge[];
}

function loadChallenges(): Challenge[] {
  const candidates = [
    path.join(__dirname, "src", "generated", "challenges.server.json"),
    path.join(process.cwd(), "src", "generated", "challenges.server.json"),
    path.join(process.cwd(), "treatcode", "src", "generated", "challenges.server.json"),
  ];
  for (const candidate of candidates) {
    try {
      const data = JSON.parse(fs.readFileSync(candidate, "utf8")) as GeneratedChallengeData;
      if (data.schema === "treatcode.challenge_data.v1" && Array.isArray(data.challenges)) return data.challenges;
    } catch {
      // Try the next generated-data location.
    }
  }
  throw new Error("Generated challenge data is unavailable; run npm run generate:challenges.");
}

const challenges = loadChallenges();
const challengeById = new Map(challenges.map((challenge) => [challenge.id, challenge]));

function challengeTestCases(challenge: Challenge): TestCase[] {
  return (challenge.execution.correctness?.test_cases || []).map((testCase) => ({
    arg: testCase.arg,
    expectedOutput: testCase.expected_output,
    expectedR13: testCase.expected_r13,
  }));
}

function renderChallengeWrapper(challenge: Challenge, code: string, arg: any): string {
  const wrapper = challenge.execution.correctness?.wrapper;
  if (!wrapper) throw new Error(`Challenge ${challenge.id} has no correctness wrapper.`);
  const values: Record<string, string> = {
    "${code}": code,
    "${arg}": String(arg),
  };
  if (wrapper.kind === "string-buffer") {
    const input = String(arg);
    values["${arg_length}"] = String(input.length);
    values["${arg_stores}"] = Array.from(input)
      .map((character, index) => `store(p + ${index}, ${character.charCodeAt(0)});`)
      .join("\n                ");
  }
  let rendered = wrapper.template;
  for (const [token, value] of Object.entries(values)) rendered = rendered.split(token).join(value);
  return rendered;
}

function clientChallenge(challenge: Challenge) {
  return {
    id: challenge.id,
    title: challenge.title,
    lifecycle: challenge.lifecycle,
    difficulty: challenge.difficulty,
    category: challenge.category,
    tags: challenge.tags,
    facets: challenge.facets,
    description: challenge.description,
    signature: challenge.signature,
    template: challenge.template,
    stats: challenge.stats,
    ...challenge.stats,
    execution: {
      mode: challenge.execution.mode,
      runner: challenge.execution.runner,
      limits: challenge.execution.limits,
    },
  };
}

async function compileAndRunTrit(
  code: string,
  engine: RunnerEngine,
  optLevel = "-O2",
): Promise<ExecutionResult> {
  const normalizedOptLevel = /^-O[0-3]$/.test(optLevel) ? optLevel as "-O0" | "-O1" | "-O2" | "-O3" : "-O2";
  return executionQueue.submit({
    schema: EXECUTION_REQUEST_SCHEMA,
    kind: "trit.compile-and-run",
    code,
    engine,
    optLevel: normalizedOptLevel,
    sourceCommit: publicSnapshot.snapshot.commit || "working-tree",
  }).result;
}

// REST Routes
function normalizeRunnerEngine(value: unknown): RunnerEngine | null {
  return value === "native" || value === "bootstrap" ? value : null;
}

interface LearningExerciseContract {
  id: string;
  runner: string;
}

function loadLearningExerciseContracts(): Map<string, LearningExerciseContract> {
  const roots = [
    path.join(__dirname, "src", "content", "learn"),
    path.join(process.cwd(), "src", "content", "learn"),
    path.join(process.cwd(), "treatcode", "src", "content", "learn"),
  ];
  for (const root of roots) {
    try {
      const contracts = new Map<string, LearningExerciseContract>();
      for (const name of fs.readdirSync(root)) {
        if (!name.endsWith(".md")) continue;
        const source = fs.readFileSync(path.join(root, name), "utf8");
        const match = source.match(/^---\r?\n([\s\S]*?)\r?\n---/);
        if (!match) continue;
        const metadata = JSON.parse(match[1]) as { interactive?: { kind?: string; exercise_id?: string; runner?: string } };
        if (metadata.interactive?.kind === "code" && metadata.interactive.exercise_id && metadata.interactive.runner) {
          contracts.set(metadata.interactive.exercise_id, { id: metadata.interactive.exercise_id, runner: metadata.interactive.runner });
        }
      }
      if (contracts.size > 0) return contracts;
    } catch {
      // Try the next workspace layout used by Bun, node, and the built server.
    }
  }
  return new Map();
}

const learningExerciseContracts = loadLearningExerciseContracts();

app.post("/api/learn/exercises/run", async (req: Request, res: Response) => {
  const decision = requireAction(req, res, "test", { require_nonce: false });
  if (!decision) return;
  const exerciseId = String(req.body?.exercise_id || "");
  const code = req.body?.code;
  const contract = learningExerciseContracts.get(exerciseId);
  if (!contract) {
    res.status(404).json({ schema: "treatcode.learning.exercise-run.v1", success: false, error: "Learning exercise not found." });
    return;
  }
  if (typeof code !== "string" || code.length === 0 || code.length > 64 * 1024 || code.includes("\u0000")) {
    res.status(400).json({ schema: "treatcode.learning.exercise-run.v1", success: false, error: "Exercise source must be non-empty, text-only, and at most 64 KiB." });
    return;
  }
  const runnerEngine = req.body?.engine === undefined ? "bootstrap" as const : normalizeRunnerEngine(req.body.engine);
  if (!runnerEngine) {
    res.status(400).json({ schema: "treatcode.learning.exercise-run.v1", success: false, error: "Exercise runner must be native or bootstrap." });
    return;
  }

  // The lesson starter deliberately contains a named function instead of a
  // hidden entry point. Add only the bounded harness needed to execute that
  // function; submissions that omit or change it receive a real compiler
  // diagnostic from the isolated runner.
  const executableSource = /\bfn\s+main\s*\(/.test(code)
    ? code
    : `${code}\n\nfn main() -> t40 {\n    return first_value();\n}\n`;
  try {
    const result = await compileAndRunTrit(executableSource, runnerEngine, "-O0");
    const compilerOutput = result.compilerOutput ? result.compilerOutput.slice(-16_384) : "";
    res.json({
      schema: "treatcode.learning.exercise-run.v1",
      exercise_id: exerciseId,
      runner: contract.runner,
      engine: runnerEngine,
      success: result.success,
      state: result.state,
      summary: result.success
        ? `Bounded compiler/VM execution completed for ${exerciseId}.`
        : `Bounded compiler/VM execution reported ${result.error || "a real failure"}.`,
      error: result.error || null,
      compilerOutput,
      run_id: result.runId,
      evidence: {
        record_hash: result.evidence.recordHash,
        record_path: result.evidence.recordPath,
        run: `/api/runs/${encodeURIComponent(result.runId)}`,
      },
    });
  } catch (error) {
    res.status(503).json({ schema: "treatcode.learning.exercise-run.v1", success: false, exercise_id: exerciseId, error: `Learning runner unavailable: ${String(error)}` });
  }
});

app.get("/api/problems", (req: Request, res: Response) => {
  // Retired entries remain in the manifest for auditability but never enter
  // the active challenge experience. Correctness fixtures stay server-only.
  res.json(challenges.filter((challenge) => challenge.lifecycle !== "retired").map(clientChallenge));
});

app.get("/api/leaderboard", (req: Request, res: Response) => {
  const persisted = communityStore.listChallengeSubmissions({ limit: 100 })
    .filter((record) => record.outcome === "accepted")
    .map((record, index) => ({
      id: index + 1,
      problemId: record.problem_id,
      name: record.owner_handle.substring(0, 20),
      engine: record.engine === "native" ? "native" as const : "bootstrap" as const,
      cycles: record.cycles || 0,
      date: record.created_at.split("T")[0],
    }));
  const combined = [...leaderboard, ...persisted].filter((entry, index, all) => all.findIndex((candidate) => candidate.problemId === entry.problemId && candidate.name === entry.name && candidate.date === entry.date && candidate.cycles === entry.cycles) === index);
  combined.sort((a, b) => a.cycles - b.cycles);
  res.json(combined);
});

app.get("/api/runs/:runId", async (req: Request, res: Response) => {
  const decision = requireAction(req, res, "read", { require_nonce: false });
  if (!decision) return;
  const runId = String(req.params.runId || "");
  const record = await executionQueue.getRecord(runId);
  if (record) {
    res.json(record);
    return;
  }
  const status = executionQueue.status(runId);
  if (!status) {
    res.status(404).json({ error: "Run not found" });
    return;
  }
  res.status(202).json(status);
});

app.post("/api/run", async (req: Request, res: Response) => {
  const decision = requireAction(req, res, "test");
  if (!decision) return;
  const { problemId, code, engine, optLevel } = req.body;
  const runnerEngine = normalizeRunnerEngine(engine);
  if (!runnerEngine) {
    return res.status(400).json({ error: "Runner engine must be native or bootstrap" });
  }
  const challenge = challengeById.get(String(problemId));
  if (!challenge) {
    return res.status(404).json({ error: "Problem not found" });
  }
  if (challenge.lifecycle !== "published") {
    return res.status(409).json({
      success: false,
      code: "challenge_not_published",
      lifecycle: challenge.lifecycle,
      error: "This challenge is not available for verified execution.",
    });
  }

  // Get first test case for manual run
  const testCases = challengeTestCases(challenge);
  if (testCases.length === 0) {
    return res.status(500).json({ success: false, error: "Published challenge has no correctness contract" });
  }
  const testCase = testCases[0];
  const wrappedCode = renderChallengeWrapper(challenge, code, testCase.arg);

  const result = await compileAndRunTrit(wrappedCode, runnerEngine, optLevel);
  res.json(result);
});

app.post("/api/submit", async (req: Request, res: Response) => {
  const testDecision = requireAction(req, res, "test", { require_nonce: false });
  if (!testDecision) return;
  const { problemId, code, engine, optLevel, solutionId, solution_id } = req.body;
  const runnerEngine = normalizeRunnerEngine(engine);
  if (!runnerEngine) {
    return res.status(400).json({ error: "Runner engine must be native or bootstrap" });
  }
  const challenge = challengeById.get(String(problemId));
  if (!challenge) {
    return res.status(404).json({ error: "Problem not found" });
  }

  if (challenge.lifecycle !== "published") {
    return res.status(409).json({
      success: false,
      code: "challenge_not_published",
      lifecycle: challenge.lifecycle,
      error: "Draft and retired challenges cannot be submitted for verified completion.",
    });
  }

  const testCases = challengeTestCases(challenge);
  if (testCases.length === 0) {
    return res.status(500).json({ success: false, error: "Published challenge has no correctness contract" });
  }

  const results = [];
  let allPassed = true;
  let totalCycles = 0;
  let totalRuntimeMs = 0;
  let totalCompileCycles = 0;
  let peakMemoryKib: number | null = null;
  let testsPassed = 0;

  for (let i = 0; i < testCases.length; i++) {
    const tc = testCases[i];
    const wrappedCode = renderChallengeWrapper(challenge, code, tc.arg);
    const runResult = await compileAndRunTrit(wrappedCode, runnerEngine, optLevel);
    if (typeof runResult.runtimeMs === "number" && Number.isFinite(runResult.runtimeMs)) totalRuntimeMs += Math.max(0, Math.round(runResult.runtimeMs));
    if (typeof runResult.compileTimeCycles === "number" && Number.isFinite(runResult.compileTimeCycles)) totalCompileCycles += Math.max(0, Math.round(runResult.compileTimeCycles));
    if (typeof runResult.peakMemoryKib === "number" && Number.isFinite(runResult.peakMemoryKib)) peakMemoryKib = Math.max(peakMemoryKib || 0, Math.round(runResult.peakMemoryKib));

    if (!runResult.success) {
      results.push({
        testCase: i + 1,
        passed: false,
        runtimeMs: runResult.runtimeMs,
        memoryKib: runResult.peakMemoryKib,
        compileCycles: runResult.compileTimeCycles || 0,
        error: runResult.error,
        compilerOutput: runResult.compilerOutput
      });
      allPassed = false;
      continue;
    }

    let passed = false;
    if (tc.expectedOutput !== undefined) {
      const actual = (runResult.consoleOutput || "").replace(/\r\n/g, "\n").trim();
      const expected = tc.expectedOutput.replace(/\r\n/g, "\n").trim();
      passed = actual === expected;
    } else if (tc.expectedR13 !== undefined) {
      passed = runResult.r13 === tc.expectedR13;
    }

    results.push({
      testCase: i + 1,
      passed,
      runtimeMs: runResult.runtimeMs,
      memoryKib: runResult.peakMemoryKib,
      compileCycles: runResult.compileTimeCycles || 0,
      cycles: runResult.cycles,
      consoleOutput: runResult.consoleOutput,
      r13: runResult.r13,
      registers: runResult.registers
    });

    if (!passed) allPassed = false;
    if (passed) testsPassed += 1;
    totalCycles += runResult.cycles || 0;
  }

  // If all tests passed, insert into leaderboard
  if (allPassed) {
    const avgCycles = Math.round(totalCycles / testCases.length);
    const newEntry: LeaderboardEntry = {
      id: leaderboard.length + 1,
      problemId,
      name: testDecision.actor.handle || testDecision.actor.display_name.substring(0, 20),
      engine: runnerEngine,
      cycles: avgCycles,
      date: new Date().toISOString().split("T")[0]
    };
    leaderboard.push(newEntry);
    leaderboard.sort((a, b) => a.cycles - b.cycles);
  }

  let submission;
  try {
    submission = communityStore.recordAuthenticatedSubmission(testDecision, {
      problem_id: String(problemId),
      solution_id: typeof solutionId === "string" ? solutionId : typeof solution_id === "string" ? solution_id : undefined,
      code: typeof code === "string" ? code : "",
      language: "trit",
      engine: runnerEngine,
      outcome: allPassed ? "accepted" : "rejected",
      accepted: allPassed,
      cycles: totalCycles > 0 ? Math.round(totalCycles / Math.max(testsPassed, 1)) : undefined,
      metrics: {
        runtime_ms: totalRuntimeMs > 0 ? totalRuntimeMs : null,
        memory_kib: peakMemoryKib,
        cycles: totalCycles > 0 ? Math.round(totalCycles / Math.max(testsPassed, 1)) : null,
        compile_cycles: totalCompileCycles > 0 ? totalCompileCycles : null,
        tests_passed: testsPassed,
        tests_total: testCases.length,
        engine: runnerEngine,
        opt_level: typeof optLevel === "string" && /^-O[0-3]$/.test(optLevel) ? optLevel : "-O2",
      },
      metadata: { opt_level: typeof optLevel === "string" ? optLevel : "-O2" },
    });
  } catch (error) {
    intelligenceErrorResponse(res, error);
    return;
  }

  res.json({
    success: allPassed,
    results,
    submission,
  });
});

// Fallback to serving the built frontend app for all other routes
app.get("*", (req: Request, res: Response) => {
  const indexHtmlPath = path.join(distPath, "index.html");
  if (fs.existsSync(indexHtmlPath)) {
    res.sendFile(indexHtmlPath);
  } else {
    res.status(404).send("API endpoint or frontend assets not found");
  }
});

if (process.env.TREATCODE_NO_LISTEN !== "1") {
  const server = app.listen(PORT, () => {
    const address = server.address();
    const actualPort = typeof address === "object" && address ? address.port : PORT;
    console.log(`TreatCode Server running at http://localhost:${actualPort}`);
  });
}
