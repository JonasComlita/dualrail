import { useEffect, useRef, useState, type ReactNode } from "react";
import { SiteHeader } from "../SiteHeader";
import { CAPABILITIES, DEFAULT_LIMITS, PILOT_LABEL, type EpisodeView, type ModelConfig, type Publication, type RunEvent, type RunView, type ScoreRow } from "./contracts";
import { EXAMPLES, METHODOLOGY } from "./public";
import { ComparisonView, LearningCurves, type PairedComparison } from "./report-views";
import "../treatcode-theme.css";
import "./intelligence.css";

const API = "/api/ternary-intelligence/v1";
const TABS = [["", "Overview"], ["methodology", "Methodology"], ["examples", "Examples"], ["results", "Results"], ["operator", "Operator Lab"]];
const active = (run: RunView | null) => Boolean(run && ["queued", "running"].includes(run.state));
const percent = (n: number | null) => n === null ? "Unavailable" : `${(n * 100).toFixed(1)}%`;
const message = (e: unknown) => e instanceof Error ? e.message : "The request could not be completed.";
async function api<T>(route: string, options: { token?: string; body?: unknown; signal?: AbortSignal; requestKey?: string } = {}): Promise<T> {
  const response = await fetch(API + route, {
    method: options.body === undefined ? "GET" : "POST", signal: options.signal,
    headers: { Accept: "application/json", ...(options.token ? { Authorization: `Bearer ${options.token}` } : {}),
      ...(options.body === undefined ? {} : { "Content-Type": "application/json", "x-action-nonce": crypto.randomUUID() }),
      ...(options.requestKey ? { "idempotency-key": options.requestKey } : {}) },
    body: options.body === undefined ? undefined : JSON.stringify(options.body),
  });
  const data = await response.json().catch(() => null);
  if (!response.ok) throw new Error(data?.error?.reason || `Benchmark service returned HTTP ${response.status}.`);
  if (!data || !("data" in data)) throw new Error("The benchmark service returned an invalid response.");
  return data.data;
}
function usePublic<T>(route: string) {
  const [value, setValue] = useState<T | null>(null), [error, setError] = useState("");
  useEffect(() => { const controller = new AbortController();
    void api<T>(route, { signal: controller.signal }).then(setValue).catch(e => { if (!controller.signal.aborted) setError(message(e)); });
    return () => controller.abort();
  }, [route]);
  return { value, error };
}
function Notice({ children, error = false }: { children: ReactNode; error?: boolean }) {
  return <div className={`ti-notice${error ? " ti-error" : ""}`} role={error ? "alert" : "status"}>{children}</div>;
}
function Json({ value }: { value: unknown }) { return <pre className="ti-json">{JSON.stringify(value, null, 2)}</pre>; }
function Failures({ items }: { items: string[] }) { return items.length ? <ul className="ti-failures">{items.map((item, i) => <li key={i}>{item}</li>)}</ul> : <p className="ti-good">All checks passed.</p>; }
function Overview() {
  const { value, error } = usePublic<{ revision: string; familyCount: number; qualifiedFamilies: number; validation: string; frozen: boolean; latestPublication: Publication | null }>("/overview");
  return <>
    <section className="ti-hero"><p className="ti-eyebrow">Reasoning with three values</p><h1>Ternary Intelligence</h1>
      <p className="ti-lead">How well can a model reason, learn, and build when the rules are ternary?</p>
      <p>Explore explicit arithmetic and logic, unfamiliar machines, and practical engineering tasks. Each evaluation records what the model solved and the conditions it worked under.</p>
      <div className="ti-actions"><a className="ti-primary" href="/intelligence/examples">Try an example</a><a href="/intelligence/methodology">Read the methodology →</a></div>
    </section>
    {error && <Notice error>{error}</Notice>}
    <section className="ti-stats" aria-label="Pilot status">
      <div><strong>36</strong><span>Private task families</span></div><div><strong>6</strong><span>Capability areas</span></div>
      <div><strong>{value ? value.validation === "qualified" ? "Qualified" : "Pending" : "Loading"}</strong><span>Executable task validation</span></div>
      <div><strong>{value?.frozen ? "Frozen" : "Draft"}</strong><span>Calibration and release</span></div>
    </section>
    <h2>Six ways to test understanding</h2><div className="ti-grid">{CAPABILITIES.map((c, i) => <article className="ti-card" key={c.id}><span className="ti-index">0{i + 1}</span><h3>{c.name}</h3><p>{c.description}</p><p className="ti-muted">6 families · 3 provisional difficulty levels</p></article>)}</div>
    <section className="ti-panel"><h2>Latest comparison</h2>{value?.latestPublication ? <><p>{value.latestPublication.label} · {new Date(value.latestPublication.createdAt).toLocaleDateString()}</p><a href="/intelligence/results">Explore published results →</a></> : <p>No model comparison has been published. Results appear after task qualification, live calibration, a frozen protocol, and a completed evaluation.</p>}</section>
    <p className="ti-muted">Correctness determines the grade. Spending, speed, and tool use are reported separately. Public examples and Practice activity never enter model scores.</p>
  </>;
}
function Methodology() {
  return <><h1>Methodology</h1><p className="ti-lead">A reproducible pilot, with the rules and limits made explicit.</p><p>{METHODOLOGY.scope}</p>
    <section className="ti-panel"><h2>What “ternary” means here</h2><p>A trit has three possible values. Tasks specify whether they use balanced digits (−1, 0, +1), unsigned digits (0, 1, 2), or three-valued logic. Each task defines its numeric range, overflow, rounding, unknown-value behavior, and submission contract.</p><p>Knowing a familiar convention is insufficient when a task declares different rules. Checkers use exact arithmetic and executable reference solutions.</p></section>
    <h2>Three evaluation conditions</h2><div className="ti-grid">{METHODOLOGY.conditions.map(c => <article className="ti-card" key={c.id}><h3>{c.name}</h3><p>{c.description}</p></article>)}</div>
    <h2>Two experiments</h2><div className="ti-grid ti-two">{METHODOLOGY.experiments.map(e => <article className="ti-card" key={e.id}><h3>{e.name}</h3><p>{e.description}</p><strong>{e.episodesPerModel} episodes per model</strong><p>Calibration repeats every assignment three times in fresh contexts.</p></article>)}</div>
    <p>Model-only episodes submit files without executing them. Tool-assisted episodes can list, read, and replace allowlisted files, run public tests, and submit. Hidden grading happens after submission in both modes.</p>
    <section className="ti-panel"><h2>Correctness and uncertainty</h2><p>{METHODOLOGY.scoring}</p><p>{METHODOLOGY.uncertainty}</p><p>{METHODOLOGY.failures}</p></section>
    <h2>Comparable resource limits</h2><div className="ti-table-wrap"><table><caption>Default limits for each episode</caption><tbody>{Object.entries(DEFAULT_LIMITS).map(([key, n]) => <tr key={key}><th scope="row">{limitLabels[key]}</th><td>{n.toLocaleString()}</td></tr>)}</tbody></table></div>
    <p>Changing these limits creates a distinct protocol profile. An optional dollar cap can stop further requests; it never changes an answer’s correctness. Unknown cost is shown as unavailable.</p>
    <section className="ti-panel"><h2>Qualification, calibration, and freeze</h2><p>Each reference must pass; repair starters and designated incorrect solutions must fail behavioral checks. Small domains are exhaustive, with deterministic boundary and property samples for larger domains.</p><p>{METHODOLOGY.calibration}</p><p>Task packages, prompts, assignments, seeds, checkers, and budgets are hashed before evaluation. Changes require a new accepted revision.</p></section>
    <h2>Execution and access</h2><p>{METHODOLOGY.execution}</p><h3>Local harness profile</h3><p>{METHODOLOGY.harness}</p><p>{METHODOLOGY.privacy}</p>
    <h2>Technical references</h2><ul>{METHODOLOGY.references.map(r => <li key={r.url}><a href={r.url}>{r.title}</a></li>)}</ul>
  </>;
}
const limitLabels: Record<string, string> = { episodeMs: "Episode time (ms)", inputTokens: "Cumulative input tokens", outputTokens: "Output tokens, including reported reasoning", toolCalls: "Tool calls", publicTestCalls: "Public-test calls", commandMs: "Time per compiler or VM command (ms)", memoryMiB: "Worker memory (MiB)", processes: "Worker processes", outputBytes: "Captured output per command (bytes)" };
function ExampleCard({ example }: { example: typeof EXAMPLES[number] }) {
  const [input, setInput] = useState(example.source || example.input), [output, setOutput] = useState(""), [busy, setBusy] = useState(false), [error, setError] = useState("");
  const worker = useRef<Worker | null>(null);
  useEffect(() => { if (example.kind === "code") return;
    const instance = new Worker(new URL("./examples.worker.ts", import.meta.url), { type: "module" }); worker.current = instance;
    instance.onmessage = event => { setBusy(false); setError(event.data.error || ""); setOutput(event.data.error ? "" : JSON.stringify(event.data.result, null, 2)); };
    instance.onerror = () => { setBusy(false); setError("The example worker could not run. Reload this page to try again."); };
    return () => { instance.terminate(); worker.current = null; };
  }, [example.kind]);
  const download = () => { const url = URL.createObjectURL(new Blob([input], { type: "text/plain" })); const a = document.createElement("a"); a.href = url; a.download = `${example.id}.trit`; a.click(); setTimeout(() => URL.revokeObjectURL(url), 1000); };
  return <article className="ti-panel ti-example"><p className="ti-eyebrow">{CAPABILITIES.find(c => c.id === example.capability)?.name}</p><h2>{example.title}</h2><p>{example.instructions}</p>
    <label htmlFor={`example-${example.id}`}>{example.kind === "code" ? "Editable Trit source" : "Structured input (JSON)"}</label>
    <textarea id={`example-${example.id}`} className="ti-source" rows={example.kind === "code" ? 7 : 3} spellCheck={false} maxLength={example.kind === "code" ? 65536 : 2048} value={input} onChange={e => { setInput(e.target.value); setOutput(""); setError(""); }} />
    <div className="ti-actions">{example.kind === "code" ? <button onClick={download}>Download source</button> : <button disabled={busy} onClick={() => { setBusy(true); setError(""); worker.current?.postMessage({ requestId: crypto.randomUUID(), id: example.id, input }); }}>{busy ? "Running…" : "Run example"}</button>}<button className="ti-quiet" onClick={() => { setInput(example.source || example.input); setOutput(""); setError(""); }}>Reset</button></div>
    {error && <Notice error>{error}</Notice>}{output && <div role="status" aria-label="Example output"><pre className="ti-json">{output}</pre></div>}
    {example.recordedOutput && <div className="ti-recorded"><h3>Recorded reference output</h3><p>{example.input}</p><pre>{example.recordedOutput}</pre><p className="ti-muted">Editing the source does not rerun this recorded output.</p></div>}
    <details><summary>Worked solution</summary><p>{example.workedSolution}</p></details>
  </article>;
}
function Examples() { return <><h1>Try ternary reasoning</h1><p className="ti-lead">Six public examples, one for each capability.</p><p>Experiment freely. These examples are separate from the private pilot and do not contribute to comparison scores.</p>{EXAMPLES.map(example => <ExampleCard key={example.id} example={example} />)}</>; }
function ScoreTable({ rows }: { rows: ScoreRow[] }) {
  return <div className="ti-table-wrap"><table><caption>Family success rates with 95% bootstrap intervals</caption><thead><tr><th>Model configuration</th><th>Capability</th><th>Condition / tools</th><th>Coverage</th><th>Success rate</th></tr></thead><tbody>{rows.map((r, i) => <tr key={i}><th scope="row">{r.modelConfigId}</th><td>{r.capability === "all" ? "All capabilities" : CAPABILITIES.find(c => c.id === r.capability)?.name}</td><td>{r.condition} / {r.mode}</td><td>{r.evaluated} / {r.expected}</td><td><span>{percent(r.rate)}{r.interval ? ` (${percent(r.interval[0])}–${percent(r.interval[1])})` : ""}</span>{r.interval && r.rate !== null && <svg className="ti-interval" viewBox="0 0 100 12" aria-hidden="true"><path d="M0 6H100" stroke="var(--tc-practice-line)" /><path d={`M${r.interval[0] * 100} 6H${r.interval[1] * 100}`} stroke="currentColor" strokeWidth="2" /><circle cx={r.rate * 100} cy="6" r="3" fill="currentColor" /></svg>}</td></tr>)}</tbody></table></div>;
}
function PublishedDetail({ publication: p }: { publication: Publication }) {
  return <article className="ti-panel"><h2>{new Date(p.createdAt).toLocaleString()}</h2><p>{p.label} · Revision {p.revision}</p><p>Coverage: {p.coverage.evaluated} / {p.coverage.expected} episodes · {p.coverage.complete ? "Complete" : "Incomplete"}</p>
    <ScoreTable rows={p.scores} />
    <h3>Learning through feedback</h3><LearningCurves publication={p} />
    <h3>Resource observations</h3><p>API costs are estimates from the recorded pricing snapshot. They do not affect correctness.</p><div className="ti-table-wrap"><table><caption>Totals across observed episodes; generated-program time is separate</caption><thead><tr><th>Configuration</th><th>Input / output tokens</th><th>Model time</th><th>Program time</th><th>Estimated API cost</th></tr></thead><tbody>{p.models.map(m => {
      const obs = p.observations.filter(o => o.modelConfigId === m.id), sum = (key: "inputTokens" | "outputTokens" | "modelMs" | "programMs" | "costNanoUsd") => obs.reduce((s, o) => s + (o.resources[key] || 0), 0);
      return <tr key={m.id}><th scope="row">{m.id}</th><td>{sum("inputTokens").toLocaleString()} / {sum("outputTokens").toLocaleString()}</td><td>{(sum("modelMs") / 1000).toFixed(1)} s</td><td>{obs.every(o => o.resources.programMs === null) ? "Unavailable" : `${(sum("programMs") / 1000).toFixed(1)} s`}</td><td>{obs.some(o => o.resources.costNanoUsd === null) ? "Unavailable" : `$${(sum("costNanoUsd") / 1e9).toFixed(4)}`}</td></tr>;
    })}</tbody></table></div><details><summary>Configuration and reproducibility metadata</summary><Json value={{ models: p.models, protocol: p.protocol, providerReportedModels: Object.fromEntries(p.models.map(m => [m.id, [...new Set(p.observations.filter(o => o.modelConfigId === m.id).flatMap(o => o.providerModels || []))]])), environment: p.environment, protocolHash: p.protocolHash, profileHash: p.profileHash }} /></details>
  </article>;
}
function Results() {
  const { value, error } = usePublic<Publication[]>("/publications"), [left, setLeft] = useState(""), [right, setRight] = useState(""), [comparison, setComparison] = useState<PairedComparison | null>(null), [comparisonError, setComparisonError] = useState("");
  const choices = value?.flatMap(p => p.models.map(m => ({ key: `${p.id}/${m.id}`, publication: p.id, model: m.id, label: `${m.id} · ${p.revision} · ${new Date(p.createdAt).toLocaleDateString()}` }))) || [];
  return <><h1>Published results</h1><p className="ti-lead">Compare observed success under matching evaluation conditions.</p><p>Correctness, learning, and resources are reported separately. Intervals describe uncertainty across task families in this exploratory pilot.</p>{error && <Notice error>{error}</Notice>}{!value && !error && <Notice>Loading publications…</Notice>}{value?.length === 0 && <section className="ti-empty"><h2>No published comparisons yet</h2><p>Genuine results will appear after live calibration, protocol freeze, and completed model evaluations.</p><a href="/intelligence/methodology">Understand the evaluation process →</a></section>}
    {choices.length > 1 && <section className="ti-panel"><h2>Paired comparison</h2><div className="ti-grid ti-two">{[["left", left, setLeft], ["right", right, setRight]].map(([id, selected, set]) => <label key={String(id)}>Configuration {id === "left" ? "A" : "B"}<select value={selected as string} onChange={e => { (set as (s: string) => void)(e.target.value); setComparison(null); }}>{[<option key="" value="">Select a publication and model</option>, ...choices.map(c => <option key={c.key} value={c.key}>{c.label}</option>)]}</select></label>)}</div><button disabled={!left || !right || left === right} onClick={async () => { setComparisonError(""); setComparison(null); const a = choices.find(c => c.key === left)!, b = choices.find(c => c.key === right)!; try { setComparison(await api(`/comparisons?${new URLSearchParams({ a: a.publication, b: b.publication, modelA: a.model, modelB: b.model })}`)); } catch (e) { setComparisonError(message(e)); } }}>Compare matched families</button>{comparisonError && <Notice error>{comparisonError}</Notice>}{comparison !== null && <ComparisonView comparison={comparison} />}</section>}
    {value?.map(p => <PublishedDetail key={p.id} publication={p} />)}
  </>;
}

interface Configuration { models: ModelConfig[]; calibrationModelIds: string[]; calibrationTiers: Record<string, string> }
interface Readiness { revision: string; worker: { ready?: boolean; errors: string[] }; qualificationFailures: string[]; models: Array<{ id: string; model: string; provider: string; credentialConfigured: boolean; verification: unknown; ready: boolean; errors: string[] }>; calibration: { failures: string[]; baselineRunId:string|null; controlledRunId:string|null; ambiguity:{template:unknown;review:unknown} }; freeze: unknown; spendingPolicy: string }
type RunDetail = RunView & { episodes: EpisodeView[] };
interface Preview { episodes: number; failures: string[]; maximumCostUsd: number | null; protocol: unknown; models: unknown }
function Operator() {
  const token = localStorage.getItem("treatcode.auth.token") || "";
  const [ready, setReady] = useState<Readiness | null>(null), [configuration, setConfiguration] = useState<Configuration | null>(null), [configText, setConfigText] = useState("");
  const [runs, setRuns] = useState<RunDetail[]>([]), [run, setRun] = useState<RunDetail | null>(null), [events, setEvents] = useState<RunEvent[]>([]), [evidence, setEvidence] = useState<unknown>(null);
  const [selected, setSelected] = useState<string[]>([]), [purpose, setPurpose] = useState("calibration"), [experiment, setExperiment] = useState("default"), [ceiling, setCeiling] = useState("");
  const [limits, setLimits] = useState({ ...DEFAULT_LIMITS }), [preview, setPreview] = useState<Preview | null>(null), [error, setError] = useState(""), [status, setStatus] = useState(""), [busy, setBusy] = useState(false);
  const [reviewText,setReviewText]=useState("");
  const requestKey = useRef(crypto.randomUUID()), cursor = useRef(0);
  const selectedRunId = useRef(new URLSearchParams(location.search).get("run"));
  const refresh = async (signal?: AbortSignal) => {
    const [r, c, all] = await Promise.all([api<Readiness>("/operator/readiness", { token, signal }), api<Configuration>("/operator/configuration", { token, signal }), api<RunDetail[]>("/operator/runs", { token, signal })]);
    setReady(r); setConfiguration(c); setConfigText(JSON.stringify(c, null, 2)); setRuns(all);
    setReviewText(JSON.stringify(r.calibration.ambiguity.review||r.calibration.ambiguity.template,null,2));
    if (selectedRunId.current) setRun(all.find(item => item.id === selectedRunId.current) || null);
  };
  useEffect(() => { if (!token) return; const controller = new AbortController(); void refresh(controller.signal).catch(e => { if (!controller.signal.aborted) setError(message(e)); }); return () => controller.abort(); }, [token]);
  // Serial polling avoids overlapping provider/evidence refreshes. The selected ID is
  // in the URL so a browser refresh reconnects to the same durable evaluation.
  useEffect(() => { if (!run) return; const controller = new AbortController(); let timer: ReturnType<typeof setTimeout>;
    const poll = async () => {
      try {
        const detail = await api<RunDetail>(`/operator/runs/${run.id}`, { token, signal: controller.signal });
        setRun(detail); setRuns(old => old.map(r => r.id === detail.id ? detail : r));
        let more = true;
        while (more && !controller.signal.aborted) {
          const batch = await api<RunEvent[]>(`/operator/runs/${run.id}/events?after=${cursor.current}`, { token, signal: controller.signal });
          if (batch.length) { cursor.current = batch[batch.length - 1].sequence; setEvents(old => [...old, ...batch]); }
          more = batch.length === 500;
        }
        if (active(detail) && !controller.signal.aborted) timer = setTimeout(poll, 2000);
      } catch (e) { if (!controller.signal.aborted) { setError(message(e)); if (active(run)) timer = setTimeout(poll, 2000); } }
    };
    void poll(); return () => { controller.abort(); clearTimeout(timer); };
  }, [run?.id, token]);
  const chooseRun = (r: RunDetail) => { cursor.current = 0; setEvents([]); setEvidence(null); selectedRunId.current = r.id; history.replaceState(null, "", `/intelligence/operator?run=${encodeURIComponent(r.id)}`); setRun(r); };
  const action = async (fn: () => Promise<void>) => { setBusy(true); setError(""); setStatus(""); try { await fn(); } catch (e) { setError(message(e)); } finally { setBusy(false); } };
  const input = () => ({ modelIds: selected, purpose, experiment, spendingCeilingUsd: ceiling === "" ? null : Number(ceiling), limits });
  const changed = () => { setPreview(null); requestKey.current = crypto.randomUUID(); };
  return <><h1>Operator Lab</h1><p className="ti-lead">Configure, evaluate, inspect, and publish.</p>
    {!token ? <Notice>Sign in with an account assigned the benchmark-operator role. <a href="/account">Open your account →</a></Notice> : null}
    {error && <Notice error>{error}</Notice>}{status && <Notice>{status}</Notice>}
    {token && !ready && !error && <Notice>Checking operator access and readiness…</Notice>}
    {ready && <>
      <section className="ti-panel"><div className="ti-section-heading"><h2>Readiness</h2><button disabled={busy} onClick={() => void action(() => refresh())}>Refresh</button></div><p>{ready.spendingPolicy}</p><div className="ti-grid ti-two"><div><h3>Compiler and worker</h3><Failures items={ready.worker.errors} /><h3>Task qualification</h3><Failures items={ready.qualificationFailures} /></div><div><h3>Provider configuration</h3>{ready.models.length ? ready.models.map(m => <p key={m.id}><strong>{m.id}</strong> · {m.provider} / {m.model}<br />{m.ready ? "Access configured · compatibility verified" : m.errors.join(" ")}</p>) : <p>No exact model configurations have been supplied.</p>}<h3>Live calibration</h3><Failures items={ready.calibration.failures} /></div></div>
        <div className="ti-actions"><button disabled={busy || runs.some(active)} onClick={() => void action(async () => { setStatus("Qualification is running. This may take several minutes."); await api("/operator/qualify", { token, body: {} }); await refresh(); setStatus("Qualification finished. Review the current validation checks."); })}>Qualify task corpus</button><button disabled={busy || Boolean(ready.freeze) || ready.calibration.failures.length > 0 || ready.qualificationFailures.length > 0} onClick={() => void action(async () => { await api("/operator/freeze", { token, body: {} }); await refresh(); setStatus("Pilot revision frozen."); })}>{ready.freeze ? "Revision frozen" : "Freeze accepted calibration"}</button></div>
      </section>
      <details className="ti-panel"><summary>Calibration report and ambiguity review</summary><Json value={ready.calibration} /><p>After reviewing current calibration evidence, mark each family ambiguous or unambiguous and record why. Unresolved ambiguity prevents freezing.</p><label htmlFor="ti-ambiguity">Family review JSON</label><textarea id="ti-ambiguity" className="ti-source" rows={14} value={reviewText} onChange={e=>setReviewText(e.target.value)} spellCheck={false}/><button disabled={busy||!ready.calibration.baselineRunId||!ready.calibration.controlledRunId} onClick={()=>void action(async()=>{await api("/operator/calibration-review",{token,body:JSON.parse(reviewText)});await refresh();setStatus("Calibration review saved.");})}>Save calibration review</button></details>
      <section className="ti-panel"><h2>Provider compatibility</h2><p>Verify each exact model configuration before starting an evaluation. Each check sends two short requests: one with tools and one without. Direct API requests are billable; local harness requests consume the signed-in account’s usage. This check contributes no intelligence score.</p>{ready.models.length ? ready.models.map(m => <div key={m.id}><h3>{m.id}</h3><Failures items={m.errors} /><button disabled={busy || runs.some(active) || !m.credentialConfigured} onClick={() => void action(async () => { setStatus(`Verifying ${m.id} with its provider…`); await api(`/operator/models/${encodeURIComponent(m.id)}/verify`, { token, body: {} }); await refresh(); setStatus("Provider verification finished. Review the recorded outcome."); })}>Verify {m.id} · 2 requests</button>{m.verification !== null && <details><summary>Verification outcome and usage</summary><Json value={m.verification} /></details>}</div>) : <p>Configure models below to begin.</p>}</section>
      <details className="ti-panel"><summary>Configure exact models and calibration preselection</summary><p>The preselected Astra, Sol, Terra, and Luna configurations use your local Codex sign-in. Direct API configurations use local server environment variables for keys. Never enter API key values here.</p><label htmlFor="ti-configuration">Model configuration JSON</label><textarea id="ti-configuration" className="ti-source" rows={16} value={configText} onChange={e => setConfigText(e.target.value)} spellCheck={false} /><p>Include every preselected configuration in calibration. The initial four-model lineup uses high reasoning effort. Declared capability tiers are provisional calibration priors, not grades.</p><button disabled={busy || runs.some(active)} onClick={() => void action(async () => { await api("/operator/configuration", { token, body: JSON.parse(configText) }); await refresh(); changed(); setSelected([]); setStatus("Configuration saved."); })}>Save configuration</button></details>
      <section className="ti-panel"><h2>New evaluation</h2><div className="ti-grid ti-two"><label>Purpose<select value={purpose} onChange={e => { setPurpose(e.target.value); changed(); }}><option value="calibration">Live calibration · 3 fresh repeats</option><option value="evaluation">Scored evaluation · frozen pilot</option></select></label><label>Experiment<select value={experiment} onChange={e => { setExperiment(e.target.value); changed(); }}><option value="default">Full pilot · 36 families</option><option value="controlled">Controlled · 12 families × 6 conditions / modes</option></select></label></div>
        <fieldset><legend>Exact model configurations</legend>{configuration?.models.length ? configuration.models.map(m => <label className="ti-checkbox" key={m.id}><input type="checkbox" checked={selected.includes(m.id)} onChange={e => { setSelected(old => e.target.checked ? [...old, m.id] : old.filter(id => id !== m.id)); changed(); }} />{m.id} · {m.provider} / {m.model}</label>) : <p>Configure models above to begin.</p>}</fieldset>
        <p><strong>{selected.length * (experiment === "default" ? 36 : 72) * (purpose === "calibration" ? 3 : 1)} episodes</strong> across {selected.length} selected configurations.</p>
        <label htmlFor="ti-ceiling">Optional spending ceiling (USD)</label><input id="ti-ceiling" type="number" min="0.01" step="0.01" value={ceiling} placeholder="No dollar cap" onChange={e => { setCeiling(e.target.value); changed(); }} /><p className="ti-muted">Leave blank to grade without a dollar ceiling. Cost does not contribute to correctness.</p>
        <details><summary>Time, token, and tool limits</summary><p>Changes create a distinct protocol profile.</p><div className="ti-grid ti-two">{Object.entries(limits).map(([key, n]) => <label key={key}>{limitLabels[key]}<input type="number" min={key === "memoryMiB" ? 8 : 1} max={DEFAULT_LIMITS[key as keyof typeof limits]} value={n} onChange={e => { setLimits(old => ({ ...old, [key]: Number(e.target.value) })); changed(); }} /></label>)}</div></details>
        <div className="ti-actions"><button disabled={busy || !selected.length} onClick={() => void action(async () => setPreview(await api<Preview>("/operator/preview", { token, body: input() })))}>Review preflight</button><button className="ti-primary" disabled={busy || !preview || preview.failures.length > 0} onClick={() => void action(async () => { const created = await api<RunDetail>("/operator/runs", { token, body: input(), requestKey: requestKey.current }); chooseRun(created); await refresh(); setPreview(null); requestKey.current = crypto.randomUUID(); setStatus("Run created. You can refresh this page to reconnect."); })}>Start reviewed run</button></div>
        {preview && <div><h3>Preflight · {preview.episodes} episodes</h3><Failures items={preview.failures} /><p>Theoretical maximum API cost: {preview.maximumCostUsd === null ? "Unavailable; no pricing snapshot" : `$${preview.maximumCostUsd.toFixed(2)}`}.</p><details><summary>Exact protocol and effective configuration</summary><Json value={preview} /></details></div>}
      </section>
      <section className="ti-panel"><h2>Durable runs</h2>{!runs.length ? <p>No evaluations have been started.</p> : <label>Inspect a run<select value={run?.id || ""} onChange={e => { const next = runs.find(r => r.id === e.target.value); if (next) chooseRun(next); }}><option value="">Select a run</option>{runs.map(r => <option key={r.id} value={r.id}>{r.createdAt} · {r.protocol.purpose} / {r.protocol.experiment} · {r.state} · {r.finished}/{r.total}</option>)}</select></label>}
        {run && <><h3>{run.protocol.purpose} / {run.protocol.experiment}</h3><p role="status">{run.state} · {run.finished} / {run.total} episodes · {run.provenance}</p><progress value={run.finished} max={run.total} aria-label="Completed episodes" /><p>Current task: {run.episodes.filter(e => e.id === run.activeEpisodeId).map(e => `${e.assignment.familyId} · ${e.modelConfigId} · ${e.assignment.condition} / ${e.assignment.mode}`).join("") || "None"}</p><p>Episodes graded: {run.episodes.filter(e => e.observation?.success !== null && e.observation?.success !== undefined).length} · Cancelled: {run.episodes.filter(e => e.state === "cancelled").length} · Interrupted or infrastructure errors: {run.episodes.filter(e => ["interrupted", "infrastructure_error"].includes(e.state)).length}</p><div className="ti-actions"><button disabled={busy || !active(run)} onClick={() => void action(async () => { setRun(await api<RunDetail>(`/operator/runs/${run.id}/cancel`, { token, body: {} })); await refresh(); })}>Cancel run</button><button disabled={busy || run.publicationFailures.length > 0} onClick={() => void action(async () => { await api(`/operator/runs/${run.id}/publish`, { token, body: {} }); await refresh(); setStatus("Immutable results published. They are now available on Results."); })}>Publish completed results</button></div><h3>Publication eligibility</h3><Failures items={run.publicationFailures} />
          <details><summary>Episode outcomes and resource usage</summary><div className="ti-table-wrap"><table><thead><tr><th>Family / configuration</th><th>Condition / mode / repeat</th><th>Outcome</th><th>Tokens in / out</th><th>Tools / public tests</th></tr></thead><tbody>{run.episodes.map(e => <tr key={e.id}><th scope="row">{e.assignment.familyId}<br />{e.modelConfigId}</th><td>{e.assignment.condition} / {e.assignment.mode} / {e.assignment.repeat + 1}</td><td>{e.state}<br />{e.observation?.category}</td><td>{e.observation ? `${e.observation.resources.inputTokens} / ${e.observation.resources.outputTokens}` : "Pending"}</td><td>{e.observation ? `${e.observation.resources.toolCalls} / ${e.observation.resources.publicTestCalls}` : "Pending"}</td></tr>)}</tbody></table></div></details>
          <h3>Private evidence</h3><p>Ordered provider, file, tool, and grader events. Artifact contents are available only to operators.</p><div className="ti-events">{events.map(event => <details key={event.sequence}><summary>#{event.sequence} · {event.type} · {event.episodeId || "run"}</summary><p>{event.at}</p><Json value={event.data} />{artifactHashes(event.data).map(h => <button key={h} disabled={busy} onClick={() => void action(async () => setEvidence(await api(`/operator/artifacts/${h}`, { token })))}>Read artifact {h.slice(0, 10)}…</button>)}</details>)}</div>{evidence !== null && <section aria-label="Selected private artifact"><div className="ti-section-heading"><h3>Artifact contents</h3><button onClick={() => setEvidence(null)}>Close artifact</button></div><Json value={evidence} /></section>}
        </>}
      </section>
    </>}
  </>;
}
function artifactHashes(data: unknown): string[] {
  const found = new Set<string>();
  const visit = (value: unknown) => { if (!value || typeof value !== "object") return; for (const [key, v] of Object.entries(value)) { if ((key === "artifactHash" || key === "evidenceHash") && typeof v === "string" && /^[a-f0-9]{64}$/.test(v)) found.add(v); else if (typeof v === "object") visit(v); } };
  visit(data); return [...found];
}
export default function TernaryIntelligenceApp() {
  const page = location.pathname.replace(/^\/intelligence\/?/, "").replace(/\/$/, "");
  return <div className="ti-shell"><a className="ti-skip" href="#ti-main">Skip to content</a><SiteHeader active="intelligence" /><nav className="ti-tabs" aria-label="Ternary Intelligence navigation">{TABS.map(([path, title]) => <a key={path} href={`/intelligence${path ? `/${path}` : ""}`} aria-current={page === path ? "page" : undefined}>{title}</a>)}</nav><main id="ti-main" className="ti-main"><p className="ti-pilot">{PILOT_LABEL}</p>{page === "" ? <Overview /> : page === "methodology" ? <Methodology /> : page === "examples" ? <Examples /> : page === "results" ? <Results /> : page === "operator" ? <Operator /> : <><h1>Page not found</h1><a href="/intelligence">Return to Ternary Intelligence</a></>}</main></div>;
}
