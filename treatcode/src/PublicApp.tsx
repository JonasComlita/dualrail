import { FormEvent, MouseEvent as ReactMouseEvent, ReactNode, useEffect, useMemo, useState } from "react";
import "./treatcode-theme.css";
import {
  PublicRecord,
  PublicSnapshot,
  SearchMode,
  SourceRef,
  collectionFor,
  searchSnapshot,
} from "./publicApi";
import {
  LEARNING_CATALOG,
  LEARNING_MATRIX,
  LEARNING_PAGES,
  learningPathPages,
  parseMarkdown,
  type Block,
  type LearningInteractive,
  type LearningPage,
} from "./learningContent";
import researchCatalog from "./content/research/catalog.json";
import { SiteHeader, type SiteHeaderSection } from "./SiteHeader";
type Route = { page: "home" | "stack" | "learn" | "research" | "search" | "resource" | "evidence"; stackSlug?: string; query: string; mode: SearchMode; focus?: string; resource?: string; recordId?: string; pageNumber: number; learningPathId?: string; learningTopicId?: string };

const PUBLIC_RESOURCES = ["projects", "stack_nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps"] as const;
type PublicResourceName = typeof PUBLIC_RESOURCES[number];

function decodeRouteSegment(value: string | undefined): string | undefined {
  if (!value) return undefined;
  try {
    return decodeURIComponent(value);
  } catch {
    return value;
  }
}

function parseRoute(): Route {
  const path = window.location.pathname.replace(/\/index\.html$/, "").replace(/\/$/, "") || "/";
  const parts = path.split("/").filter(Boolean);
  const params = new URLSearchParams(window.location.search);
  const rawResource = parts[0] === "resources" ? parts[1] : undefined;
  const page = parts[0] === "stack" ? "stack" : parts[0] === "learn" ? "learn" : parts[0] === "research" ? "research" : parts[0] === "search" ? "search" : parts[0] === "resources" ? "resource" : parts[0] === "evidence" ? "evidence" : "home";
  const urlLearningPath = page === "learn" && parts.length >= 2 ? parts[1] : undefined;
  const urlLearningTopic = page === "learn" && parts.length >= 3 ? parts[2] : undefined;
  const pageNumber = Math.max(1, Number.parseInt(params.get("page") || "1", 10) || 1);
  return {
    page,
    stackSlug: page === "stack" ? decodeRouteSegment(parts[1]) : undefined,
    query: params.get("q") || "",
    mode: (params.get("mode") as SearchMode) || "semantic",
    focus: params.get("focus") || undefined,
    resource: decodeRouteSegment(rawResource),
    recordId: decodeRouteSegment(parts[2]),
    pageNumber,
    learningPathId: urlLearningPath || params.get("path") || undefined,
    learningTopicId: urlLearningTopic || params.get("topic") || undefined,
  };
}

function displayName(record: PublicRecord): string {
  return String(record.name || record.title || record.symbol || record.path || record.id);
}

function count(snapshot: PublicSnapshot, resource: keyof PublicSnapshot): number {
  const value = snapshot[resource];
  return Array.isArray(value) ? value.length : 0;
}

function idSuffix(id: string): string {
  return id.split(":").slice(2).join(":") || id;
}

function resourceForRecord(record: PublicRecord): PublicResourceName | null {
  const explicit = String(record.public_resource || "");
  if ((PUBLIC_RESOURCES as readonly string[]).includes(explicit)) return explicit as PublicResourceName;
  const entityType = String(record.entity_type || "");
  const resource = entityType === "stack_node" ? "stack_nodes" : `${entityType}s`;
  return (PUBLIC_RESOURCES as readonly string[]).includes(resource) ? resource as PublicResourceName : null;
}

function resourceHref(record: PublicRecord): string {
  const resource = resourceForRecord(record);
  return resource ? `/resources/${resource}/${encodeURIComponent(record.id)}` : `/search?q=${encodeURIComponent(record.id)}&mode=exact`;
}

function evidenceHref(record: PublicRecord): string {
  return `/evidence/${encodeURIComponent(record.id)}`;
}

function stackHref(record: PublicRecord): string {
  if (record.entity_type === "stack_node") return `/stack/${String(record.slug || idSuffix(record.id))}`;
  return resourceHref(record);
}

function citationHref(ref: SourceRef): string | null {
  if (!ref.path || ref.status === "missing" || ref.commit === "unknown") return null;
  const span = ref.source_span ? `#L${ref.source_span.start_line}` : "";
  return `${ref.repository}/blob/${ref.commit}/${ref.path}${span}`;
}

function SourceCitation({ ref }: { ref: SourceRef | null | undefined }) {
  if (!ref) return null;
  const label = `${ref.path || ref.artifact_hash || "immutable evidence"}${ref.source_span ? `:${ref.source_span.start_line}` : ""}`;
  const href = citationHref(ref);
  return href ? <a className="tc-citation" href={href} target="_blank" rel="noreferrer">{label}</a> : <span className="tc-citation missing">{label} · {ref.reason || ref.status || "not available"}</span>;
}

function SnapshotBadge({ snapshot }: { snapshot: PublicSnapshot }) {
  return <p className="tc-citation">Snapshot {snapshot.snapshot.id} · commit {snapshot.snapshot.commit}</p>;
}

function RecordLink({ record, onNavigate, children, href: requestedHref }: { record: PublicRecord; onNavigate: (href: string) => void; children?: ReactNode; href?: string }) {
  const href = requestedHref || stackHref(record);
  return <a href={href} onClick={(event) => { event.preventDefault(); onNavigate(href); }}>{children || displayName(record)}</a>;
}

function SearchBox({ initialQuery, initialMode, onSearch }: { initialQuery: string; initialMode: SearchMode; onSearch: (query: string, mode: SearchMode) => void }) {
  const [query, setQuery] = useState(initialQuery);
  const [mode, setMode] = useState<SearchMode>(initialMode);
  useEffect(() => { setQuery(initialQuery); setMode(initialMode); }, [initialQuery, initialMode]);
  const submit = (event: FormEvent) => { event.preventDefault(); if (query.trim()) onSearch(query.trim(), mode); };
  return <form className="tc-search" role="search" onSubmit={submit}>
    <label className="tc-citation" htmlFor="public-search" style={{ position: "absolute", width: 1, height: 1, overflow: "hidden", clip: "rect(0 0 0 0)" }}>Search the public graph</label>
    <input id="public-search" value={query} onChange={(event) => setQuery(event.target.value)} placeholder="Search source, symbol, contract, gap…" />
    <select aria-label="Search mode" value={mode} onChange={(event) => setMode(event.target.value as SearchMode)}>
      <option value="semantic">Semantic</option><option value="exact">Exact</option><option value="symbol">Symbol</option><option value="relationship">Relationship</option>
    </select>
    <button type="submit">Search</button>
  </form>;
}

function Header({ route, onNavigate }: { route: Route; onNavigate: (href: string) => void }) {
  const active: SiteHeaderSection = route.page === "resource" || route.page === "evidence"
    ? "evidence"
    : route.page === "home" || route.page === "search" ? "overview" : route.page;
  const isPublicRoute = (href: string) => href === "/" || href === "/stack" || href.startsWith("/stack/") || href === "/learn" || href.startsWith("/learn/") || href === "/research" || href.startsWith("/research/") || href === "/search" || href.startsWith("/resources/") || href === "/evidence" || href.startsWith("/evidence/");
  return <SiteHeader active={active} onNavigate={onNavigate} intercept={isPublicRoute} />;
}

function EntityCollection({ title, records, snapshot, onNavigate, empty = "No records are attached to this view yet." }: { title: string; records: PublicRecord[]; snapshot: PublicSnapshot; onNavigate: (href: string) => void; empty?: string }) {
  return <section className="tc-panel">
    <h2>{title} <span className="tc-muted">({records.length})</span></h2>
    {records.length === 0 ? <div className="tc-empty">{empty}</div> : <ul>{records.map((record) => <li key={record.id}><RecordLink record={record} onNavigate={onNavigate} /><span className="tc-citation">{record.entity_type || "entity"} · {snapshot.snapshot.commit}</span></li>)}</ul>}
  </section>;
}

function HomePage({ snapshot, onNavigate, onSearch, route }: { snapshot: PublicSnapshot; onNavigate: (href: string) => void; onSearch: (query: string, mode: SearchMode) => void; route: Route }) {
  const nodes = [...snapshot.stack_nodes].sort((a, b) => Number(a.ordinal || 0) - Number(b.ordinal || 0));
  const featured = nodes;
  return <>
    <div className="tc-hero-row">
      <div><span className="tc-eyebrow">Public platform knowledge</span><h1>Understand the stack. Follow the evidence.</h1><p className="tc-lede">A static-first map of Trit from silicon to user surfaces. Every layer stays connected to the source, contract, test, benchmark, decision, release, and gap that qualify its claims.</p><SearchBox initialQuery={route.query} initialMode={route.mode} onSearch={onSearch} /></div>
      <aside className="tc-hero-panel"><span className="tc-eyebrow">Current snapshot</span><strong>{snapshot.snapshot.commit === "unknown" ? "Snapshot unavailable" : snapshot.snapshot.commit.substring(0, 12)}</strong><small>{snapshot.snapshot.repository.replace("https://github.com/", "")}</small><SnapshotBadge snapshot={snapshot} /></aside>
    </div>
    <div className="tc-metrics" aria-label="Snapshot metrics">
      <div className="tc-metric"><strong>{count(snapshot, "stack_nodes")}</strong><span>stack phases</span></div><div className="tc-metric"><strong>{count(snapshot, "capabilities")}</strong><span>capabilities</span></div><div className="tc-metric"><strong>{count(snapshot, "contracts")}</strong><span>contracts</span></div><div className="tc-metric"><strong>{count(snapshot, "sources")}</strong><span>source records</span></div>
    </div>
    <div className="tc-section-heading"><h2>Silicon → user dependency trail</h2><a href="/stack" onClick={(event) => { event.preventDefault(); onNavigate("/stack"); }}>Open full explorer →</a></div>
    <div className="tc-stack-trail" aria-label="Stack phases">{featured.map((node) => <RecordLink key={node.id} record={node} onNavigate={onNavigate}><b>PHASE {String(node.ordinal).padStart(2, "0")}</b><span>{displayName(node)}</span></RecordLink>)}</div>
    <div className="tc-section-heading"><h2>What the public graph makes reachable</h2></div>
    <div className="tc-card-grid"><div className="tc-card"><h3>Dependency views</h3><p>Move from an ordered stack phase to the contracts and capabilities it enables, with dependency edges kept visible.</p></div><div className="tc-card"><h3>Evidence views</h3><p>Open exact source paths, symbol spans, tests, benchmark definitions, decisions, and releases from one read-only surface.</p></div><div className="tc-card"><h3>Known gaps</h3><p>Open, planned, missing, and partial evidence remains labeled so navigation never turns a roadmap item into a production claim.</p></div></div>
    <div className="tc-section-heading"><h2>Release boundaries</h2></div>
    <EntityCollection title="Published releases" records={snapshot.releases} snapshot={snapshot} onNavigate={onNavigate} empty="No release record is available in this snapshot." />
  </>;
}

function relatedRecords(snapshot: PublicSnapshot, ids: unknown): PublicRecord[] {
  const lookup = new Map<string, PublicRecord>();
  for (const resource of PUBLIC_RESOURCES) for (const record of snapshot[resource]) lookup.set(record.id, record);
  return (Array.isArray(ids) ? ids : []).map((id) => lookup.get(String(id))).filter((record): record is PublicRecord => Boolean(record));
}

function recordsForRelation(snapshot: PublicSnapshot, id: string, direction: "from" | "to", types?: string[]): PublicRecord[] {
  const ids = new Set((snapshot.relations || [])
    .filter((relation) => relation[direction] === id && (!types || types.includes(relation.type)))
    .map((relation) => direction === "from" ? relation.to : relation.from));
  return relatedRecords(snapshot, [...ids]);
}

function SourcePanel({ title, refs, snapshot, onNavigate }: { title: string; refs: SourceRef[]; snapshot: PublicSnapshot; onNavigate: (href: string) => void }) {
  const safeRefs = (Array.isArray(refs) ? refs : []).filter(Boolean);
  return <section className="tc-panel"><h2>{title} <span className="tc-muted">({safeRefs.length})</span></h2>{safeRefs.length ? <ul>{safeRefs.map((ref, index) => { const source = ref.path ? snapshot.sources.find((candidate) => candidate.path === ref.path) : undefined; const label = `${ref.path || ref.artifact_hash || "immutable evidence"}${ref.source_span ? `:${ref.source_span.start_line}` : ""}`; return <li key={`${ref.path || ref.artifact_hash || "evidence"}-${index}`}>{source ? <RecordLink record={source} onNavigate={onNavigate} href={resourceHref(source)}>{label}</RecordLink> : <SourceCitation ref={ref} />}{source && citationHref(ref) ? <a className="tc-citation" href={citationHref(ref) || undefined} target="_blank" rel="noreferrer">open source</a> : null}{ref.role ? <span className="tc-citation">{ref.role}</span> : null}</li>; })}</ul> : <div className="tc-empty">No provenance recorded for this relation.</div>}</section>;
}

function StackDetail({ node, snapshot, onNavigate }: { node: PublicRecord; snapshot: PublicSnapshot; onNavigate: (href: string) => void }) {
  const dependencies = relatedRecords(snapshot, node.depends_on);
  const dependents = relatedRecords(snapshot, node.dependent_ids);
  const components = relatedRecords(snapshot, node.component_ids);
  const capabilities = relatedRecords(snapshot, node.capability_ids);
  const contracts = relatedRecords(snapshot, node.contract_ids);
  const decisions = relatedRecords(snapshot, node.decision_ids);
  const tests = relatedRecords(snapshot, node.test_ids);
  const benchmarks = relatedRecords(snapshot, node.benchmark_ids);
  const gaps = relatedRecords(snapshot, node.gap_ids);
  const releases = relatedRecords(snapshot, node.release_ids);
  const coverage = (node.coverage && typeof node.coverage === "object" ? node.coverage : {}) as Record<string, unknown>;
  const statusSummary = (node.status_summary && typeof node.status_summary === "object" ? node.status_summary : {}) as Record<string, unknown>;
  return <article className="tc-detail">
    <div className="tc-detail-header"><span className="tc-eyebrow">Phase {String(node.ordinal).padStart(2, "0")}</span><h1>{displayName(node)}</h1><p className="tc-lede">{String(node.problem || node.description || "No problem statement recorded.")}</p><span className="tc-detail-id">{node.id} · source id {String(node.source_id || "not recorded")} · implementation {String(node.implementation_status || "unrecorded")}</span></div>
    <section className="tc-panel"><h2>Phase contract</h2><dl className="tc-facts"><dt>Problem</dt><dd>{String(node.problem || node.description || "Not recorded.")}</dd><dt>Inputs</dt><dd>{String(node.inputs || "Not recorded.")}</dd><dt>Outputs</dt><dd>{String(node.outputs || "Not recorded.")}</dd><dt>Implementation status</dt><dd>{String(node.implementation_status || "unrecorded")}</dd></dl></section>
    <section className="tc-panel"><h2>Coverage status</h2><div className="tc-status-grid">{Object.entries(statusSummary).map(([dimension, status]) => <div className="tc-status-item" key={dimension}><b>{dimension}</b><span>{String(status)}</span>{coverage[dimension] && typeof coverage[dimension] === "object" ? <small>{String((coverage[dimension] as Record<string, unknown>).evidence_refs ? `${((coverage[dimension] as Record<string, unknown>).evidence_refs as unknown[]).length} evidence refs` : "")}</small> : null}</div>)}</div></section>
    <div className="tc-section-heading"><h2>Dependencies</h2></div>
    {dependencies.length ? <div className="tc-stack-trail">{dependencies.map((record) => <RecordLink key={record.id} record={record} onNavigate={onNavigate}><b>REQUIRES</b><span>{displayName(record)}</span></RecordLink>)}</div> : <div className="tc-empty">This is the root of the recorded stack.</div>}
    <EntityCollection title="Dependent phases" records={dependents} snapshot={snapshot} onNavigate={onNavigate} empty="No later phase declares this phase as a dependency." />
    <div className="tc-detail-columns">
      <EntityCollection title="Components" records={components} snapshot={snapshot} onNavigate={onNavigate} empty="No source component is attached to this phase." />
      <EntityCollection title="Capabilities" records={capabilities} snapshot={snapshot} onNavigate={onNavigate} />
      <EntityCollection title="Contracts" records={contracts} snapshot={snapshot} onNavigate={onNavigate} />
      <EntityCollection title="Decisions" records={decisions} snapshot={snapshot} onNavigate={onNavigate} empty="No decision is directly attached to this phase." />
      <EntityCollection title="Tests" records={tests} snapshot={snapshot} onNavigate={onNavigate} />
      <EntityCollection title="Benchmarks" records={benchmarks} snapshot={snapshot} onNavigate={onNavigate} empty="No benchmark is attached to this phase." />
      <EntityCollection title="Known gaps" records={gaps} snapshot={snapshot} onNavigate={onNavigate} empty="No known gap is attached to this phase." />
      <EntityCollection title="Releases" records={releases} snapshot={snapshot} onNavigate={onNavigate} empty="No release boundary is attached to this phase." />
      <SourcePanel title="Source" refs={(node.source_refs as SourceRef[]) || []} snapshot={snapshot} onNavigate={onNavigate} />
      <SourcePanel title="Evidence" refs={(node.evidence_refs as SourceRef[]) || []} snapshot={snapshot} onNavigate={onNavigate} />
    </div>
  </article>;
}

function StackPage({ snapshot, route, onNavigate, onSearch }: { snapshot: PublicSnapshot; route: Route; onNavigate: (href: string) => void; onSearch: (query: string, mode: SearchMode) => void }) {
  const nodes = useMemo(() => [...snapshot.stack_nodes].sort((a, b) => Number(a.ordinal || 0) - Number(b.ordinal || 0)), [snapshot.stack_nodes]);
  const requested = route.stackSlug || (route.focus ? idSuffix(route.focus) : "");
  const selected = nodes.find((node) => node.slug === requested || idSuffix(node.id) === requested) || nodes[0];
  return <>
    <div className="tc-hero-row"><div><span className="tc-eyebrow">Public stack explorer</span><h1>Trace every dependency.</h1><p className="tc-lede">Select a phase to move through silicon, ISA, VM, compiler, kernel, and user-facing layers. Each record is a pointer into the snapshot graph, not duplicated source prose.</p></div><div><SearchBox initialQuery={route.query} initialMode={route.mode} onSearch={onSearch} /><SnapshotBadge snapshot={snapshot} /></div></div>
    <div className="tc-layout"><aside className="tc-sidebar" aria-label="Stack phases"><h2>Stack phases <span className="tc-muted">{nodes.length}</span></h2>{nodes.map((node) => <a key={node.id} href={stackHref(node)} aria-current={selected?.id === node.id ? "page" : undefined} onClick={(event) => { event.preventDefault(); onNavigate(stackHref(node)); }}><b>PHASE {String(node.ordinal).padStart(2, "0")}</b><span>{displayName(node)}</span></a>)}</aside>{selected ? <StackDetail node={selected} snapshot={snapshot} onNavigate={onNavigate} /> : <div className="tc-empty">No stack nodes are available.</div>}</div>
  </>;
}

function publicLearningInline(text: string): ReactNode[] {
  const nodes: ReactNode[] = [];
  let remaining = text;
  let key = 0;
  while (remaining) {
    const link = remaining.match(/^\[([^\]]+)\]\(([^)]+)\)/);
    if (link) {
      const external = /^(https?:|mailto:)/.test(link[2]);
      nodes.push(<a key={key++} href={link[2]} target={external ? "_blank" : undefined} rel={external ? "noreferrer" : undefined}>{link[1]}</a>);
      remaining = remaining.slice(link[0].length);
      continue;
    }
    const code = remaining.match(/^`([^`]+)`/);
    if (code) {
      nodes.push(<code key={key++}>{code[1]}</code>);
      remaining = remaining.slice(code[0].length);
      continue;
    }
    const strong = remaining.match(/^\*\*([^*]+)\*\*/);
    if (strong) {
      nodes.push(<strong key={key++}>{strong[1]}</strong>);
      remaining = remaining.slice(strong[0].length);
      continue;
    }
    const next = remaining.search(/\[|`|\*\*/);
    const textEnd = next < 0 ? remaining.length : next || 1;
    nodes.push(remaining.slice(0, textEnd));
    remaining = remaining.slice(textEnd);
  }
  return nodes;
}

function PublicLearningContent({ content }: { content: string }) {
  return (
    <div className="tc-learning-content">
      {parseMarkdown(content).map((block: Block, index) => {
        if (block.type === "p") return <p key={index}>{publicLearningInline(block.content)}</p>;
        if (block.type === "h2") return <h3 key={index}>{publicLearningInline(block.content)}</h3>;
        if (block.type === "h3") return <h4 key={index}>{publicLearningInline(block.content)}</h4>;
        if (block.type === "h4") return <h5 key={index}>{publicLearningInline(block.content)}</h5>;
        if (block.type === "code") return <pre key={index}><code>{block.content}</code></pre>;
        if (block.type === "ul") return <ul key={index}>{block.items?.map((item) => <li key={item}>{publicLearningInline(item)}</li>)}</ul>;
        if (block.type === "ol") return <ol key={index}>{block.items?.map((item) => <li key={item}>{publicLearningInline(item)}</li>)}</ol>;
        return (
          <div className="tc-learning-table" key={index}>
            <table>
              <thead><tr>{block.headers?.map((header) => <th key={header}>{publicLearningInline(header)}</th>)}</tr></thead>
              <tbody>{block.rows?.map((row, rowIndex) => <tr key={rowIndex}>{row.map((cell, cellIndex) => <td key={`${rowIndex}-${cellIndex}`}>{publicLearningInline(cell)}</td>)}</tr>)}</tbody>
            </table>
          </div>
        );
      })}
    </div>
  );
}

function PublicLearningCheck({ module }: { module: LearningInteractive }) {
  const [selected, setSelected] = useState<number | null>(null);
  const [feedback, setFeedback] = useState("");
  const [code, setCode] = useState(module.kind === "code" ? module.starter : "");
  const [traceStep, setTraceStep] = useState(-1);
  const [sourceAnswer, setSourceAnswer] = useState("");
  const [running, setRunning] = useState(false);

  if (module.kind === "choice") {
    return (
      <section className="tc-learning-check" aria-labelledby="public-learning-check-title">
        <span className="tc-eyebrow">Interactive check</span>
        <h2 id="public-learning-check-title">{module.title}</h2>
        <fieldset>
          <legend>{module.prompt}</legend>
          {module.options.map((option, index) => (
            <label key={option}>
              <input type="radio" name={`public-learning-${module.title}`} checked={selected === index} onChange={() => { setSelected(index); setFeedback(""); }} />
              {option}
            </label>
          ))}
        </fieldset>
        <button data-testid="learning-choice-check" type="button" className="tc-secondary" disabled={selected === null} onClick={() => setFeedback(selected === module.answer ? `Correct. ${module.explanation}` : "Not yet. Re-read this page and try again.")}>Check answer</button>
        <p data-testid="learning-feedback" aria-live="polite" className="tc-learning-feedback">{feedback}</p>
      </section>
    );
  }

  if (module.kind === "trace") {
    const visibleStep = traceStep >= 0 ? module.steps[traceStep] : null;
    const advance = () => {
      const nextStep = Math.min(traceStep + 1, module.steps.length - 1);
      setTraceStep(nextStep);
      const step = module.steps[nextStep];
      setFeedback(`${step.label}: ${step.explanation}`);
    };
    return (
      <section className="tc-learning-check" data-testid="learning-trace" aria-labelledby="public-learning-check-title">
        <span className="tc-eyebrow">State trace</span>
        <h2 id="public-learning-check-title">{module.title}</h2>
        <p>{module.prompt}</p>
        <ol className="tc-trace-steps">
          {module.steps.map((step, index) => <li key={step.label} aria-current={index === traceStep ? "step" : undefined} className={index <= traceStep ? "is-visible" : ""}><strong>{step.label}</strong><code>{index <= traceStep ? step.state : "locked until the prior boundary"}</code></li>)}
        </ol>
        <button data-testid="learning-trace-next" type="button" className="tc-primary" onClick={advance} disabled={traceStep >= module.steps.length - 1}>{traceStep < 0 ? "Start trace" : traceStep >= module.steps.length - 1 ? "Trace complete" : "Advance trace"}</button>
        {visibleStep ? <p data-testid="learning-feedback" aria-live="polite" className="tc-learning-feedback">{feedback}</p> : <p data-testid="learning-feedback" aria-live="polite" className="tc-learning-feedback">Advance one state at a time; the trace is deterministic and bounded.</p>}
      </section>
    );
  }

  if (module.kind === "source") {
    const verifySource = () => {
      const expected = module.expectedIncludes.every((fragment) => sourceAnswer.includes(fragment));
      setFeedback(expected ? `Source boundary verified. ${module.explanation}` : `Enter the repository filename shown in the source link: ${module.sourcePath}.`);
    };
    return (
      <section className="tc-learning-check" data-testid="learning-source-investigation" aria-labelledby="public-learning-check-title">
        <span className="tc-eyebrow">Source investigation</span>
        <h2 id="public-learning-check-title">{module.title}</h2>
        <p>{module.prompt}</p>
        <p><a href={module.sourceHref} target="_blank" rel="noreferrer">Open {module.sourcePath}</a></p>
        <label htmlFor="learning-source-answer">Name the repository file you inspected</label>
        <input id="learning-source-answer" value={sourceAnswer} onChange={(event) => setSourceAnswer(event.target.value)} />
        <button data-testid="learning-source-check" type="button" className="tc-secondary" onClick={verifySource}>Verify source</button>
        <p data-testid="learning-feedback" aria-live="polite" className="tc-learning-feedback">{feedback}</p>
      </section>
    );
  }

  const validate = () => {
    const missing = module.expectedIncludes.filter((fragment) => !code.includes(fragment));
    setFeedback(missing.length ? `Add the expected TCL shape: ${missing.join(", ")}.` : `Example shape verified. ${module.explanation}`);
  };
  const runExercise = async () => {
    setRunning(true);
    setFeedback("Submitting to the bounded repository-backed compiler…");
    try {
      const token = window.localStorage.getItem("treatcode.auth.token") || "";
      const response = await fetch("/api/learn/exercises/run", { method: "POST", headers: { "Content-Type": "application/json", Accept: "application/json", ...(token ? { Authorization: `Bearer ${token}` } : {}), "X-TreatCode-Project": "tc:project:trit" }, body: JSON.stringify({ exercise_id: module.exercise_id, code }) });
      const payload = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(String(payload.error || `runner returned ${response.status}`));
      setFeedback(payload.success ? `Compiler/VM execution passed. ${payload.summary || module.explanation}` : `Compiler/VM execution reported a real failure. ${payload.error || module.explanation}`);
    } catch (error) {
      setFeedback(`Exercise unavailable: ${String(error instanceof Error ? error.message : error)}. The local shape check remains available.`);
    } finally {
      setRunning(false);
    }
  };
  return (
    <section className="tc-learning-check" data-testid="learning-code-exercise" aria-labelledby="public-learning-check-title">
      <span className="tc-eyebrow">Interactive code module</span>
      <h2 id="public-learning-check-title">{module.title}</h2>
      <label htmlFor="public-tcl-practice">Edit the example, then validate it in the browser.</label>
      <textarea id="public-tcl-practice" aria-label="TCL practice code" value={code} onChange={(event) => setCode(event.target.value)} spellCheck={false} />
      <div className="tc-learning-actions"><button data-testid="learning-code-validate" type="button" className="tc-secondary" onClick={validate}>Check local shape</button><button data-testid="learning-code-run" type="button" className="tc-primary" onClick={() => void runExercise()} disabled={running}>{running ? "Running…" : "Run compiler / VM"}</button><a className="tc-secondary" href="/practice">Open challenges</a></div>
      <p data-testid="learning-feedback" aria-live="polite" className="tc-learning-feedback">{feedback}</p>
    </section>
  );
}

function learningSourceHref(snapshot: PublicSnapshot, repositoryPath: string): string {
  const repository = snapshot.snapshot.repository || "https://github.com/JonasComlita/dualrail";
  const commit = snapshot.snapshot.commit && snapshot.snapshot.commit !== "unknown" ? snapshot.snapshot.commit : "main";
  return `${repository}/blob/${commit}/${repositoryPath}`;
}

function PublicLearningProvenance({ page, snapshot }: { page: LearningPage; snapshot: PublicSnapshot }) {
  const phase = snapshot.stack_nodes.find((node) => node.id === page.phase_id);
  const records = (ids: string[] | undefined) => relatedRecords(snapshot, ids);
  const references = (title: string, items: LearningPage["sources"]) => (
    <section className="tc-panel">
      <h2>{title}</h2>
      <ul>
        {items.map((item) => <li key={item.path}><a href={learningSourceHref(snapshot, item.path)} target="_blank" rel="noreferrer">{item.label}</a><span className="tc-citation">{item.path}</span></li>)}
      </ul>
    </section>
  );
  const linkedRecords = (title: string, items: PublicRecord[], empty: string) => (
    <section className="tc-panel" data-testid={`learning-${title.toLowerCase().replace(/ /g, "-")}`}>
      <h2>{title} <span className="tc-muted">({items.length})</span></h2>
      {items.length ? <ul>{items.map((record) => <li key={record.id}><a href={stackHref(record)}>{displayName(record)}</a><span className="tc-citation">{record.id}</span></li>)}</ul> : <div className="tc-empty">{empty}</div>}
    </section>
  );
  return <div className="tc-detail-columns" data-testid="learning-provenance">
    {references("Production source", page.sources)}
    {references("Validation evidence", page.evidence)}
    <section className="tc-panel"><h2>Stack Explorer</h2><p>Current phase: <a href={`/stack/${page.phase_slug}`}>{page.phase_name}</a></p><p className="tc-citation">{page.phase_id} · status {page.implementation_status}</p>{phase?.coverage ? <p className="tc-citation">implemented {String((phase.coverage as Record<string, unknown>).implemented || "recorded")} · tested {String((phase.coverage as Record<string, unknown>).tested || "recorded")}</p> : null}</section>
    {linkedRecords("Tests", records(page.test_ids), "No test record is attached to this lesson.")}
    {linkedRecords("Benchmarks", records(page.benchmark_ids), "No benchmark is attached to this lesson.")}
    {linkedRecords("Known gaps", records(page.gap_ids), "No known gap is attached to this lesson.")}
  </div>;
}

function MarkdownLearnPage({ snapshot, onNavigate, route, onSearch }: { snapshot: PublicSnapshot; onNavigate: (href: string) => void; route: Route; onSearch: (query: string, mode: SearchMode) => void }) {
  const selectedPath = LEARNING_CATALOG.paths.find((path) => path.id === route.learningPathId) || LEARNING_CATALOG.paths[0];
  const pages = learningPathPages(selectedPath);
  const selectedPage = pages.find((page) => page.id === route.learningTopicId) || pages[0];
  const selectedIndex = selectedPage ? pages.findIndex((page) => page.id === selectedPage.id) : -1;
  const previousPage = selectedIndex > 0 ? pages[selectedIndex - 1] : null;
  const nextPage = selectedIndex >= 0 && selectedIndex < pages.length - 1 ? pages[selectedIndex + 1] : null;
  const learnHref = (pathId: string, topicId?: string) => topicId ? `/learn/${encodeURIComponent(pathId)}/${encodeURIComponent(topicId)}` : `/learn/${encodeURIComponent(pathId)}`;
  const phaseCoverage = new Set(pages.map((page) => page.phase_id)).size;
  return <>
    <div className="tc-hero-row"><div><span className="tc-eyebrow">Evidence-linked learning</span><h1>Learn the whole Trit stack.</h1><p className="tc-lede">Every lesson is a direct, Markdown-backed route connected to its current implementation, planned gaps, exact source, and validation evidence.</p></div><div><SearchBox initialQuery={route.query} initialMode={route.mode} onSearch={onSearch} /><SnapshotBadge snapshot={snapshot} /></div></div>
    <div className="tc-section-heading"><h2>Choose a path</h2><span className="tc-muted">{selectedPath.title}</span></div>
    <div className="tc-card-grid">{LEARNING_CATALOG.paths.map((path) => <article className="tc-card" key={path.id}><span className="tc-eyebrow">{path.title}</span><h3>{path.audience}</h3><p>{path.description}</p><p style={{ marginTop: 12 }}><a href={learnHref(path.id)} onClick={(event) => { event.preventDefault(); onNavigate(learnHref(path.id)); }}>{path.page_ids.length} ordered lessons · {path.required_phase_coverage === "all" ? "all phases" : "selected phases"}</a></p></article>)}</div>
    <div className="tc-learning-progress" data-testid="learning-progress" aria-label="Learning coverage"><strong>{selectedIndex >= 0 ? selectedIndex + 1 : 0} / {pages.length}</strong><span>{phaseCoverage} / {LEARNING_MATRIX.phase_count} phases reachable in this path</span><div role="progressbar" aria-valuemin={0} aria-valuemax={pages.length} aria-valuenow={Math.max(0, selectedIndex + 1)}><i style={{ width: `${pages.length ? Math.max(0, (selectedIndex + 1) / pages.length * 100) : 0}%` }} /></div></div>
    <div className="tc-layout tc-learning-layout"><aside className="tc-sidebar" aria-label="Learning pages"><h2>{selectedPath.title} <span className="tc-muted">{pages.length}</span></h2>{pages.map((page, index) => <a data-testid="learning-page-link" key={page.id} href={learnHref(selectedPath.id, page.id)} aria-current={selectedPage?.id === page.id ? "page" : undefined} onClick={(event) => { event.preventDefault(); onNavigate(learnHref(selectedPath.id, page.id)); }}><b>{String(index + 1).padStart(2, "0")} · {page.module}</b><span>{page.title}</span></a>)}</aside>
      {selectedPage ? <article className="tc-detail"><header className="tc-detail-header"><span className="tc-eyebrow">{selectedPage.module} · {selectedPage.level} · {selectedPage.lesson_kind}</span><h2>{selectedPage.title}</h2><p className="tc-lede">{selectedPage.summary}</p><span className="tc-detail-id">Status: {selectedPage.implementation_status} · Prerequisites: {selectedPage.prerequisites.length ? selectedPage.prerequisites.map((id) => <span key={id}> <a href={learnHref(selectedPath.id, id)}>{LEARNING_PAGES.find((page) => page.id === id)?.title || id}</a></span>) : "none"}</span></header><PublicLearningContent content={selectedPage.content} /><PublicLearningCheck module={selectedPage.interactive} /><PublicLearningProvenance page={selectedPage} snapshot={snapshot} /><nav className="tc-learning-next" aria-label="Lesson progression">{previousPage ? <a data-testid="learning-previous" href={learnHref(selectedPath.id, previousPage.id)} onClick={(event) => { event.preventDefault(); onNavigate(learnHref(selectedPath.id, previousPage.id)); }}>← {previousPage.title}</a> : <a href={learnHref(selectedPath.id)}>Path start</a>}{nextPage ? <a data-testid="learning-next" href={learnHref(selectedPath.id, nextPage.id)} onClick={(event) => { event.preventDefault(); onNavigate(learnHref(selectedPath.id, nextPage.id)); }}>{nextPage.title} →</a> : <a href="/stack/closure-evidence">Open closure evidence →</a>}</nav></article> : <div className="tc-empty">No learning page is available.</div>}
    </div>
  </>;
}

const PUBLIC_PAGE_SIZE = 50;

function publicRecordMap(snapshot: PublicSnapshot): Map<string, PublicRecord> {
  const records = new Map<string, PublicRecord>();
  for (const resource of PUBLIC_RESOURCES) for (const record of snapshot[resource]) records.set(record.id, record);
  return records;
}

function publicRecord(snapshot: PublicSnapshot, id: string | undefined): PublicRecord | undefined {
  return id ? publicRecordMap(snapshot).get(id) : undefined;
}

function pageHref(baseHref: string, page: number): string {
  const [pathname, query = ""] = baseHref.split("?", 2);
  const params = new URLSearchParams(query);
  if (page <= 1) params.delete("page");
  else params.set("page", String(page));
  const serialized = params.toString();
  return serialized ? `${pathname}?${serialized}` : pathname;
}

function PaginationNav({ baseHref, page, total, pageSize, onNavigate }: { baseHref: string; page: number; total: number; pageSize: number; onNavigate: (href: string) => void }) {
  const pageCount = Math.max(1, Math.ceil(total / pageSize));
  if (pageCount <= 1) return <p className="tc-citation">All {total} records are on this page.</p>;
  const previous = page > 1 ? pageHref(baseHref, page - 1) : null;
  const next = page < pageCount ? pageHref(baseHref, page + 1) : null;
  const open = (event: ReactMouseEvent<HTMLAnchorElement>, href: string) => { event.preventDefault(); onNavigate(href); };
  return <nav className="tc-pagination" aria-label="Pagination"><span>Page {Math.min(page, pageCount)} of {pageCount} · {total} total records</span><span>{previous ? <a href={previous} onClick={(event) => open(event, previous)}>← Previous</a> : <span aria-hidden="true">← Previous</span>} {next ? <a href={next} onClick={(event) => open(event, next)}>Next →</a> : <span aria-hidden="true">Next →</span>}</span></nav>;
}

function PublicValue({ value, snapshot, onNavigate, depth = 0 }: { value: unknown; snapshot: PublicSnapshot; onNavigate: (href: string) => void; depth?: number }): ReactNode {
  if (value === null || value === undefined) return <span className="tc-muted">—</span>;
  if (depth > 3) return <span>{String(value)}</span>;
  if (Array.isArray(value)) return value.length ? <ul className="tc-value-list">{value.map((item, index) => <li key={index}><PublicValue value={item} snapshot={snapshot} onNavigate={onNavigate} depth={depth + 1} /></li>)}</ul> : <span className="tc-muted">none</span>;
  if (typeof value === "object") return <dl className="tc-value-list">{Object.entries(value as Record<string, unknown>).map(([key, item]) => <div key={key}><dt>{key}</dt><dd><PublicValue value={item} snapshot={snapshot} onNavigate={onNavigate} depth={depth + 1} /></dd></div>)}</dl>;
  if (typeof value === "string") {
    const linked = publicRecord(snapshot, value);
    return linked ? <RecordLink record={linked} href={resourceHref(linked)} onNavigate={onNavigate}>{value}</RecordLink> : <span>{value}</span>;
  }
  return <span>{String(value)}</span>;
}

function RecordDetailPage({ record, snapshot, onNavigate, evidenceMode = false }: { record: PublicRecord; snapshot: PublicSnapshot; onNavigate: (href: string) => void; evidenceMode?: boolean }) {
  const relationRows = (snapshot.relations || []).filter((relation) => relation.from === record.id || relation.to === record.id);
  const related = relationRows.map((relation) => publicRecord(snapshot, relation.from === record.id ? relation.to : relation.from)).filter((item): item is PublicRecord => Boolean(item));
  const fields = Object.entries(record).filter(([key]) => !["id", "entity_type", "name", "title", "source_refs", "evidence_refs"].includes(key)).sort(([left], [right]) => left.localeCompare(right));
  const route = resourceHref(record);
  return <article className="tc-detail">
    <header className="tc-detail-header"><span className="tc-eyebrow">{evidenceMode ? "Evidence record" : `${resourceForRecord(record) || "Public record"} detail`}</span><h1>{displayName(record)}</h1><p className="tc-lede">{String(record.description || record.title || "Repository-backed public record.")}</p><span className="tc-detail-id">{record.id} · {String(record.entity_type || "entity")}</span><p><a href={route} onClick={(event) => { event.preventDefault(); onNavigate(route); }}>Canonical resource route</a> · <a href={evidenceHref(record)} onClick={(event) => { event.preventDefault(); onNavigate(evidenceHref(record)); }}>Evidence view</a></p></header>
    <section className="tc-panel"><h2>Record fields</h2><dl className="tc-facts">{fields.map(([key, value]) => <div key={key}><dt>{key}</dt><dd><PublicValue value={value} snapshot={snapshot} onNavigate={onNavigate} /></dd></div>)}</dl></section>
    <div className="tc-detail-columns"><SourcePanel title="Source provenance" refs={(record.source_refs as SourceRef[]) || []} snapshot={snapshot} onNavigate={onNavigate} /><SourcePanel title="Evidence provenance" refs={(record.evidence_refs as SourceRef[]) || []} snapshot={snapshot} onNavigate={onNavigate} /></div>
    <section className="tc-panel"><h2>Relationships <span className="tc-muted">({relationRows.length})</span></h2>{relationRows.length ? <ul>{relationRows.map((relation, index) => { const outgoing = relation.from === record.id; const other = publicRecord(snapshot, outgoing ? relation.to : relation.from); return <li key={`${relation.from}-${relation.type}-${relation.to}-${index}`}><span className="tc-citation">{outgoing ? relation.type : `inverse ${relation.type}`}</span> {other ? <RecordLink record={other} href={resourceHref(other)} onNavigate={onNavigate}>{displayName(other)}</RecordLink> : <span>{outgoing ? relation.to : relation.from}</span>}<span className="tc-citation"> · {relation.origin || "public registry"}</span></li>; })}</ul> : <div className="tc-empty">No relationship edge is recorded for this entity.</div>}</section>
    {related.length ? <EntityCollection title="Related records" records={related} snapshot={snapshot} onNavigate={onNavigate} /> : null}
  </article>;
}

function ResourceCollectionPage({ snapshot, route, onNavigate }: { snapshot: PublicSnapshot; route: Route; onNavigate: (href: string) => void }) {
  const resourceName = (PUBLIC_RESOURCES as readonly string[]).includes(route.resource || "") ? route.resource as PublicResourceName : null;
  if (!resourceName) return <section className="tc-empty"><h1>Unknown public resource</h1><p>Choose one of the versioned public collections.</p><p>{PUBLIC_RESOURCES.map((resource) => <span key={resource}> <a href={`/resources/${resource}`} onClick={(event) => { event.preventDefault(); onNavigate(`/resources/${resource}`); }}>{resource}</a></span>)}</p></section>;
  const collection = snapshot[resourceName];
  const first = (route.pageNumber - 1) * PUBLIC_PAGE_SIZE;
  const pageRecords = collection.filter((_record, index) => index >= first && index < first + PUBLIC_PAGE_SIZE);
  const baseHref = `/resources/${resourceName}`;
  return <><div className="tc-hero-row"><div><span className="tc-eyebrow">Public resource collection</span><h1>{resourceName}</h1><p className="tc-lede">Every record in this collection is reachable through a deterministic, read-only route and carries source and evidence provenance.</p></div><div><SnapshotBadge snapshot={snapshot} /><p className="tc-citation">{collection.length} total records · page size {PUBLIC_PAGE_SIZE}</p></div></div><PaginationNav baseHref={baseHref} page={route.pageNumber} total={collection.length} pageSize={PUBLIC_PAGE_SIZE} onNavigate={onNavigate} /><EntityCollection title={`${resourceName} records`} records={pageRecords} snapshot={snapshot} onNavigate={onNavigate} empty="This authoritative collection is empty." /></>;
}

function EvidencePage({ snapshot, route, onNavigate }: { snapshot: PublicSnapshot; route: Route; onNavigate: (href: string) => void }) {
  const record = publicRecord(snapshot, route.recordId);
  if (record) return <><div className="tc-section-heading"><h1>Evidence for {displayName(record)}</h1><span className="tc-muted">{record.id}</span></div><RecordDetailPage record={record} snapshot={snapshot} onNavigate={onNavigate} evidenceMode /></>;
  return <><div className="tc-hero-row"><div><span className="tc-eyebrow">Evidence index</span><h1>Follow every public claim to its source.</h1><p className="tc-lede">Browse the complete versioned collections, then open any record's evidence route for its source spans, artifact hashes, and relationship context.</p></div><div><SnapshotBadge snapshot={snapshot} /></div></div><div className="tc-card-grid">{PUBLIC_RESOURCES.map((resource) => <article className="tc-card" key={resource}><span className="tc-eyebrow">{resource}</span><h2>{snapshot[resource].length} records</h2><p>Deterministic collection and evidence routes are available for every record.</p><p><a href={`/resources/${resource}`} onClick={(event) => { event.preventDefault(); onNavigate(`/resources/${resource}`); }}>Browse {resource} →</a></p></article>)}</div></>;
}

function SearchPage({ snapshot, route, onNavigate, onSearch }: { snapshot: PublicSnapshot; route: Route; onNavigate: (href: string) => void; onSearch: (query: string, mode: SearchMode) => void }) {
  const results = useMemo(() => searchSnapshot(snapshot, route.query, route.mode), [snapshot, route.query, route.mode]);
  const first = (route.pageNumber - 1) * PUBLIC_PAGE_SIZE;
  const pageResults = results.filter((_result, index) => index >= first && index < first + PUBLIC_PAGE_SIZE);
  const baseHref = `/search?q=${encodeURIComponent(route.query)}&mode=${route.mode}`;
  return <><div className="tc-hero-row"><div><span className="tc-eyebrow">Provenance search</span><h1>Find the exact boundary.</h1><p className="tc-lede">Search IDs and paths exactly, resolve symbols, inspect relationships, or discover related concepts. Every match carries its source provenance and a reason for the match.</p></div><div><SearchBox initialQuery={route.query} initialMode={route.mode} onSearch={onSearch} /><SnapshotBadge snapshot={snapshot} /></div></div><div className="tc-section-heading"><h2>{route.query ? `Results for “${route.query}”` : "Search the public graph"}</h2><span className="tc-muted">{results.length} total matches · {route.mode}</span></div><PaginationNav baseHref={baseHref} page={route.pageNumber} total={results.length} pageSize={PUBLIC_PAGE_SIZE} onNavigate={onNavigate} />{pageResults.length ? <div className="tc-card-grid">{pageResults.map((result) => { const record = publicRecord(snapshot, result.entity_id); const href = record ? resourceHref(record) : `/search?q=${encodeURIComponent(result.entity_id)}&mode=exact`; return <article className="tc-card" key={`${result.entity_id}-${result.match_type}`}><span className="tc-eyebrow">{result.entity_type} · score {result.score}</span><h3><a href={href} onClick={(event) => { event.preventDefault(); onNavigate(href); }}>{result.name}</a></h3><p>{result.matched_fields.join(", ")} match · {result.match_type} search</p><p>{result.match_reason}</p>{result.provenance.map((ref, index) => <SourceCitation key={`${ref.path || ref.artifact_hash || "ref"}-${index}`} ref={ref} />)}</article>; })}</div> : <div className="tc-empty">{route.query ? "No authoritative records matched this query. Try a symbol name, exact path, or relationship phrase." : "Enter a query to search the snapshot."}</div>}</>;
}

type ResearchEntry = (typeof researchCatalog.records)[number];
type ResearchSort = "recommended" | "newest" | "oldest" | "title" | "author";

const RESEARCH_COLLECTION_ORDER = [
  "root_preexisting",
  "01_history_architecture",
  "02_circuits_devices",
  "03_optical_emerging",
  "04_theory_security_applications",
  "05_ai_industry_patents",
];

const RESEARCH_COLLECTION_DESCRIPTIONS: Record<string, string> = {
  root_preexisting: "Core references gathered for the TreatCode research library.",
  "01_history_architecture": "The historical machines, representations, and architecture of ternary computing.",
  "02_circuits_devices": "Logic gates, arithmetic, memory, circuits, and physical device implementations.",
  "03_optical_emerging": "Optical, memristive, and other emerging approaches to ternary hardware.",
  "04_theory_security_applications": "Formal theory, security, algorithms, and applied ternary systems.",
  "05_ai_industry_patents": "Recent AI and industry work, patents, and directions for practical systems.",
};

function researchCollectionLabel(collection: string): string {
  const labels: Record<string, string> = {
    root_preexisting: "Core references",
    "01_history_architecture": "History & architecture",
    "02_circuits_devices": "Circuits & devices",
    "03_optical_emerging": "Optical & emerging",
    "04_theory_security_applications": "Theory, security & applications",
    "05_ai_industry_patents": "AI, industry & patents",
  };
  return labels[collection] || collection.replace(/^\d+_/, "").replace(/_/g, " ");
}

function researchCollectionRank(collection: string): number {
  const rank = RESEARCH_COLLECTION_ORDER.indexOf(collection);
  return rank === -1 ? RESEARCH_COLLECTION_ORDER.length : rank;
}

function researchCollectionDescription(collection: string): string {
  return RESEARCH_COLLECTION_DESCRIPTIONS[collection] || "Research papers related to ternary computing.";
}

function researchYear(entry: ResearchEntry): number {
  const year = entry.year?.match(/\b\d{4}\b/)?.[0];
  return year ? Number(year) : 0;
}

function compareResearchEntries(left: ResearchEntry, right: ResearchEntry, sort: ResearchSort): number {
  if (sort === "recommended") {
    const collectionOrder = researchCollectionRank(left.collection) - researchCollectionRank(right.collection);
    if (collectionOrder !== 0) return collectionOrder;
    const yearOrder = researchYear(right) - researchYear(left);
    if (yearOrder !== 0) return yearOrder;
  }
  if (sort === "newest" || sort === "oldest") {
    const yearOrder = sort === "newest" ? researchYear(right) - researchYear(left) : researchYear(left) - researchYear(right);
    if (yearOrder !== 0) return yearOrder;
  }
  if (sort === "author") {
    const authorOrder = (left.authors || "").localeCompare(right.authors || "");
    if (authorOrder !== 0) return authorOrder;
  }
  return left.title.localeCompare(right.title);
}

function researchCollectionId(collection: string): string {
  return `research-group-${collection.replace(/[^a-z0-9]+/gi, "-").replace(/^-|-$/g, "").toLowerCase()}`;
}

function ResearchPage() {
  const [query, setQuery] = useState("");
  const [collection, setCollection] = useState("all");
  const [availability, setAvailability] = useState("all");
  const [sort, setSort] = useState<ResearchSort>("recommended");
  useEffect(() => {
    const previousTitle = document.title;
    document.title = "Research Library · TreatCode";
    return () => { document.title = previousTitle; };
  }, []);
  const collections = useMemo(() => [...new Set(researchCatalog.records.map((entry) => entry.collection))].sort((left, right) => researchCollectionRank(left) - researchCollectionRank(right)), []);
  const collectionStats = useMemo(() => collections.map((item) => {
    const entries = researchCatalog.records.filter((entry) => entry.collection === item);
    return { collection: item, total: entries.length, local: entries.filter((entry) => entry.status === "local").length };
  }), [collections]);
  const records = useMemo(() => {
    const normalizedQuery = query.trim().toLowerCase();
    return researchCatalog.records
      .filter((entry) => collection === "all" || entry.collection === collection)
      .filter((entry) => availability === "all" || entry.status === availability)
      .filter((entry) => !normalizedQuery || [entry.title, entry.authors, entry.venue, entry.kind, entry.identifier, entry.notes].join(" ").toLowerCase().includes(normalizedQuery))
      .sort((left, right) => compareResearchEntries(left, right, sort));
  }, [availability, collection, query, sort]);
  const groups = useMemo(() => {
    const grouped = new Map<string, ResearchEntry[]>();
    for (const entry of records) {
      const group = grouped.get(entry.collection) || [];
      group.push(entry);
      grouped.set(entry.collection, group);
    }
    return [...grouped.entries()].sort(([left], [right]) => researchCollectionRank(left) - researchCollectionRank(right));
  }, [records]);
  const sortDescription: Record<ResearchSort, string> = {
    recommended: "recommended subject order",
    newest: "newest first within each subject",
    oldest: "oldest first within each subject",
    title: "title A–Z within each subject",
    author: "author A–Z within each subject",
  };

  return <article className="tc-research-page">
    <div className="tc-research-hero">
      <div>
        <span className="tc-eyebrow">Ternary research library</span>
        <h1>Read the work behind the stack.</h1>
        <p className="tc-lede">A practical reading room for ternary computing: history, logic, devices, architectures, theory, security, and applications. Download the papers held locally, or follow the retained source links for papers that were not available to the collector.</p>
      </div>
      <aside className="tc-research-summary" aria-label="Research catalog summary">
        <div><strong>{researchCatalog.localCount}</strong><span>local PDFs</span></div>
        <div><strong>{researchCatalog.externalCount}</strong><span>linked-only</span></div>
        <div><strong>{researchCatalog.records.length}</strong><span>catalog entries</span></div>
      </aside>
    </div>
    <section className="tc-research-collections" aria-labelledby="research-collections-heading">
      <div className="tc-research-section-heading">
        <div><span className="tc-eyebrow">Organized reading paths</span><h2 id="research-collections-heading">Browse by subject</h2><p>Start with the area closest to your question. The recommended order moves from foundations through hardware and emerging systems to applications.</p></div>
        <span className="tc-research-section-count">{collections.length} collections</span>
      </div>
      <div className="tc-research-collection-grid">
        <button type="button" className={`tc-research-collection-button ${collection === "all" ? "is-active" : ""}`} aria-pressed={collection === "all"} onClick={() => setCollection("all")}>
          <span className="tc-eyebrow">Complete library</span><strong>All subjects</strong><span>{researchCatalog.records.length} papers · grouped for browsing</span>
        </button>
        {collectionStats.map((item) => <button type="button" key={item.collection} className={`tc-research-collection-button ${collection === item.collection ? "is-active" : ""}`} aria-pressed={collection === item.collection} onClick={() => setCollection(item.collection)}>
          <span className="tc-eyebrow">Subject collection</span><strong>{researchCollectionLabel(item.collection)}</strong><span>{item.total} papers · {item.local} local PDFs</span>
        </button>)}
      </div>
    </section>
    <section className="tc-research-toolbar" aria-label="Filter research">
      <label>Search the catalog<input value={query} onChange={(event) => setQuery(event.target.value)} placeholder="title, author, venue, DOI…" /></label>
      <label>Availability<select value={availability} onChange={(event) => setAvailability(event.target.value)}><option value="all">All entries</option><option value="local">Local PDF</option><option value="external">Linked-only</option></select></label>
      <label>Sort results<select value={sort} onChange={(event) => setSort(event.target.value as ResearchSort)}><option value="recommended">Recommended path</option><option value="newest">Newest first</option><option value="oldest">Oldest first</option><option value="title">Title A–Z</option><option value="author">Author A–Z</option></select></label>
    </section>
    <p className="tc-research-result-count" aria-live="polite">Showing {records.length} of {researchCatalog.records.length} catalog entries · {sortDescription[sort]}</p>
    {groups.map(([groupCollection, groupRecords]) => <section className="tc-research-group" key={groupCollection} aria-labelledby={researchCollectionId(groupCollection)}>
      <header className="tc-research-group-heading"><div><span className="tc-eyebrow">Subject collection</span><h2 id={researchCollectionId(groupCollection)}>{researchCollectionLabel(groupCollection)}</h2><p>{researchCollectionDescription(groupCollection)}</p></div><span className="tc-research-section-count">{groupRecords.length} {groupRecords.length === 1 ? "paper" : "papers"}</span></header>
      <div className="tc-research-list">{groupRecords.map((entry) => <ResearchCard key={entry.id} entry={entry} />)}</div>
    </section>)}
    {records.length === 0 ? <div className="tc-empty">No research entries match those filters.</div> : null}
  </article>;
}

function ResearchCard({ entry }: { entry: ResearchEntry }) {
  const primaryUrl = entry.status === "local" && entry.fileUrl ? entry.fileUrl : entry.sourceUrl || entry.landingUrl;
  return <article className="tc-research-card">
    <div className="tc-research-card-heading">
      <div><span className="tc-eyebrow">{researchCollectionLabel(entry.collection)}</span><span className={`tc-research-status ${entry.status}`}>{entry.status === "local" ? "Local PDF" : "Link-only"}</span></div>
      <span className="tc-research-kind">{entry.kind || "research"}</span>
    </div>
    <h2>{entry.title}</h2>
    <p className="tc-research-meta">{[entry.authors, entry.year, entry.venue].filter(Boolean).join(" · ") || "Metadata not recorded"}</p>
    {entry.identifier ? <p className="tc-research-identifier">{entry.identifier}</p> : null}
    {entry.notes ? <p className="tc-research-note">{entry.notes}</p> : null}
    <div className="tc-research-actions">
      {primaryUrl ? <a className="tc-button" href={primaryUrl} target={entry.status === "local" ? undefined : "_blank"} rel={entry.status === "local" ? undefined : "noreferrer"} download={entry.status === "local" ? true : undefined}>{entry.status === "local" ? "Download PDF" : "Open source"}</a> : null}
      {entry.status === "local" && entry.sourceUrl ? <a className="tc-research-secondary-link" href={entry.sourceUrl} target="_blank" rel="noreferrer">Source PDF</a> : null}
      {entry.landingUrl && entry.landingUrl !== entry.sourceUrl ? <a className="tc-research-secondary-link" href={entry.landingUrl} target="_blank" rel="noreferrer">Landing page</a> : null}
    </div>
  </article>;
}

export default function PublicApp() {
  const [snapshot, setSnapshot] = useState<PublicSnapshot | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [route, setRoute] = useState<Route>(() => parseRoute());

  useEffect(() => {
    const handlePopState = () => setRoute(parseRoute());
    window.addEventListener("popstate", handlePopState);
    fetch("/api/public/v1/snapshot.json", { headers: { Accept: "application/json" } })
      .then(async (response) => { if (!response.ok) throw new Error(`snapshot request failed (${response.status})`); return response.json() as Promise<PublicSnapshot>; })
      .then((data) => { if (data?.schema_version !== "treatcode.public.snapshot.v1" || !data.snapshot?.commit || PUBLIC_RESOURCES.some((resource) => !Array.isArray(data[resource]))) throw new Error("complete public snapshot schema is not available"); setSnapshot(data); })
      .catch((error: Error) => setLoadError(error.message));
    return () => window.removeEventListener("popstate", handlePopState);
  }, []);

  const navigate = (href: string) => {
    const target = href.startsWith("/") ? href : `/${href}`;
    window.history.pushState({}, "", target);
    setRoute(parseRoute());
    window.scrollTo?.({ top: 0, behavior: "smooth" });
  };
  const search = (query: string, mode: SearchMode) => navigate(`/search?q=${encodeURIComponent(query)}&mode=${mode}`);

  return <div className="tc-app-root"><div className="tc-shell"><Header route={route} onNavigate={navigate} /><main className="tc-main">
    {loadError ? <div className="tc-error" role="status" aria-live="polite">The repository-backed public snapshot could not be loaded. {loadError}</div> : null}
    {!snapshot ? <div className="tc-empty" role="status" aria-live="polite">Loading the repository-backed public snapshot…</div> : null}
    {snapshot && route.page === "home" ? <HomePage snapshot={snapshot} onNavigate={navigate} onSearch={search} route={route} /> : null}
    {snapshot && route.page === "stack" ? <StackPage snapshot={snapshot} route={route} onNavigate={navigate} onSearch={search} /> : null}
    {snapshot && route.page === "learn" ? <MarkdownLearnPage snapshot={snapshot} onNavigate={navigate} route={route} onSearch={search} /> : null}
    {route.page === "research" ? <ResearchPage /> : null}
    {snapshot && route.page === "search" ? <SearchPage snapshot={snapshot} route={route} onNavigate={navigate} onSearch={search} /> : null}
    {snapshot && route.page === "resource" && route.recordId ? (publicRecord(snapshot, route.recordId) ? <RecordDetailPage record={publicRecord(snapshot, route.recordId)!} snapshot={snapshot} onNavigate={navigate} /> : <div className="tc-error">The requested public record was not found.</div>) : null}
    {snapshot && route.page === "resource" && !route.recordId ? <ResourceCollectionPage snapshot={snapshot} route={route} onNavigate={navigate} /> : null}
    {snapshot && route.page === "evidence" ? <EvidencePage snapshot={snapshot} route={route} onNavigate={navigate} /> : null}
  </main><footer className="tc-footer">{snapshot ? <SnapshotBadge snapshot={snapshot} /> : <span>Repository-backed snapshot unavailable</span>}Read-only public route · <a href="/api/public/v1/openapi.json">OpenAPI v1</a> · <a href="/api/public/v1/snapshot.json">Static snapshot</a></footer></div></div>;
}
