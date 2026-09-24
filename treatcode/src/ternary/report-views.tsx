import type { Condition, Publication, ToolMode } from "./contracts";

const pct = (n: number | null) => n === null ? "Unavailable" : `${(n * 100).toFixed(1)}%`;
const rounds = [0, 1, 3] as const;

export function LearningCurves({ publication }: { publication: Publication }) {
  const series = publication.models.flatMap(model => (["model-only", "tools"] as const).flatMap(mode => {
    const observations = publication.observations.filter(o => o.modelConfigId === model.id && o.condition === "learning" && o.mode === mode);
    if (!observations.length) return [];
    const rates = rounds.map(round => {
      const families = [...new Set(observations.map(o => o.familyId))];
      const values = families.flatMap(family => {
        const stages = observations.filter(o => o.familyId === family).flatMap(o => o.learning.filter(s => s.round === round && s.total > 0));
        return stages.length ? [stages.reduce((sum, s) => sum + s.passed / s.total, 0) / stages.length] : [];
      });
      return values.length ? values.reduce((sum, n) => sum + n, 0) / values.length : null;
    });
    return [{ name: `${model.id} / ${mode}`, rates }];
  }));
  if (!series.length) return <p>No learning episodes are included in this experiment. Use the controlled experiment to measure transfer through feedback.</p>;
  return <><p>Average fraction of unseen probes passed, weighting each task family equally. Probe sets differ at each stage.</p>
    <div className="ti-grid ti-two">{series.map(s => <figure className="ti-learning" key={s.name}>
      <figcaption>{s.name}</figcaption>
      <svg viewBox="0 0 360 210" role="img" aria-label={`${s.name}: ${s.rates.map((rate, i) => `${pct(rate)} after ${rounds[i]} feedback rounds`).join(", ")}`}>
        {[0, .5, 1].map(n => <g key={n}><path d={`M48 ${160 - n * 130}H332`} stroke="var(--tc-practice-line)" /><text x="40" y={164 - n * 130} textAnchor="end">{n * 100}%</text></g>)}
        {rounds.map((round, i) => <text key={round} x={60 + i * 130} y="186" textAnchor="middle">{round === 0 ? "Before" : `Round ${round}`}</text>)}
        {s.rates.slice(1).map((rate, i) => rate !== null && s.rates[i] !== null ? <path key={i} d={`M${60 + i * 130} ${160 - s.rates[i]! * 130}L${190 + i * 130} ${160 - rate * 130}`} stroke="currentColor" strokeWidth="2" fill="none" /> : null)}
        {s.rates.map((rate, i) => rate !== null && <circle key={i} cx={60 + i * 130} cy={160 - rate * 130} r="4" fill="currentColor" />)}
      </svg>
    </figure>)}</div>
    <div className="ti-table-wrap"><table><caption>Transfer-probe pass rates shown in the learning curves</caption><thead><tr><th>Configuration / tools</th><th>Before feedback</th><th>After round 1</th><th>After round 3</th></tr></thead><tbody>{series.map(s => <tr key={s.name}><th scope="row">{s.name}</th>{s.rates.map((rate, i) => <td key={i}>{pct(rate)}</td>)}</tr>)}</tbody></table></div>
  </>;
}

export interface PairedComparison {
  modelA: string; modelB: string; unit: string; samples: number; confidence: number;
  rows: Array<{ condition: Condition; mode: ToolMode; families: number; difference: number; interval: [number, number] | null }>;
}
export function ComparisonView({ comparison: c }: { comparison: PairedComparison }) {
  const points = (n: number) => `${n > 0 ? "+" : ""}${(n * 100).toFixed(1)} pp`;
  return <><p>{c.modelA} minus {c.modelB}. Differences are in percentage points (pp); positive values favor {c.modelA}. Intervals use 10,000 paired task-family bootstrap samples.</p>
    <div className="ti-table-wrap"><table><caption>Paired success-rate differences with 95% intervals</caption><thead><tr><th>Condition</th><th>Tools</th><th>Families</th><th>Difference</th><th>95% interval</th></tr></thead><tbody>{c.rows.map(r => <tr key={`${r.condition}-${r.mode}`}><th scope="row">{r.condition}</th><td>{r.mode}</td><td>{r.families}</td><td>{points(r.difference)}</td><td>{r.interval ? `${points(r.interval[0])} to ${points(r.interval[1])}` : "Unavailable"}</td></tr>)}</tbody></table></div>
  </>;
}
