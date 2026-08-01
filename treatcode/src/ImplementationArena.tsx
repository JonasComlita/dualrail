import { useEffect, useMemo, useState } from "react";

type Metric = { unit: string; samples: number[] };
type BenchmarkRun = {
  id: string;
  workload_id: string;
  representation_id?: string;
  profile_id: string;
  runner_image: string;
  input_hash: string;
  context?: Record<string, unknown>;
  correctness: {
    equivalent: boolean;
    fixture_id: string;
    reference_output_hash: string;
    observed_output_hash: string;
  };
  metrics: Record<string, Metric>;
};
type Workload = {
  id: string;
  title: string;
  kind: string;
  objective: string;
  representations?: Array<{ id: string; label: string; description: string }>;
  layout?: { name: string; dimensions: number[]; precision: string; vector_width: number; alignment_bytes: number };
};
type Manifest = {
  schema: string;
  protocol: {
    id: string;
    warmups: number;
    repetitions: number;
    sample_unit: string;
    variance: { maximum_coefficient_of_variATION?: number; maximum_coefficient_of_variation: number; repeat_envelope_ratio: number };
    regression_thresholds: Record<string, number>;
  };
  target_profiles: Array<{ id: string; execution: string; authority: string; estimate: boolean; runner_image: string }>;
  workloads: Workload[];
  arena: { correctness_evidence: string; benchmark_evidence: string };
};
type BenchmarkPayload = { manifest: Manifest; reference: { runs: BenchmarkRun[]; source: Record<string, string>; environment: Record<string, unknown> } };

const EMPTY_MANIFEST: Manifest = {
  schema: "trit.benchmark_manifest.v1",
  protocol: { id: "p10.reproducible.v1", warmups: 2, repetitions: 7, sample_unit: "nanoseconds", variance: { maximum_coefficient_of_variATION: 0.1, maximum_coefficient_of_variation: 0.1, repeat_envelope_ratio: 0.15 }, regression_thresholds: {} },
  target_profiles: [],
  workloads: [],
  arena: { correctness_evidence: "", benchmark_evidence: "" },
};

const DISPLAY_METRIC_IDS = [
  "wall_time_ns",
  "vm_cycles",
  "instructions",
  "dispatches",
  "memory_read_bytes",
  "memory_write_bytes",
  "code_size_bytes",
  "max_register_pressure",
  "allocations",
  "throughput_ops_per_second",
];

function distribution(samples: number[]) {
  const ordered = [...samples].sort((a, b) => a - b);
  const mean = samples.reduce((sum, value) => sum + value, 0) / Math.max(samples.length, 1);
  const variance = samples.reduce((sum, value) => sum + (value - mean) ** 2, 0) / Math.max(samples.length, 1);
  const p95Index = Math.max(0, (ordered.length - 1) * 0.95);
  const p95Lower = Math.floor(p95Index);
  const p95Upper = Math.ceil(p95Index);
  const p95 = p95Lower === p95Upper ? ordered[p95Lower] ?? 0 : (ordered[p95Lower] ?? 0) + ((ordered[p95Upper] ?? 0) - (ordered[p95Lower] ?? 0)) * (p95Index - p95Lower);
  return {
    min: ordered[0] ?? 0,
    p50: ordered[Math.floor(ordered.length / 2)] ?? 0,
    p95,
    max: ordered[ordered.length - 1] ?? 0,
    mean,
    cv: mean ? Math.sqrt(variance) / mean : 0,
  };
}

function compact(value: number, digits = 2) {
  return value.toLocaleString(undefined, { maximumFractionDigits: digits });
}

function profileLabel(profile: Manifest["target_profiles"][number]) {
  return `${profile.id} · ${profile.authority}${profile.estimate ? " estimate" : " measured"}`;
}

export default function ImplementationArena() {
  const [payload, setPayload] = useState<BenchmarkPayload | null>(null);
  const [error, setError] = useState("");
  const [workloadId, setWorkloadId] = useState("");
  const [profileId, setProfileId] = useState("binary-host");
  const [representationId, setRepresentationId] = useState("");
  const [candidateCode, setCandidateCode] = useState("// Attach an implementation to a P10 pilot.\n// Correctness evidence must pass before metrics can be compared.\n");
  const [candidateAttached, setCandidateAttached] = useState(false);

  useEffect(() => {
    fetch("/api/benchmarks/p10")
      .then((response) => response.ok ? response.json() : Promise.reject(new Error(`Benchmark API returned ${response.status}`)))
      .then((value: BenchmarkPayload) => {
        setPayload(value);
        const firstWorkload = value.manifest.workloads[0];
        setWorkloadId(firstWorkload?.id || "");
        setRepresentationId(firstWorkload?.representations?.[0]?.id || "");
      })
      .catch((reason: Error) => setError(reason.message));
  }, []);

  const manifest = payload?.manifest || EMPTY_MANIFEST;
  const workload = manifest.workloads.find((item) => item.id === workloadId) || manifest.workloads[0];
  const representations = workload?.representations || [];
  const runs = payload?.reference.runs || [];
  const availableProfiles = useMemo(
    () => manifest.target_profiles.filter((profile) => runs.some((run) => run.workload_id === workload?.id && run.profile_id === profile.id && (!representationId || run.representation_id === representationId))),
    [manifest.target_profiles, runs, workload?.id, representationId],
  );
  const selectedRun = runs.find((run) => run.workload_id === workload?.id && run.profile_id === profileId && (!run.representation_id || run.representation_id === representationId)) || runs.find((run) => run.workload_id === workload?.id);
  const selectedProfile = manifest.target_profiles.find((profile) => profile.id === selectedRun?.profile_id) || manifest.target_profiles[0];
  const correctnessPassed = selectedRun?.correctness.equivalent === true;

  function selectWorkload(nextId: string) {
    const next = manifest.workloads.find((item) => item.id === nextId);
    setWorkloadId(nextId);
    setRepresentationId(next?.representations?.[0]?.id || "");
    setProfileId("binary-host");
    setCandidateAttached(false);
  }

  return (
    <div className="p10-arena" data-testid="implementation-arena">
      <header className="p10-arena-header">
        <a className="p10-brand" href="/">Treat<span>Code</span></a>
        <nav aria-label="Arena navigation"><a href="/stack">Stack Explorer</a><a href="/learn">Learn</a><a href="/practice">Practice</a></nav>
        <a className="p10-api-link" href="/api/benchmarks/p10">Evidence API</a>
      </header>

      <main className="p10-arena-main">
        <div className="p10-hero">
          <div>
            <span className="p10-eyebrow">P10 · optimization and benchmark lab</span>
            <h1>Implementation Arena</h1>
            <p>Compare an implementation against an exact reference only after correctness equivalence passes. Every result keeps its target profile, workload, limits, distributions, and provenance visible.</p>
          </div>
          <div className="p10-protocol-card" data-testid="protocol-summary">
            <span className="p10-eyebrow">Protocol</span>
            <strong>{manifest.protocol.id}</strong>
            <span>{manifest.protocol.warmups} warmups · {manifest.protocol.repetitions} repetitions · p50 / p95 / CV</span>
          </div>
        </div>

        {error ? <div className="p10-error" role="alert">Unable to load benchmark evidence: {error}</div> : null}

        <section className="p10-grid p10-controls" aria-label="Benchmark controls">
          <label>Workload<select value={workload?.id || ""} onChange={(event) => selectWorkload(event.target.value)}>{manifest.workloads.map((item) => <option key={item.id} value={item.id}>{item.title}</option>)}</select></label>
          {representations.length ? <label>Representation<select value={representationId} onChange={(event) => setRepresentationId(event.target.value)}>{representations.map((item) => <option key={item.id} value={item.id}>{item.label}</option>)}</select></label> : null}
          <label>Target profile<select value={profileId} onChange={(event) => setProfileId(event.target.value)}>{manifest.target_profiles.map((profile) => <option key={profile.id} value={profile.id}>{profileLabel(profile)}</option>)}</select></label>
        </section>

        <section className="p10-gate-row" aria-label="Correctness gate">
          <div className={`p10-gate ${correctnessPassed ? "passed" : "blocked"}`} data-testid="correctness-evidence">
            <span className="p10-gate-icon">{correctnessPassed ? "✓" : "!"}</span>
            <div><strong>Correctness evidence: {correctnessPassed ? "equivalent" : "blocked"}</strong><p>{correctnessPassed ? `Fixture ${selectedRun?.correctness.fixture_id} matches the reference output.` : "Select a reference run with an equivalent fixture before comparing performance."}</p></div>
          </div>
          <div className="p10-gate-note">{manifest.arena.correctness_evidence}</div>
        </section>

        <section className="p10-panel" data-testid="benchmark-evidence">
          <div className="p10-section-heading"><div><span className="p10-eyebrow">Benchmark evidence</span><h2>{workload?.title || "Loading P10 workload…"}</h2></div><span className={`p10-badge ${selectedProfile?.estimate ? "estimate" : "measured"}`}>{selectedProfile ? profileLabel(selectedProfile) : "no profile"}</span></div>
          <p className="p10-muted">{workload?.objective}</p>
          {workload?.kind === "vector_matrix" && workload.layout ? <div className="p10-layout-summary"><span>Layout <b>{workload.layout.name}</b></span><span>Dimensions <b>{workload.layout.dimensions.join(" × ")}</b></span><span>Precision <b>{workload.layout.precision}</b></span><span>Vector width <b>{workload.layout.vector_width}</b></span><span>Alignment <b>{workload.layout.alignment_bytes} B</b></span></div> : null}
          {workload?.kind === "tritwise" ? <div className="p10-representation-note"><b>Representation-aware sign inversion:</b> {representations.find((item) => item.id === representationId)?.description || "Choose a representation to keep positional, lane/rail, and hardware costs separate."}</div> : null}
          <div className="p10-metric-table-wrap">
            <table className="p10-metric-table"><thead><tr><th>Metric</th><th>Unit</th><th>p50</th><th>p95</th><th>Mean</th><th>CV</th></tr></thead><tbody>
              {selectedRun && correctnessPassed ? DISPLAY_METRIC_IDS.filter((metricId) => selectedRun.metrics[metricId]).map((metricId) => { const metric = selectedRun.metrics[metricId]; const stats = distribution(metric.samples); return <tr key={metricId}><td><code>{metricId}</code></td><td>{metric.unit}</td><td>{compact(stats.p50)}</td><td>{compact(stats.p95)}</td><td>{compact(stats.mean)}</td><td>{(stats.cv * 100).toFixed(2)}%</td></tr>; }) : <tr><td colSpan={6} className="p10-empty">Performance metrics are withheld until correctness evidence passes.</td></tr>}
            </tbody></table>
          </div>
          <div className="p10-run-meta"><span>Runner image: <code>{selectedRun?.runner_image || "—"}</code></span><span>Input: <code>{selectedRun?.input_hash?.slice(0, 20) || "—"}…</code></span><span>Samples: {selectedRun ? selectedRun.metrics.wall_time_ns?.samples.length : 0}</span><span>Environment: {String(payload?.reference.environment.fingerprint || "—")}</span></div>
          <p className="p10-evidence-note">{manifest.arena.benchmark_evidence}</p>
        </section>

        <section className="p10-grid p10-bottom-grid">
          <div className="p10-panel">
            <div className="p10-section-heading"><div><span className="p10-eyebrow">Candidate implementation</span><h2>Attach a measured candidate</h2></div><span className={`p10-badge ${candidateAttached ? "measured" : "pending"}`}>{candidateAttached ? "attached" : "draft"}</span></div>
            <textarea aria-label="Candidate implementation" value={candidateCode} onChange={(event) => { setCandidateCode(event.target.value); setCandidateAttached(false); }} spellCheck={false} />
            <button type="button" className="p10-primary" onClick={() => setCandidateAttached(true)}>Attach candidate evidence</button>
            <p className="p10-muted">Attachment records code context; execution and immutable run evidence belong to the isolated runner. This surface never treats a draft as a performance result.</p>
          </div>
          <div className="p10-panel">
            <span className="p10-eyebrow">Target profiles</span>
            <h2>Keep targets distinct</h2>
            <div className="p10-profile-list">{availableProfiles.length ? availableProfiles.map((profile) => <button type="button" key={profile.id} className={profile.id === profileId ? "selected" : ""} onClick={() => setProfileId(profile.id)}><span>{profile.id}</span><small>{profile.execution}</small><b>{profile.estimate ? "proxy / estimate" : "measured"}</b></button>) : <p className="p10-empty">No run is available for this representation/profile pair.</p>}</div>
            <p className="p10-muted">A binary host, VM, GPU, FPGA, and native-ternary estimate cannot share one leaderboard or one interpretation.</p>
          </div>
        </section>
      </main>
      <footer className="p10-footer">P10 reference artifact · <a href="/api/benchmarks/p10">JSON evidence</a> · <a href="/">Return to TreatCode</a></footer>
    </div>
  );
}
