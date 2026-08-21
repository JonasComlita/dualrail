import { FormEvent, ReactNode, useEffect, useMemo, useState } from "react";
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
  LEARNING_PAGES,
  parseMarkdown,
  type Block,
  type LearningInteractive,
  type LearningPage,
} from "./learningContent";

type Route = { page: "home" | "stack" | "learn" | "search"; stackSlug?: string; query: string; mode: SearchMode; focus?: string; learningPathId?: string; learningTopicId?: string };

const FALLBACK_SNAPSHOT: PublicSnapshot = {
  schema_version: "treatcode.public.snapshot.v1",
  snapshot: {
    id: "tc:snapshot:unavailable",
    repository: "https://github.com/JonasComlita/dualrail",
    commit: "unknown",
    generated_at: "1970-01-01T00:00:00Z",
    source: "static route fallback",
  },
  projects: [],
  stack_nodes: [
    { id: "tc:layer:representation", entity_type: "stack_node", ordinal: 1, slug: "representation", name: "Representation", description: "Trit representation and hardware realization.", depends_on: [], source_refs: [], evidence_refs: [] },
    { id: "tc:layer:isa", entity_type: "stack_node", ordinal: 2, slug: "isa", name: "ISA and ABI", description: "Instruction, privilege, trap, and ABI contracts.", depends_on: ["tc:layer:representation"], source_refs: [], evidence_refs: [] },
    { id: "tc:layer:user", entity_type: "stack_node", ordinal: 20, slug: "user", name: "User surface", description: "User-facing applications and learning surfaces.", depends_on: ["tc:layer:isa"], source_refs: [], evidence_refs: [] },
  ],
  components: [], capabilities: [], contracts: [], decisions: [], sources: [], symbols: [], tests: [], benchmarks: [], runs: [], releases: [], gaps: [], relations: [],
  statistics: {},
};

function parseRoute(): Route {
  const path = window.location.pathname.replace(/\/index\.html$/, "").replace(/\/$/, "") || "/";
  const parts = path.split("/").filter(Boolean);
  const params = new URLSearchParams(window.location.search);
  const page = parts[0] === "stack" ? "stack" : parts[0] === "learn" ? "learn" : parts[0] === "search" ? "search" : "home";
  return {
    page,
    stackSlug: page === "stack" ? parts[1] : undefined,
    query: params.get("q") || "",
    mode: (params.get("mode") as SearchMode) || "semantic",
    focus: params.get("focus") || undefined,
    learningPathId: params.get("path") || undefined,
    learningTopicId: params.get("topic") || undefined,
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

function stackHref(record: PublicRecord): string {
  if (record.entity_type === "stack_node") return `/stack/${String(record.slug || idSuffix(record.id))}`;
  return `/stack?focus=${encodeURIComponent(record.id)}`;
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

function RecordLink({ record, onNavigate, children }: { record: PublicRecord; onNavigate: (href: string) => void; children?: ReactNode }) {
  const href = stackHref(record);
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
  const nav = (href: string, label: string, active: boolean) => <a href={href} aria-current={active ? "page" : undefined} onClick={(event) => { event.preventDefault(); onNavigate(href); }}>{label}</a>;
  return <header className="tc-topbar">
    <a className="tc-brand" href="/" onClick={(event) => { event.preventDefault(); onNavigate("/"); }}>Treat<span className="tc-brand-mark">Code</span></a>
    <nav className="tc-nav" aria-label="Primary navigation">
      {nav("/", "Overview", route.page === "home")}
      {nav("/stack", "Stack Explorer", route.page === "stack")}
      {nav("/learn", "Learn", route.page === "learn")}
      {nav("/practice", "Practice", false)}
      {nav("/arena", "Implementation Arena", false)}
      <a href="/intelligence">Intelligence Benchmark</a>
      <a href="/api/public/v1/openapi.json">API</a>
    </nav>
  </header>;
}

function EntityCollection({ title, records, snapshot, onNavigate, empty = "No records are attached to this view yet." }: { title: string; records: PublicRecord[]; snapshot: PublicSnapshot; onNavigate: (href: string) => void; empty?: string }) {
  return <section className="tc-panel">
    <h2>{title} <span className="tc-muted">({records.length})</span></h2>
    {records.length === 0 ? <div className="tc-empty">{empty}</div> : <ul>{records.slice(0, 12).map((record) => <li key={record.id}><RecordLink record={record} onNavigate={onNavigate} /><span className="tc-citation">{record.entity_type || "entity"} · {snapshot.snapshot.commit}</span></li>)}</ul>}
  </section>;
}

function HomePage({ snapshot, onNavigate, onSearch, route }: { snapshot: PublicSnapshot; onNavigate: (href: string) => void; onSearch: (query: string, mode: SearchMode) => void; route: Route }) {
  const nodes = [...snapshot.stack_nodes].sort((a, b) => Number(a.ordinal || 0) - Number(b.ordinal || 0));
  const featured = nodes.filter((node) => [0, 1, 2, 5, 9, 13, 20].includes(Number(node.ordinal))).slice(0, 7);
  return <>
    <div className="tc-hero-row">
      <div><span className="tc-eyebrow">Public platform knowledge</span><h1>Understand the stack. Follow the evidence.</h1><p className="tc-lede">A static-first map of Trit from silicon to user surfaces. Every layer stays connected to the source, contract, test, benchmark, decision, release, and gap that qualify its claims.</p><SearchBox initialQuery={route.query} initialMode={route.mode} onSearch={onSearch} /></div>
      <aside className="tc-hero-panel"><span className="tc-eyebrow">Current snapshot</span><strong>{snapshot.snapshot.commit === "unknown" ? "Static fallback" : snapshot.snapshot.commit.slice(0, 12)}</strong><small>{snapshot.snapshot.repository.replace("https://github.com/", "")}</small><SnapshotBadge snapshot={snapshot} /></aside>
    </div>
    <div className="tc-metrics" aria-label="Snapshot metrics">
      <div className="tc-metric"><strong>{count(snapshot, "stack_nodes")}</strong><span>stack phases</span></div><div className="tc-metric"><strong>{count(snapshot, "capabilities")}</strong><span>capabilities</span></div><div className="tc-metric"><strong>{count(snapshot, "contracts")}</strong><span>contracts</span></div><div className="tc-metric"><strong>{count(snapshot, "sources")}</strong><span>source records</span></div>
    </div>
    <div className="tc-section-heading"><h2>Silicon → user dependency trail</h2><a href="/stack" onClick={(event) => { event.preventDefault(); onNavigate("/stack"); }}>Open full explorer →</a></div>
    <div className="tc-stack-trail" aria-label="Stack phases">{featured.map((node) => <RecordLink key={node.id} record={node} onNavigate={onNavigate}><b>PHASE {String(node.ordinal).padStart(2, "0")}</b><span>{displayName(node)}</span></RecordLink>)}</div>
    <div className="tc-section-heading"><h2>What the public graph makes reachable</h2></div>
    <div className="tc-card-grid"><div className="tc-card"><h3>Dependency views</h3><p>Move from an ordered stack phase to the contracts and capabilities it enables, with dependency edges kept visible.</p></div><div className="tc-card"><h3>Evidence views</h3><p>Open exact source paths, symbol spans, tests, benchmark definitions, decisions, and releases from one read-only surface.</p></div><div className="tc-card"><h3>Known gaps</h3><p>Open, planned, missing, and partial evidence remains labeled so navigation never turns a roadmap item into a production claim.</p></div></div>
    <div className="tc-section-heading"><h2>Release boundary</h2></div>
    <div className="tc-card"><h3>{displayName(snapshot.releases[0] || { id: "tc:release:none", name: "No release record" })}</h3><p>{String(snapshot.releases[0]?.description || "The snapshot is generated from the repository registries and test manifest.")}</p><SnapshotBadge snapshot={snapshot} /></div>
  </>;
}

function relatedRecords(snapshot: PublicSnapshot, ids: unknown): PublicRecord[] {
  const lookup = new Map<string, PublicRecord>();
  for (const resource of ["stack_nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps"] as const) for (const record of snapshot[resource]) lookup.set(record.id, record);
  return (Array.isArray(ids) ? ids : []).map((id) => lookup.get(String(id))).filter((record): record is PublicRecord => Boolean(record));
}

function SourcePanel({ title, refs }: { title: string; refs: SourceRef[] }) {
  const safeRefs = (Array.isArray(refs) ? refs : []).filter(Boolean);
  return <section className="tc-panel"><h2>{title}</h2>{safeRefs.length ? <ul>{safeRefs.map((ref, index) => <li key={`${ref.path || ref.artifact_hash || "evidence"}-${index}`}><SourceCitation ref={ref} />{ref.role ? <span className="tc-citation">{ref.role}</span> : null}</li>)}</ul> : <div className="tc-empty">No provenance recorded for this relation.</div>}</section>;
}

function StackDetail({ node, snapshot, onNavigate }: { node: PublicRecord; snapshot: PublicSnapshot; onNavigate: (href: string) => void }) {
  const dependencies = relatedRecords(snapshot, node.depends_on);
  const capabilities = relatedRecords(snapshot, node.capability_ids);
  const contracts = relatedRecords(snapshot, node.contract_ids);
  const tests = relatedRecords(snapshot, node.test_ids);
  const benchmarks = relatedRecords(snapshot, node.benchmark_ids);
  const gaps = relatedRecords(snapshot, node.gap_ids);
  const releases = relatedRecords(snapshot, node.release_ids);
  return <article className="tc-detail">
    <div className="tc-detail-header"><span className="tc-eyebrow">Phase {String(node.ordinal).padStart(2, "0")}</span><h1>{displayName(node)}</h1><p className="tc-lede">{String(node.description || "No description recorded.")}</p><span className="tc-detail-id">{node.id} · source id {String(node.source_id || "not recorded")}</span></div>
    <div className="tc-section-heading"><h2>Dependencies</h2></div>
    {dependencies.length ? <div className="tc-stack-trail">{dependencies.map((record) => <RecordLink key={record.id} record={record} onNavigate={onNavigate}><b>REQUIRES</b><span>{displayName(record)}</span></RecordLink>)}</div> : <div className="tc-empty">This is the root of the recorded stack.</div>}
    <div className="tc-detail-columns">
      <EntityCollection title="Capabilities" records={capabilities} snapshot={snapshot} onNavigate={onNavigate} />
      <EntityCollection title="Contracts" records={contracts} snapshot={snapshot} onNavigate={onNavigate} />
      <EntityCollection title="Tests" records={tests} snapshot={snapshot} onNavigate={onNavigate} />
      <EntityCollection title="Benchmarks" records={benchmarks} snapshot={snapshot} onNavigate={onNavigate} empty="No benchmark is attached to this phase." />
      <EntityCollection title="Known gaps" records={gaps} snapshot={snapshot} onNavigate={onNavigate} empty="No known gap is attached to this phase." />
      <EntityCollection title="Releases" records={releases} snapshot={snapshot} onNavigate={onNavigate} empty="No release boundary is attached to this phase." />
      <SourcePanel title="Source" refs={(node.source_refs as SourceRef[]) || []} />
      <SourcePanel title="Evidence" refs={(node.evidence_refs as SourceRef[]) || []} />
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

function publicLearningText(text: string): string {
  return text.replace(/`([^`]+)`/g, "$1").replace(/\*\*([^*]+)\*\*/g, "$1");
}

function PublicLearningContent({ content }: { content: string }) {
  return (
    <div className="tc-learning-content">
      {parseMarkdown(content).map((block: Block, index) => {
        if (block.type === "p") return <p key={index}>{publicLearningText(block.content)}</p>;
        if (block.type === "h2") return <h3 key={index}>{publicLearningText(block.content)}</h3>;
        if (block.type === "h3") return <h4 key={index}>{publicLearningText(block.content)}</h4>;
        if (block.type === "h4") return <h5 key={index}>{publicLearningText(block.content)}</h5>;
        if (block.type === "code") return <pre key={index}><code>{block.content}</code></pre>;
        if (block.type === "ul") return <ul key={index}>{block.items?.map((item) => <li key={item}>{publicLearningText(item)}</li>)}</ul>;
        if (block.type === "ol") return <ol key={index}>{block.items?.map((item) => <li key={item}>{publicLearningText(item)}</li>)}</ol>;
        return (
          <div className="tc-learning-table" key={index}>
            <table>
              <thead><tr>{block.headers?.map((header) => <th key={header}>{publicLearningText(header)}</th>)}</tr></thead>
              <tbody>{block.rows?.map((row, rowIndex) => <tr key={rowIndex}>{row.map((cell, cellIndex) => <td key={`${rowIndex}-${cellIndex}`}>{publicLearningText(cell)}</td>)}</tr>)}</tbody>
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
        <button type="button" className="tc-secondary" disabled={selected === null} onClick={() => setFeedback(selected === module.answer ? `Correct. ${module.explanation}` : "Not yet. Re-read this page and try again.")}>Check answer</button>
        <p aria-live="polite" className="tc-learning-feedback">{feedback}</p>
      </section>
    );
  }

  const validate = () => {
    const missing = module.expectedIncludes.filter((fragment) => !code.includes(fragment));
    setFeedback(missing.length ? `Add the expected TCL shape: ${missing.join(", ")}.` : `Example shape verified. ${module.explanation}`);
  };
  return (
    <section className="tc-learning-check" aria-labelledby="public-learning-check-title">
      <span className="tc-eyebrow">Interactive code module</span>
      <h2 id="public-learning-check-title">{module.title}</h2>
      <label htmlFor="public-tcl-practice">Edit the example, then validate it in the browser.</label>
      <textarea id="public-tcl-practice" aria-label="TCL practice code" value={code} onChange={(event) => setCode(event.target.value)} spellCheck={false} />
      <div className="tc-learning-actions"><button type="button" className="tc-primary" onClick={validate}>Validate example</button><a className="tc-secondary" href="/practice">Open challenges</a></div>
      <p aria-live="polite" className="tc-learning-feedback">{feedback}</p>
    </section>
  );
}

function learningSourceHref(snapshot: PublicSnapshot, repositoryPath: string): string {
  const repository = snapshot.snapshot.repository || "https://github.com/JonasComlita/dualrail";
  const commit = snapshot.snapshot.commit && snapshot.snapshot.commit !== "unknown" ? snapshot.snapshot.commit : "main";
  return `${repository}/blob/${commit}/${repositoryPath}`;
}

function PublicLearningProvenance({ page, snapshot }: { page: LearningPage; snapshot: PublicSnapshot }) {
  const references = (title: string, items: LearningPage["sources"]) => (
    <section className="tc-panel">
      <h2>{title}</h2>
      <ul>
        {items.map((item) => <li key={item.path}><a href={learningSourceHref(snapshot, item.path)} target="_blank" rel="noreferrer">{item.label}</a><span className="tc-citation">{item.path}</span></li>)}
      </ul>
    </section>
  );
  return <div className="tc-detail-columns">{references("Production source", page.sources)}{references("Validation evidence", page.evidence)}</div>;
}

function MarkdownLearnPage({ snapshot, onNavigate, route, onSearch }: { snapshot: PublicSnapshot; onNavigate: (href: string) => void; route: Route; onSearch: (query: string, mode: SearchMode) => void }) {
  const selectedPath = LEARNING_CATALOG.paths.find((path) => path.id === route.learningPathId) || LEARNING_CATALOG.paths[0];
  const pages = selectedPath.page_ids.map((id) => LEARNING_PAGES.find((page) => page.id === id)).filter((page): page is LearningPage => Boolean(page));
  const selectedPage = pages.find((page) => page.id === route.learningTopicId) || pages[0];
  const learnHref = (pathId: string, topicId?: string) => `/learn?path=${encodeURIComponent(pathId)}${topicId ? `&topic=${encodeURIComponent(topicId)}` : ""}`;
  return <>
    <div className="tc-hero-row"><div><span className="tc-eyebrow">Evidence-linked learning</span><h1>Learn from the boundary outward.</h1><p className="tc-lede">The published learning pages are Markdown-backed, ordered by prerequisite, and connected to the source and test evidence that make each claim concrete.</p></div><div><SearchBox initialQuery={route.query} initialMode={route.mode} onSearch={onSearch} /><SnapshotBadge snapshot={snapshot} /></div></div>
    <div className="tc-section-heading"><h2>Choose a path</h2><span className="tc-muted">{selectedPath.title}</span></div>
    <div className="tc-card-grid">{LEARNING_CATALOG.paths.map((path) => <article className="tc-card" key={path.id}><span className="tc-eyebrow">{path.title}</span><h3>{path.audience}</h3><p>{path.description}</p><p style={{ marginTop: 12 }}><a href={learnHref(path.id)} onClick={(event) => { event.preventDefault(); onNavigate(learnHref(path.id)); }}>{path.page_ids.length} ordered lessons</a></p></article>)}</div>
    <div className="tc-layout tc-learning-layout"><aside className="tc-sidebar" aria-label="Learning pages"><h2>{selectedPath.title}</h2>{pages.map((page) => <a key={page.id} href={learnHref(selectedPath.id, page.id)} aria-current={selectedPage?.id === page.id ? "page" : undefined} onClick={(event) => { event.preventDefault(); onNavigate(learnHref(selectedPath.id, page.id)); }}><b>{page.module}</b><span>{page.title}</span></a>)}</aside>
      {selectedPage ? <article className="tc-detail"><header className="tc-detail-header"><span className="tc-eyebrow">{selectedPage.module} · {selectedPage.level}</span><h2>{selectedPage.title}</h2><p className="tc-lede">{selectedPage.summary}</p><span className="tc-detail-id">Prerequisites: {selectedPage.prerequisites.length ? selectedPage.prerequisites.join(", ") : "none"}</span></header><PublicLearningContent content={selectedPage.content} /><PublicLearningCheck module={selectedPage.interactive} /><PublicLearningProvenance page={selectedPage} snapshot={snapshot} /></article> : <div className="tc-empty">No learning page is available.</div>}
    </div>
  </>;
}

function LegacyLearnPage({ snapshot, onNavigate, route, onSearch }: { snapshot: PublicSnapshot; onNavigate: (href: string) => void; route: Route; onSearch: (query: string, mode: SearchMode) => void }) {
  const topics = [
    ["Representation", "Begin with balanced trits, dual-rail storage, invalid sentinels, and the hardware contract."],
    ["ISA and VM", "Follow instructions, registers, privilege, traps, architectural state, and memory into execution."],
    ["Compiler boundary", "Connect TCL, IR, code generation, calling convention, and object/link boundaries."],
    ["Kernel to user", "See how syscalls, process handoff, apps, and release images turn the stack into a product."],
  ];
  const nodes = [...snapshot.stack_nodes].sort((a, b) => Number(a.ordinal || 0) - Number(b.ordinal || 0));
  return <><div className="tc-hero-row"><div><span className="tc-eyebrow">Evidence-linked learning</span><h1>Learn from the boundary outward.</h1><p className="tc-lede">The learning route explains concepts by pointing back to the layer, contract, source, and test that make each concept concrete.</p></div><div><SearchBox initialQuery={route.query} initialMode={route.mode} onSearch={onSearch} /><SnapshotBadge snapshot={snapshot} /></div></div><div className="tc-card-grid">{topics.map(([title, description], index) => { const node = nodes[index * Math.max(1, Math.floor(nodes.length / topics.length))] || nodes[index]; return <div className="tc-card" key={title}><span className="tc-eyebrow">Lesson {String(index + 1).padStart(2, "0")}</span><h3>{title}</h3><p>{description}</p>{node ? <p style={{ marginTop: 12 }}><RecordLink record={node} onNavigate={onNavigate}>Open {displayName(node)} →</RecordLink></p> : null}</div>; })}</div><div className="tc-section-heading"><h2>Read by concern</h2></div><div className="tc-detail-columns"><EntityCollection title="Contracts to understand" records={snapshot.contracts.slice(0, 6)} snapshot={snapshot} onNavigate={onNavigate} /><EntityCollection title="Decisions to keep current" records={snapshot.decisions.slice(0, 6)} snapshot={snapshot} onNavigate={onNavigate} /><EntityCollection title="Tests to run" records={snapshot.tests.slice(0, 6)} snapshot={snapshot} onNavigate={onNavigate} /><EntityCollection title="Open gaps" records={snapshot.gaps.slice(0, 6)} snapshot={snapshot} onNavigate={onNavigate} /></div></>;
}

function SearchPage({ snapshot, route, onNavigate, onSearch }: { snapshot: PublicSnapshot; route: Route; onNavigate: (href: string) => void; onSearch: (query: string, mode: SearchMode) => void }) {
  const results = useMemo(() => searchSnapshot(snapshot, route.query, route.mode), [snapshot, route.query, route.mode]);
  return <><div className="tc-hero-row"><div><span className="tc-eyebrow">Provenance search</span><h1>Find the exact boundary.</h1><p className="tc-lede">Search IDs and paths exactly, resolve symbols, inspect relationships, or discover related concepts. Every match carries its source provenance.</p></div><div><SearchBox initialQuery={route.query} initialMode={route.mode} onSearch={onSearch} /><SnapshotBadge snapshot={snapshot} /></div></div><div className="tc-section-heading"><h2>{route.query ? `Results for “${route.query}”` : "Search the public graph"}</h2><span className="tc-muted">{results.length} matches · {route.mode}</span></div>{results.length ? <div className="tc-card-grid">{results.map((result) => <article className="tc-card" key={`${result.entity_id}-${result.match_type}`}><span className="tc-eyebrow">{result.entity_type} · score {result.score}</span><h3><a href={stackHref({ id: result.entity_id, entity_type: result.entity_type, name: result.name })} onClick={(event) => { event.preventDefault(); onNavigate(stackHref({ id: result.entity_id, entity_type: result.entity_type, name: result.name })); }}>{result.name}</a></h3><p>{result.matched_fields.join(", ")} match · {result.match_type} search</p>{result.provenance.slice(0, 2).map((ref, index) => <SourceCitation key={`${ref.path}-${index}`} ref={ref} />)}</article>)}</div> : <div className="tc-empty">{route.query ? "No authoritative records matched this query. Try a symbol name, exact path, or relationship phrase." : "Enter a query to search the snapshot."}</div>}</>;
}

export default function PublicApp() {
  const [snapshot, setSnapshot] = useState<PublicSnapshot>(FALLBACK_SNAPSHOT);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [route, setRoute] = useState<Route>(() => parseRoute());

  useEffect(() => {
    const handlePopState = () => setRoute(parseRoute());
    window.addEventListener("popstate", handlePopState);
    fetch("/api/public/v1/snapshot.json", { headers: { Accept: "application/json" } })
      .then(async (response) => { if (!response.ok) throw new Error(`snapshot request failed (${response.status})`); return response.json() as Promise<PublicSnapshot>; })
      .then((data) => { if (data?.schema_version !== "treatcode.public.snapshot.v1") throw new Error("snapshot schema version is not supported"); setSnapshot(data); })
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
    {loadError ? <div className="tc-error" role="status" aria-live="polite">Live snapshot unavailable; showing the static route fallback. {loadError}</div> : null}
    {route.page === "home" ? <HomePage snapshot={snapshot} onNavigate={navigate} onSearch={search} route={route} /> : null}
    {route.page === "stack" ? <StackPage snapshot={snapshot} route={route} onNavigate={navigate} onSearch={search} /> : null}
     {route.page === "learn" ? <MarkdownLearnPage snapshot={snapshot} onNavigate={navigate} route={route} onSearch={search} /> : null}
    {route.page === "search" ? <SearchPage snapshot={snapshot} route={route} onNavigate={navigate} onSearch={search} /> : null}
  </main><footer className="tc-footer"><SnapshotBadge snapshot={snapshot} />Read-only public route · <a href="/api/public/v1/openapi.json">OpenAPI v1</a> · <a href="/api/public/v1/snapshot.json">Static snapshot</a></footer></div></div>;
}
