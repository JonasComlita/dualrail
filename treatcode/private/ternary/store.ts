import { Database } from "bun:sqlite";
import { mkdirSync, readFileSync, writeFileSync, existsSync } from "node:fs";
import path from "node:path";
import { randomUUID } from "node:crypto";
import type { Assignment, EpisodeState, EpisodeView, ModelConfig, Observation, Protocol, Publication, RunEvent, RunState, RunView } from "../../src/ternary/contracts";
import { canonical, hash, invariant, LabError, now } from "./util";

type RunRecord = { id: string; state: RunState; created_at: string; updated_at: string; protocol_hash: string; payload: string; spent: number; reserved: number; ceiling: number | null; provenance: "live" | "fixture"; owner: string };
type EpisodeRecord = { id: string; run_id: string; ordinal: number; model_id: string; assignment: string; state: EpisodeState; observation: string | null };

/** This database and its objects are never mounted as static files. */
export class LabStore {
  readonly db: Database;
  readonly objects: string;
  constructor(readonly root: string) {
    mkdirSync(root, { recursive: true });
    this.objects = path.join(root, "objects");
    mkdirSync(this.objects, { recursive: true });
    this.db = new Database(path.join(root, "lab.sqlite"), { create: true, strict: true });
    this.db.exec(`PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA busy_timeout=5000;
      CREATE TABLE IF NOT EXISTS runs(id TEXT PRIMARY KEY, state TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
        protocol_hash TEXT NOT NULL, payload TEXT NOT NULL, spent INTEGER NOT NULL DEFAULT 0, reserved INTEGER NOT NULL DEFAULT 0,
        ceiling INTEGER, provenance TEXT NOT NULL, owner TEXT NOT NULL, request_key TEXT UNIQUE NOT NULL, request_hash TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS episodes(id TEXT PRIMARY KEY, run_id TEXT NOT NULL REFERENCES runs(id), ordinal INTEGER NOT NULL,
        model_id TEXT NOT NULL, assignment TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'pending', observation TEXT, UNIQUE(run_id, ordinal));
      CREATE TABLE IF NOT EXISTS events(sequence INTEGER PRIMARY KEY AUTOINCREMENT, run_id TEXT NOT NULL REFERENCES runs(id),
        episode_id TEXT REFERENCES episodes(id), type TEXT NOT NULL, at TEXT NOT NULL, data TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS reservations(id TEXT PRIMARY KEY, run_id TEXT NOT NULL REFERENCES runs(id), amount INTEGER NOT NULL, settled INTEGER NOT NULL DEFAULT 0);
      CREATE TABLE IF NOT EXISTS publications(id TEXT PRIMARY KEY, run_id TEXT UNIQUE NOT NULL REFERENCES runs(id), created_at TEXT NOT NULL, payload TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, payload TEXT NOT NULL);
      CREATE TRIGGER IF NOT EXISTS immutable_publication_update BEFORE UPDATE ON publications BEGIN SELECT RAISE(ABORT, 'immutable publication'); END;
      CREATE TRIGGER IF NOT EXISTS immutable_publication_delete BEFORE DELETE ON publications BEGIN SELECT RAISE(ABORT, 'immutable publication'); END;
      CREATE INDEX IF NOT EXISTS events_run ON events(run_id, sequence);
      CREATE INDEX IF NOT EXISTS episodes_run ON episodes(run_id, ordinal);`);
  }
  putArtifact(data: unknown): string {
    const bytes = canonical(data); const id = hash(bytes); const target = path.join(this.objects, id + ".json");
    if (existsSync(target)) invariant(readFileSync(target, "utf8") === bytes, "Artifact hash collision.");
    else writeFileSync(target, bytes, { flag: "wx", mode: 0o600 });
    return id;
  }
  artifact(id: string): unknown {
    invariant(/^[a-f0-9]{64}$/.test(id), "Invalid artifact identifier.");
    const data = readFileSync(path.join(this.objects, id + ".json"), "utf8");
    invariant(hash(data) === id, "Artifact integrity check failed.");
    return JSON.parse(data);
  }
  setting<T>(key: string): T | null {
    const row = this.db.query<{ payload: string }, [string]>("SELECT payload FROM settings WHERE key=?").get(key);
    return row ? JSON.parse(row.payload) : null;
  }
  setSetting(key: string, value: unknown) { this.db.query("INSERT INTO settings VALUES(?,?) ON CONFLICT(key) DO UPDATE SET payload=excluded.payload").run(key, canonical(value)); }
  createRun(input: { protocol: Protocol; models: ModelConfig[]; ceilingUsd: number | null; owner: string; requestKey: string; provenance: "live" | "fixture" }): RunView {
    invariant(input.requestKey.length >= 8 && input.requestKey.length <= 160, "An idempotency key of 8–160 characters is required.");
    invariant(input.ceilingUsd === null || Number.isFinite(input.ceilingUsd) && input.ceilingUsd > 0 && input.ceilingUsd <= 100000, "Invalid optional spending ceiling.");
    const requestHash = hash(input), requestKey = input.owner + ":" + input.requestKey;
    return this.db.transaction(() => {
      const prior = this.db.query<{ id: string; request_hash: string }, [string]>("SELECT id,request_hash FROM runs WHERE request_key=?").get(requestKey);
      if (prior) { invariant(prior.request_hash === requestHash, "Idempotency key was already used with different input."); return this.run(prior.id); }
      const id = randomUUID(), at = now(), protocolHash = this.putArtifact(input.protocol);
      this.db.query("INSERT INTO runs(id,state,created_at,updated_at,protocol_hash,payload,ceiling,provenance,owner,request_key,request_hash) VALUES(?,'queued',?,?,?,?,?,?,?,?,?)")
        .run(id, at, at, protocolHash, canonical({ protocol: input.protocol, models: input.models }), input.ceilingUsd === null ? null : Math.round(input.ceilingUsd * 1e9), input.provenance, input.owner, requestKey, requestHash);
      let ordinal = 0;
      for (const model of input.models) for (const assignment of input.protocol.assignments) this.db.query("INSERT INTO episodes(id,run_id,ordinal,model_id,assignment) VALUES(?,?,?,?,?)").run(randomUUID(), id, ordinal++, model.id, canonical(assignment));
      this.event(id, null, "run.created", { episodes: ordinal, protocolHash, provenance: input.provenance });
      return this.run(id);
    })();
  }
  run(id: string): RunView {
    const row = this.db.query<RunRecord, [string]>("SELECT * FROM runs WHERE id=?").get(id);
    if (!row) throw new LabError("not_found", "Run not found.", 404);
    const payload = JSON.parse(row.payload) as { protocol: Protocol; models: ModelConfig[] };
    const episodes = this.episodes(id);
    return { id, state: row.state, createdAt: row.created_at, updatedAt: row.updated_at, protocolHash: row.protocol_hash, ...payload,
      total: episodes.length, finished: episodes.filter(e => !["pending", "running"].includes(e.state)).length,
      activeEpisodeId: episodes.find(e => e.state === "running")?.id || null,
      spendingCeilingUsd: row.ceiling === null ? null : row.ceiling / 1e9, spentNanoUsd: row.spent, reservedNanoUsd: row.reserved,
      provenance: row.provenance, publicationFailures: [] };
  }
  runs(): RunView[] { return this.db.query<{ id: string }, []>("SELECT id FROM runs ORDER BY created_at DESC").all().map(row => this.run(row.id)); }
  episodes(runId: string): EpisodeView[] {
    return this.db.query<EpisodeRecord, [string]>("SELECT * FROM episodes WHERE run_id=? ORDER BY ordinal").all(runId).map(row => ({ id: row.id, runId: row.run_id, ordinal: row.ordinal, modelConfigId: row.model_id, assignment: JSON.parse(row.assignment), state: row.state, observation: row.observation ? JSON.parse(row.observation) : null }));
  }
  startEpisode(id: string): boolean {
    return this.db.transaction(() => {
      if (this.db.query("SELECT id FROM episodes WHERE state='running' LIMIT 1").get()) return false;
      return this.db.query("UPDATE episodes SET state='running' WHERE id=? AND state='pending'").run(id).changes === 1;
    })();
  }
  finishEpisode(id: string, observation: Observation) {
    const result = this.db.query("UPDATE episodes SET state=?,observation=? WHERE id=? AND state IN ('running','pending')").run(observation.state, canonical(observation), id);
    invariant(result.changes === 1, "Episode has already finished.");
  }
  state(id: string, state: RunState) { this.db.query("UPDATE runs SET state=?,updated_at=? WHERE id=?").run(state, now(), id); }
  event(runId: string, episodeId: string | null, type: string, data: unknown) {
    this.db.query("INSERT INTO events(run_id,episode_id,type,at,data) VALUES(?,?,?,?,?)").run(runId, episodeId, type, now(), canonical(data));
  }
  events(runId: string, after = 0): RunEvent[] {
    return this.db.query<{ sequence: number; episode_id: string | null; type: string; at: string; data: string }, [string, number]>("SELECT * FROM events WHERE run_id=? AND sequence>? ORDER BY sequence LIMIT 500").all(runId, after).map(row => ({ sequence: row.sequence, episodeId: row.episode_id, type: row.type, at: row.at, data: JSON.parse(row.data) }));
  }
  reserve(runId: string, amount: number): string {
    invariant(Number.isSafeInteger(amount) && amount >= 0, "Invalid cost reservation.");
    return this.db.transaction(() => {
      const row = this.db.query<RunRecord, [string]>("SELECT * FROM runs WHERE id=?").get(runId);
      invariant(row, "Run not found.");
      if(row.ceiling!==null&&row.spent+row.reserved+amount>row.ceiling)throw new LabError("spending_limit","The next request would exceed the spending ceiling.",409);
      const id = randomUUID();
      this.db.query("UPDATE runs SET reserved=reserved+? WHERE id=?").run(amount, runId);
      this.db.query("INSERT INTO reservations VALUES(?,?,?,0)").run(id, runId, amount);
      return id;
    })();
  }
  settle(reservationId: string, actual: number | null) {
    const exceeded=this.db.transaction(() => {
      const row = this.db.query<{ run_id: string; amount: number; settled: number }, [string]>("SELECT * FROM reservations WHERE id=?").get(reservationId);
      invariant(row && !row.settled, "Unknown or already settled reservation.");
      // Unknown completion keeps the full reservation charged; a retry cannot spend it twice.
      const charged = actual === null ? row.amount : actual;
      invariant(Number.isSafeInteger(charged) && charged >= 0, "Invalid measured cost.");
      this.db.query("UPDATE runs SET reserved=reserved-?,spent=spent+? WHERE id=?").run(row.amount, charged, row.run_id);
      this.db.query("UPDATE reservations SET settled=1 WHERE id=?").run(reservationId);
      return charged>row.amount;
    })();
    if(exceeded)throw new LabError("pricing_contract", "Provider usage exceeded its reserved upper bound.", 500);
  }
  recover(): string[] {
    return this.db.transaction(() => {
      const interrupted = this.db.query<{ id: string; run_id: string }, []>("SELECT id,run_id FROM episodes WHERE state='running'").all();
      for (const e of interrupted) {
        this.db.query("UPDATE episodes SET state='interrupted' WHERE id=?").run(e.id);
        this.event(e.run_id, e.id, "episode.interrupted", { reason: "Server restarted after episode exposure; it will not be replayed." });
      }
      for (const r of this.db.query<{ id: string }, []>("SELECT id FROM reservations WHERE settled=0").all()) this.settle(r.id, null);
      this.db.query("UPDATE runs SET state='queued',updated_at=? WHERE state IN ('running','interrupted') AND EXISTS(SELECT 1 FROM episodes WHERE episodes.run_id=runs.id AND episodes.state='pending')").run(now());
      this.db.query("UPDATE runs SET state='completed',updated_at=? WHERE state='running'").run(now());
      return interrupted.map(e => e.id);
    })();
  }
  publish(publication: Publication): Publication {
    const run=this.run(publication.runId);
    invariant(run.provenance==="live"&&run.protocol.purpose==="evaluation"&&run.state==="completed","Only completed live evaluations can be published.");
    const existing = this.db.query<{ payload: string }, [string]>("SELECT payload FROM publications WHERE run_id=?").get(publication.runId);
    if (existing) return JSON.parse(existing.payload);
    this.db.query("INSERT INTO publications VALUES(?,?,?,?)").run(publication.id, publication.runId, publication.createdAt, canonical(publication));
    this.putArtifact(publication);
    return publication;
  }
  publications(): Publication[] { return this.db.query<{ payload: string }, []>("SELECT payload FROM publications ORDER BY created_at DESC").all().map(r => JSON.parse(r.payload)); }
  close() { this.db.close(); }
}
