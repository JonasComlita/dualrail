import { useEffect, useState } from "react";

type RecordValue = Record<string, any>;

function record(value: unknown): RecordValue {
  return value && typeof value === "object" ? value as RecordValue : {};
}

function phaseLabel(name: string): string {
  return name.charAt(0).toUpperCase() + name.slice(1);
}

export function IntelligenceV31Panel() {
  const [catalog, setCatalog] = useState<RecordValue | null>(null);
  const [comparison, setComparison] = useState<RecordValue | null>(null);
  const [error, setError] = useState("");

  useEffect(() => {
    let cancelled = false;
    Promise.all([
      fetch("/api/intelligence/v3.1/catalog", { headers: { Accept: "application/json" } }).then(async (response) => {
        if (!response.ok) throw new Error(`v3.1 catalog returned HTTP ${response.status}`);
        const payload = record(await response.json());
        return record(record(payload.data).catalog || payload.catalog);
      }),
      fetch("/api/intelligence/v3.1/comparisons/latest", { headers: { Accept: "application/json" } }).then(async (response) => {
        if (!response.ok) throw new Error(`v3.1 comparison returned HTTP ${response.status}`);
        const payload = record(await response.json());
        return record(record(payload.data).comparison || payload.comparison);
      }),
    ]).then(([nextCatalog, nextComparison]) => {
      if (cancelled) return;
      setCatalog(nextCatalog);
      setComparison(nextComparison);
    }).catch((reason) => {
      if (!cancelled) setError(reason instanceof Error ? reason.message : "Unable to load Intelligence v3.1 evidence.");
    });
    return () => { cancelled = true; };
  }, []);

  if (error) return <section className="intelligence-panel intelligence-v31" data-testid="intelligence-v31"><div className="intelligence-error" role="alert">Intelligence v3.1 evidence is unavailable: {error}</div></section>;
  if (!catalog) return <section className="intelligence-panel intelligence-v31" data-testid="intelligence-v31"><div className="intelligence-status" role="status">Loading Intelligence v3.1 readiness…</div></section>;

  const phases = record(catalog.phases);
  const target = record(catalog.replication_target);
  const scoreBand = record(target.score_band);
  const lead = record(target.sol_lead_tasks);
  const latest = record(record(comparison).comparison);
  const latestPhase = String(record(comparison).phase || "diagnostic");
  const official = catalog.official === true && record(comparison).official === true && record(comparison).phase === "official";
  const scores = record(latest.scores);
  const lunaScore = typeof scores.luna_max === "number" ? scores.luna_max : record(scores.luna_max).percent;
  const solScore = typeof scores.sol_high === "number" ? scores.sol_high : record(scores.sol_high).percent;
  const interval = record(latest.paired_confidence_interval);
  const signTest = record(latest.exact_sign_test);
  const discrimination = record(latest.discrimination);
  const dimensionPolicy = record(catalog.score_dimensions);
  const evaluationGoals = record(catalog.evaluation_goals);
  const tracks = Array.isArray(catalog.tracks) ? catalog.tracks as RecordValue[] : [];
  const dimensions = record(latest.dimensions);
  const byTrackDimensions = record(dimensions.by_track);
  const byTrackLuna = record(byTrackDimensions.luna_max);
  const byTrackSol = record(byTrackDimensions.sol_high);
  const observedTrackIds = [...new Set([...Object.keys(byTrackLuna), ...Object.keys(byTrackSol)])];
  const dimensionDefinitions = Array.isArray(dimensionPolicy.dimensions) ? dimensionPolicy.dimensions as RecordValue[] : [];
  const dimensionKeys = ["correctness", "robustness", "latency", "resource_use", "tool_execution", "discussion_quality"];
  const matrix = Array.isArray(latest.task_matrix) ? latest.task_matrix : [];
  const blockers = Array.isArray(catalog.blockers) ? catalog.blockers as string[] : [];

  return <section className="intelligence-panel intelligence-v31" data-testid="intelligence-v31" aria-labelledby="intelligence-v31-title">
    <div className="intelligence-section-heading">
      <div><span className="intelligence-eyebrow">Multi-track intelligence portfolio · v3.1</span><h2 id="intelligence-v31-title">Coding, agency, reasoning, and generalization</h2></div>
      <span className={`intelligence-badge ${official ? "official" : "pending"}`}>{official ? "official · frozen holdout" : "development · not official"}</span>
    </div>
    <p className="intelligence-muted">The legacy exposed scalar tasks are diagnostics only. V3.1 combines fresh coding, repository repair, terminal agency, expert reasoning, frontier mathematics, and abstract generalization. A Luna/Sol separation is useful evidence when observed, but it is not the only evaluation goal.</p>
    {Array.isArray(evaluationGoals.primary) && evaluationGoals.primary.length ? <p className="intelligence-muted" data-testid="intelligence-v31-goals">{evaluationGoals.primary.join(" · ")}</p> : null}
    <div className="intelligence-v31-target" data-testid="intelligence-v31-target">
      <span><b>Target band</b>{scoreBand.minimum ?? 60}–{scoreBand.maximum ?? 75}% per model</span>
      <span><b>Optional separation experiment</b>{lead.minimum ?? 1}–{lead.maximum ?? 4} task Sol lead when a holdout supports it</span>
      <span><b>Anti-overfit rule</b>No exact 69/67 fitting or post-unblinding retune</span>
    </div>
    <div className="intelligence-v31-track-section" data-testid="intelligence-v31-tracks">
      <h3>Benchmark portfolio</h3>
      <p className="intelligence-muted">Tracks borrow proven task shapes without treating any external score as a local result. Freshness, hidden coverage, and the execution mode are recorded per track.</p>
      {tracks.length ? <div className="intelligence-v31-track-grid">{tracks.map((track) => <article className="intelligence-v31-track" key={String(track.id)}><div className="intelligence-v31-track-heading"><strong>{String(track.label || track.id)}</strong><span>{String(track.execution_mode || "scalar")}</span></div><small>{String(track.task_shape || "")}</small><em>{Array.isArray(track.inspiration) ? track.inspiration.join(" · ") : ""}</em></article>)}</div> : <p className="intelligence-empty">Track manifest is unavailable.</p>}
    </div>
    <div className="intelligence-v31-phases" aria-label="Intelligence v3.1 phases: diagnostic, pilot, calibration, frozen, official">
      {Object.entries(phases).map(([name, raw]) => {
        const phase = record(raw);
        return <article key={name} className={`intelligence-v31-phase phase-${name}`}><span>{phaseLabel(name)}</span><strong>{String(phase.status || "pending").replace(/_/g, " ")}</strong><small>{phase.task_count ?? 0}/{phase.required ?? phase.task_count ?? 0} tasks{phase.shard_count !== undefined ? ` · ${phase.shard_count}/${phase.required_shards} atomic shards` : ""}{phase.executable_packages !== undefined ? ` · ${phase.executable_packages} executable` : ""}</small></article>;
      })}
    </div>
    <div className="intelligence-v31-detail-grid">
      <article>
        <h3>{official ? "Official frozen comparison" : "Latest development comparison"}</h3>
        <p className="intelligence-muted">Luna max {lunaScore ?? "—"}% · Sol high {solScore ?? "—"}% · {String(latest.result || "no completed comparison").replace(/_/g, " ")}.{official ? " The suite was scored once without post-unblinding retuning." : latestPhase === "development" ? " This disposable paired run is scored from sealed grades and cannot be an official holdout." : latestPhase === "pilot" ? " This exposed pilot cannot satisfy the v3.1 replication target." : " This diagnostic is not a v3.1 holdout result."}</p>
        <div className="intelligence-v31-statistics" aria-label="Paired comparison statistics">
          <span><b>95% paired CI</b>{interval.low !== undefined ? `${interval.low} to ${interval.high} points` : "awaiting frozen holdout"}</span>
          <span><b>Exact sign test</b>{signTest.two_sided_p !== undefined ? `p=${signTest.two_sided_p}` : "awaiting frozen holdout"}</span>
          <span><b>Correctness signal</b>{discrimination.status ? String(discrimination.status).replace(/_/g, " ") : "awaiting paired tasks"}</span>
        </div>
        {discrimination.note ? <p className="intelligence-muted" data-testid="intelligence-v31-discrimination-note">{discrimination.note}</p> : null}
        <h4>Independent score dimensions</h4>
        <p className="intelligence-muted">{String(dimensionPolicy.headline || "correctness")} is the headline task result; latency, resource use, tool execution, robustness, and discussion quality are never collapsed into an IQ-like number. Missing telemetry remains unscored.</p>
        {dimensions.luna_max || dimensions.sol_high ? <><div className="intelligence-v31-dimensions" data-testid="intelligence-v31-dimensions"><div className="intelligence-v31-dimension-row intelligence-v31-dimension-header"><span>Dimension</span><span>Luna max</span><span>Sol high</span></div>{dimensionKeys.map((key) => { const definition = dimensionDefinitions.find((item) => item.id === key); const luna = record(dimensions.luna_max)[key]; const sol = record(dimensions.sol_high)[key]; return <div className="intelligence-v31-dimension-row" key={key}><span>{String(definition?.label || key.replace(/_/g, " "))}</span><span>{typeof luna === "number" ? `${luna}%` : "—"}</span><span>{typeof sol === "number" ? `${sol}%` : "—"}</span></div>; })}</div>{Array.isArray(dimensions.notes) && dimensions.notes.length ? <p className="intelligence-muted">{dimensions.notes.join(" · ")}</p> : null}</> : <p className="intelligence-empty" data-testid="intelligence-v31-dimensions-empty">No independent dimension telemetry is available for this comparison.</p>}
        {observedTrackIds.length ? <><h5>Observed track slices</h5><div className="intelligence-v31-dimensions intelligence-v31-track-scores" data-testid="intelligence-v31-track-scores"><div className="intelligence-v31-dimension-row intelligence-v31-dimension-header"><span>Track</span><span>Luna correctness</span><span>Sol correctness</span></div>{observedTrackIds.map((trackId) => { const luna = record(byTrackLuna[trackId]).correctness; const sol = record(byTrackSol[trackId]).correctness; return <div className="intelligence-v31-dimension-row" key={trackId}><span>{String(tracks.find((track) => track.id === trackId)?.label || trackId)}</span><span>{typeof luna === "number" ? `${luna}%` : "—"}</span><span>{typeof sol === "number" ? `${sol}%` : "—"}</span></div>; })}</div></> : null}
        <h4>Task pass matrix</h4>
        {matrix.length ? <div className="intelligence-table-wrap intelligence-v31-matrix"><table className="intelligence-table"><thead><tr><th>Task</th><th>Luna max</th><th>Sol high</th></tr></thead><tbody>{matrix.map((item: RecordValue) => <tr key={item.task_id}><td><code>{item.task_id}</code></td><td>{item.luna_passed === null ? "excluded" : item.luna_passed ? "pass" : "fail"}</td><td>{item.sol_passed === null ? "excluded" : item.sol_passed ? "pass" : "fail"}</td></tr>)}</tbody></table></div> : <p className="intelligence-empty">No task matrix is available.</p>}
      </article>
      <article>
        <h3>Open benchmark work</h3>
        {blockers.length ? <ul className="intelligence-v31-blockers">{blockers.map((blocker) => <li key={blocker}>{blocker}</li>)}</ul> : <p className="intelligence-muted">No open benchmark work is recorded.</p>}
      </article>
    </div>
    <div className="intelligence-v31-links"><a href="/api/intelligence/v3.1/catalog">Catalog API</a><a href="/api/intelligence/v3.1/tracks">Track manifest</a><a href="/api/intelligence/v3.1/protocol">Protocol</a><a href="/api/intelligence/v3.1/comparisons/latest">Latest comparison</a></div>
  </section>;
}
