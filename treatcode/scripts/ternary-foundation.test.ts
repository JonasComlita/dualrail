import { afterEach, expect, test } from "bun:test";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { DEFAULT_LIMITS, PILOT_LABEL, type Protocol, type ModelConfig } from "../src/ternary/contracts";
import { LabStore } from "../private/ternary/store";
import { WindowsWorker, checkSource } from "../private/ternary/worker";
import { normalizeResponse, ProviderSession, validateModel } from "../private/ternary/providers";

const roots: string[] = [];
const root = () => { const p = fs.mkdtempSync(path.join(os.tmpdir(), "ternary-foundation-")); roots.push(p); return p; };
afterEach(() => { for (const p of roots.splice(0)) fs.rmSync(p, { recursive: true, force: true }); });
const model: ModelConfig = { id: "test-openai", provider: "openai", model: "fixture-exact-model", keyEnv: "TI_FIXTURE_KEY", settings: {}, supportedSettings: {}, maxOutputTokens: 1024, pricing: { snapshot: "fixture-price", date: "2026-09-17", inputUsdPerMillion: 1, outputUsdPerMillion: 2 } };
const protocol: Protocol = { revision: "test", profile: "default", limits: { ...DEFAULT_LIMITS }, familyHashes: { test: "test" }, checkerHash: "test", workerHash: "test", compilerHash: "test", promptHash: "test", controlledFamilies: ["test"], assignments: [{ familyId: "test", condition: "specification", mode: "tools", seed: 1, repeat: 0 }], purpose: "calibration", experiment: "default", bootstrapSamples: 10000, label: PILOT_LABEL };

test("SQLite idempotency, spending reservation, and restart preserve exposed episodes", () => {
  const dir = root(); let store = new LabStore(dir);
  const input = { protocol, models: [model], ceilingUsd: 1, owner: "operator", requestKey: "test-request", provenance: "fixture" as const };
  const run = store.createRun(input);
  expect(store.createRun(input).id).toBe(run.id);
  expect(() => store.createRun({ ...input, ceilingUsd: 2 })).toThrow("different input");
  const reservation = store.reserve(run.id, 900_000_000);
  expect(() => store.reserve(run.id, 200_000_000)).toThrow("ceiling");
  const episode = store.episodes(run.id)[0];
  expect(store.startEpisode(episode.id)).toBe(true);
  store.state(run.id, "running"); store.close();
  store = new LabStore(dir); expect(store.recover()).toEqual([episode.id]);
  expect(store.episodes(run.id)[0].state).toBe("interrupted");
  expect(store.run(run.id).spentNanoUsd).toBe(900_000_000);
  expect(store.run(run.id).reservedNanoUsd).toBe(0);
  expect(() => store.settle(reservation, 1)).toThrow("already settled");
  const digest = store.putArtifact({ secret: "private-only" });
  expect(store.artifact(digest)).toEqual({ secret: "private-only" });
  expect(() => store.artifact("../lab.sqlite")).toThrow();
  store.close();
});

test("provider normalization counts reasoning, caches and refusal without inventing usage", () => {
  expect(normalizeResponse("google", { candidates: [{ content: { parts: [{ text: "42" }] }, finishReason: "STOP" }], usageMetadata: { promptTokenCount: 11, candidatesTokenCount: 7, thoughtsTokenCount: 13 } }).usage.outputTokens).toBe(20);
  expect(normalizeResponse("anthropic", { content: [], stop_reason: "refusal", usage: { input_tokens: 2, output_tokens: 1, cache_read_input_tokens: 10, cache_creation_input_tokens: 3 } }).usage.inputTokens).toBe(15);
  expect(() => normalizeResponse("openai", { output: [], status: "completed" })).toThrow("omitted valid");
  expect(() => normalizeResponse("openai", { output: [], status: "in_progress" })).toThrow("unsupported completion");
  expect(() => normalizeResponse("openai", { output: [], status: "completed", usage: { input_tokens: 1, output_tokens: 1, output_tokens_details: { reasoning_tokens: -1 } } })).toThrow("reasoning tokens");
  expect(() => validateModel({ ...model, settings: { temperature: 5 }, supportedSettings: { temperature: [0,1] } })).toThrow("unsupported");
});

test("Responses continuation retains encrypted reasoning and tool call identity; a new episode is empty", async () => {
  process.env.TI_FIXTURE_KEY = "fixture-no-network";
  const requests: any[] = [];
  const raw = { id: "r1", model: model.model, status: "completed", output: [{ type: "reasoning", id: "opaque1", encrypted_content: "signature" }, { type: "function_call", call_id: "call1", name: "list_files", arguments: "{}" }], usage: { input_tokens: 10, output_tokens: 20, output_tokens_details: { reasoning_tokens: 8 } } };
  const fake = (async (_url: unknown, init: RequestInit) => { requests.push(JSON.parse(init.body as string)); return Response.json(raw); }) as typeof fetch;
  const s = new ProviderSession(model, "system", [], fake); s.user("first");
  const result = await s.next(50, new AbortController().signal);
  s.results([{ id: result.calls[0].id, name: "list_files", result: ["solution.trit"] }]);
  expect((s.request(50).body.input as any[])[1].encrypted_content).toBe("signature");
  expect((s.request(50).body.input as any[])[3].call_id).toBe("call1");
  expect(new ProviderSession(model, "system", []).request(50).body.input).toEqual([]);
  expect(requests).toHaveLength(1);
  delete process.env.TI_FIXTURE_KEY;
});

test("Windows compiler/VM grades a reference and kills a wrong repair without leaking cases", async () => {
  const worker = new WindowsWorker(path.resolve(import.meta.dir, "../.."), root());
  expect(worker.readiness().ready).toBe(true);
  const cases = [{ id: "one", args: [2,3,0] as [number,number,number], expected: 5 }];
  const good = await worker.grade("fn solve(a: t40, b: t40, c: t40) -> t40 { return a + b; }", cases, { ...DEFAULT_LIMITS }, new AbortController().signal);
  expect(good).toMatchObject({ passed: true, category: "correct" });
  const bad = await worker.grade("fn solve(a: t40, b: t40, c: t40) -> t40 { return a - b; }", cases, { ...DEFAULT_LIMITS }, new AbortController().signal);
  expect(bad).toMatchObject({ passed: false, category: "wrong_answer" });
  expect(() => checkSource('import "secret"; fn solve(a:int,b:int,c:int)->int{return 0;}')).toThrow();
  expect(() => checkSource("fn solve(a:int,b:int,c:int)->int{return sys_open(a);}")).toThrow();
}, 20000);
