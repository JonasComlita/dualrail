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
  const scores = record(latest.scores);
  const lunaScore = typeof scores.luna_max === "number" ? scores.luna_max : record(scores.luna_max).percent;
  const solScore = typeof scores.sol_high === "number" ? scores.sol_high : record(scores.sol_high).percent;
  const interval = record(latest.paired_confidence_interval);
  const signTest = record(latest.exact_sign_test);
  const matrix = Array.isArray(latest.task_matrix) ? latest.task_matrix : [];
  const blockers = Array.isArray(catalog.blockers) ? catalog.blockers as string[] : [];

  return <section className="intelligence-panel intelligence-v31" data-testid="intelligence-v31" aria-labelledby="intelligence-v31-title">
    <div className="intelligence-section-heading">
      <div><span className="intelligence-eyebrow">Repository benchmark · v3.1</span><h2 id="intelligence-v31-title">Harder held-out model comparison</h2></div>
      <span className="intelligence-badge pending">development · not official</span>
    </div>
    <p className="intelligence-muted">The exposed scalar tasks are diagnostics only. V3.1 uses disposable pilots, non-subject calibration, and a separately frozen 100-task Trit-centered repository holdout.</p>
    <div className="intelligence-v31-target" data-testid="intelligence-v31-target">
      <span><b>Target band</b>{scoreBand.minimum ?? 60}–{scoreBand.maximum ?? 75}% per model</span>
      <span><b>Target separation</b>{lead.minimum ?? 1}–{lead.maximum ?? 4} task Sol lead</span>
      <span><b>Anti-overfit rule</b>No exact 69/67 fitting or post-unblinding retune</span>
    </div>
    <div className="intelligence-v31-phases" aria-label="Intelligence v3.1 phases">
      {Object.entries(phases).map(([name, raw]) => {
        const phase = record(raw);
        return <article key={name} className={`intelligence-v31-phase phase-${name}`}><span>{phaseLabel(name)}</span><strong>{String(phase.status || "pending").replace(/_/g, " ")}</strong><small>{phase.task_count ?? 0}/{phase.required ?? phase.task_count ?? 0} tasks{phase.executable_packages !== undefined ? ` · ${phase.executable_packages} executable` : ""}</small></article>;
      })}
    </div>
    <div className="intelligence-v31-detail-grid">
      <article>
        <h3>Latest development comparison</h3>
        <p className="intelligence-muted">Luna max {lunaScore ?? "—"}% · Sol high {solScore ?? "—"}% · {String(latest.result || "no completed comparison").replace(/_/g, " ")}. This exposed pilot cannot satisfy the v3.1 replication target.</p>
        <div className="intelligence-v31-statistics" aria-label="Paired comparison statistics">
          <span><b>95% paired CI</b>{interval.low !== undefined ? `${interval.low} to ${interval.high} points` : "awaiting frozen holdout"}</span>
          <span><b>Exact sign test</b>{signTest.two_sided_p !== undefined ? `p=${signTest.two_sided_p}` : "awaiting frozen holdout"}</span>
        </div>
        <h4>Task pass matrix</h4>
        {matrix.length ? <div className="intelligence-table-wrap intelligence-v31-matrix"><table className="intelligence-table"><thead><tr><th>Task</th><th>Luna max</th><th>Sol high</th></tr></thead><tbody>{matrix.map((item: RecordValue) => <tr key={item.task_id}><td><code>{item.task_id}</code></td><td>{item.luna_passed === null ? "excluded" : item.luna_passed ? "pass" : "fail"}</td><td>{item.sol_passed === null ? "excluded" : item.sol_passed ? "pass" : "fail"}</td></tr>)}</tbody></table></div> : <p className="intelligence-empty">No task matrix is available.</p>}
      </article>
      <article>
        <h3>Publication blockers</h3>
        <ul className="intelligence-v31-blockers">{blockers.map((blocker) => <li key={blocker}>{blocker}</li>)}</ul>
      </article>
    </div>
    <div className="intelligence-v31-links"><a href="/api/intelligence/v3.1/catalog">Catalog API</a><a href="/api/intelligence/v3.1/protocol">Protocol</a><a href="/api/intelligence/v3.1/comparisons/latest">Latest comparison</a></div>
  </section>;
}
