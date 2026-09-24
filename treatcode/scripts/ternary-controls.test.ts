import { afterEach, expect, test } from "bun:test";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import express from "express";
import { AuthStore, DEFAULT_PROJECT_ID } from "../src/auth";
import { DEFAULT_LIMITS, PILOT_LABEL, type ModelConfig, type Observation, type Publication } from "../src/ternary/contracts";
import { EXAMPLES } from "../src/ternary/public";
import { interpretExample } from "../src/ternary/example-interpreter";
import { corpus } from "../private/ternary/corpus";
import { IntelligenceLab } from "../private/ternary/service";
import { intelligenceRouter } from "../private/ternary/routes";
import { ProviderSession, ProviderFailure, normalizeResponse } from "../private/ternary/providers";
import { calibrationSummary, compare, scoreRows } from "../private/ternary/reporting";
import { checkSource } from "../private/ternary/worker";
import { hash } from "../private/ternary/util";

const repo = path.resolve(import.meta.dir, "../..");
const roots: string[] = [], labs: IntelligenceLab[] = [];
const temp = () => { const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ternary-controls-")); roots.push(dir); return dir; };
const model: ModelConfig = { id: "fixture-a", provider: "openai", model: "fixture-not-a-real-model", keyEnv: "TI_CONTROLS_FIXTURE_KEY", settings: {}, supportedSettings: {}, maxOutputTokens: 1024, pricing: null };
const lab = (factory?: NonNullable<ConstructorParameters<typeof IntelligenceLab>[2]>["sessionFactory"]) => { const l = new IntelligenceLab(repo, temp(), { autoStart: false, sessionFactory: factory ? (...args) => { const session=factory(...args);session.countInput=async()=>30;return session; } : undefined }); labs.push(l); return l; };
afterEach(() => { for (const l of labs.splice(0)) l.close(); delete process.env.TI_CONTROLS_FIXTURE_KEY; for (const dir of roots.splice(0)) fs.rmSync(dir, { recursive: true, force: true }); });
const response = (text: string, extra: Record<string, unknown> = {}) => ({ id: "fixture-response", model: model.model, status: "completed", output: [{ type: "message", content: [{ type: "output_text", text }] }], usage: { input_tokens: 30, output_tokens: 40 }, ...extra });
function enqueue(l: IntelligenceLab, assignments: Array<{ familyId: string; condition: "prior" | "specification" | "learning"; mode: "model-only" | "tools"; seed: number; repeat: number }>, limitOverrides = {}) {
  const protocol = l.protocol({ modelIds: [model.id], purpose: "calibration", experiment: "controlled", spendingCeilingUsd: null, limits: limitOverrides });
  protocol.assignments = assignments;
  return l.store.createRun({ protocol, models: [model], ceilingUsd: null, owner: "fixture", requestKey: crypto.randomUUID(), provenance: "fixture" });
}
test("corpus has 36 independent packages, balanced coverage, exact oracles, and disjoint learning probes", () => {
  const tasks = corpus(); expect(tasks).toHaveLength(36); expect(corpus()).toEqual(tasks);
  for (const capability of ["representation", "logic", "learning", "algorithms", "debugging", "systems"]) {
    const family = tasks.filter(t => t.capability === capability); expect(family).toHaveLength(6);
    for (const difficulty of ["introductory", "intermediate", "advanced"]) expect(family.filter(t => t.difficulty === difficulty)).toHaveLength(2);
  }
  for (const task of tasks) {
    const stages = [...task.publicCases, ...task.feedback.flat(), ...task.probes.flat()];
    expect(new Set(stages.map(c => c.args.join(","))).size).toBe(stages.length);
    expect(task.probes.every(p => p.length > 0)).toBe(true);
    expect(task.qualificationCases.every(c => Number.isSafeInteger(c.expected))).toBe(true);
    expect(task.mutants.every(m => m.source !== task.reference && m.targetCaseIds.length > 0)).toBe(true);
    expect(EXAMPLES.some(e => e.id === task.id)).toBe(false);
  }
  expect(tasks.find(t => t.id === "representation-01")!.qualificationCases).toHaveLength(27);
  expect(tasks.find(t => t.id === "representation-02")!.qualificationCases).toHaveLength(125);
});
test("public interpreter checks carries, unknowns, negative steps, and rejects executable input", () => {
  expect(interpretExample("addition", '{"a":11,"b":7}')).toEqual({ sum: 18, carry: 1, word: -9, balancedDigits: "T00" });
  expect(interpretExample("addition", '{"a":-13,"b":-13}')).toEqual({ sum: -26, carry: -1, word: 1, balancedDigits: "001" });
  expect(interpretExample("consensus", '{"votes":[1,0,-1]}')).toEqual({ decision: 0 });
  expect(interpretExample("orbit", '{"color":"moss","steps":-1}')).toEqual({ color: "violet" });
  for (const text of ["globalThis.fetch('secret')", '{"a":999,"b":0}', '{"a":0.5,"b":1}']) expect(() => interpretExample("addition", text)).toThrow();
  expect(() => interpretExample("median", "{}")).toThrow();
});
test("Claude and Gemini retain signed assistant content and tool identity across continuation", async () => {
  process.env.TI_CONTROLS_FIXTURE_KEY = "fixture";
  const fixtures = [
    { provider: "anthropic" as const, raw: { model: "exact-claude", content: [{ type: "thinking", thinking: "private", signature: "signed-claude" }, { type: "tool_use", id: "tool-c", name: "list_files", input: {} }], stop_reason: "tool_use", usage: { input_tokens: 10, output_tokens: 20 } }, signature: "signed-claude", id: "tool-c" },
    { provider: "google" as const, raw: { modelVersion: "exact-gemini", candidates: [{ content: { role: "model", parts: [{ thoughtSignature: "signed-gemini", functionCall: { id: "tool-g", name: "list_files", args: {} } }] }, finishReason: "STOP" }], usageMetadata: { promptTokenCount: 10, candidatesTokenCount: 20, thoughtsTokenCount: 8 } }, signature: "signed-gemini", id: "tool-g" },
  ];
  for (const f of fixtures) {
    let calls = 0;
    const s = new ProviderSession({ ...model, provider: f.provider }, "system", [], (async () => { calls++; return Response.json(f.raw); }) as typeof fetch);
    s.user("task"); const turn = await s.next(100, new AbortController().signal); s.results([{ id: turn.calls[0].id, name: "list_files", result: ["solution.trit"] }]);
    expect(JSON.stringify(s.request(100).body)).toContain(f.signature); expect(JSON.stringify(s.request(100).body)).toContain(f.id); expect(calls).toBe(1);
    expect(JSON.stringify(new ProviderSession({ ...model, provider: f.provider }, "system", []).request(100).body)).not.toContain(f.signature);
  }
});
test("transport, rate limit, malformed response, and cancellation never trigger ambiguous retries", async () => {
  process.env.TI_CONTROLS_FIXTURE_KEY = "fixture";
  for (const fixture of [() => { throw new Error("connection lost"); }, () => new Response("rate limited", { status: 429 }), () => new Response("server failed", { status: 503 }), () => new Response("not JSON")]) {
    let calls = 0; const s = new ProviderSession(model, "system", [], (async () => { calls++; return fixture(); }) as typeof fetch); s.user("task");
    await expect(s.next(100, new AbortController().signal)).rejects.toBeInstanceOf(ProviderFailure); expect(calls).toBe(1);
  }
  const controller = new AbortController(); controller.abort();
  const s = new ProviderSession(model, "system", [], (async () => { throw new Error("aborted"); }) as typeof fetch);
  await expect(s.next(100, controller.signal)).rejects.toMatchObject({ code: "request_interrupted", ambiguous: true });
  expect(normalizeResponse("openai", response("", { output: [{ type: "message", content: [{ type: "refusal", refusal: "declined" }] }] })).status).toBe("refusal");
  expect(() => new ProviderSession({ ...model, provider: "anthropic", maxOutputTokens: 4096, settings: { "thinking.type": "enabled", "thinking.budget_tokens": 1024 }, supportedSettings: { "thinking.type": ["enabled"], "thinking.budget_tokens": [1024] } }, "system", []).request(1000)).toThrow("thinking budget");
});
test("provider token counting sends system text, tool schemas and complete continuation", async () => {
  process.env.TI_CONTROLS_FIXTURE_KEY="fixture";
  for(const provider of ["openai","anthropic","google"] as const){
    const requests:any[]=[];
    const session=new ProviderSession({...model,provider},"counted-system",[{name:"list_files",description:"list",parameters:{type:"object",properties:{}}}],(async(url,init)=>{requests.push({url,body:JSON.parse(init!.body as string)});return Response.json(provider==="google"?{totalTokens:731}:{input_tokens:731});}) as typeof fetch);
    session.user("counted-input");expect(await session.countInput(1024,new AbortController().signal)).toBe(731);
    expect(JSON.stringify(requests[0].body)).toContain("counted-system");expect(JSON.stringify(requests[0].body)).toContain("counted-input");expect(JSON.stringify(requests[0].body)).toContain("list_files");
    expect(String(requests[0].url)).toContain(provider==="google"?":countTokens":provider==="openai"?"/responses/input_tokens":"/messages/count_tokens");
  }
});
test("provider verification rejects unsupported configurations before task dispatch and invalidates on configuration changes",async()=>{
  process.env.TI_CONTROLS_FIXTURE_KEY="fixture";let calls=0;
  const l=lab((config,system,tools)=>new ProviderSession(config,system,tools,(async()=>{calls++;return config.settings.temperature===2?new Response("unsupported temperature",{status:400}):Response.json(response(JSON.stringify({files:{"solution.trit":`fn solve(a:t40,b:t40,c:t40)->t40{return ${tools.length?53:37};}`}})));}) as typeof fetch));
  l.configure({models:[model],calibrationModelIds:[],calibrationTiers:{}});
  const good=await l.verifyModel(model.id);expect(good.state).toBe("passed");expect(good.requestCount).toBe(2);expect(good.provenance).toBe("fixture");expect(l.readiness().models[0].ready).toBe(true);
  l.configure({models:[{...model,settings:{temperature:2},supportedSettings:{temperature:[2]}}],calibrationModelIds:[],calibrationTiers:{}});
  expect(l.readiness().models[0].ready).toBe(false);const rejected=await l.verifyModel(model.id);expect(rejected.state).toBe("failed");expect(rejected.failures[0]).toContain("HTTP 400");expect(calls).toBe(3);expect(l.store.runs()).toHaveLength(0);
});
test("compatibility rejects a syntactically valid but behaviorally wrong constant answer",async()=>{
  process.env.TI_CONTROLS_FIXTURE_KEY="fixture";
  const l=lab((config,system,tools)=>new ProviderSession(config,system,tools,(async()=>Response.json(response(JSON.stringify({files:{"solution.trit":"fn solve(a:t40,b:t40,c:t40)->t40{return 0;}"}})))) as typeof fetch));
  l.configure({models:[model],calibrationModelIds:[model.id],calibrationTiers:{[model.id]:"lower"}});
  const result=await l.verifyModel(model.id);
  expect(result.state).toBe("failed");expect(result.requestCount).toBe(1);
  expect(result.failures[0]).toContain("requested behavior");expect(l.readiness().models[0].ready).toBe(false);
});

test("learning preserves one episode context, measures rounds 0/1/3, then resets for the next family", async () => {
  process.env.TI_CONTROLS_FIXTURE_KEY = "fixture";
  const sessions: any[][] = [];
  const l = lab((config, system, tools) => {
    const requests: any[] = []; sessions.push(requests);
    return new ProviderSession(config, system, tools, (async (_url, init) => {
      const body = JSON.parse(init!.body as string); requests.push(body);
      const task = corpus().find(t => body.input[0].content.startsWith(t.title))!;
      return Response.json(response(JSON.stringify({ files: { "solution.trit": task.reference } })));
    }) as typeof fetch);
  });
  const run = enqueue(l, [
    { familyId: "logic-01", condition: "learning", mode: "model-only", seed: 1, repeat: 0 },
    { familyId: "representation-01", condition: "prior", mode: "model-only", seed: 2, repeat: 0 },
  ]);
  await l.pump();
  const episodes = l.store.episodes(run.id); expect(episodes.map(e => e.state)).toEqual(["passed", "passed"]);
  expect(episodes[0].observation!.learning.map(s => s.round)).toEqual([0, 1, 3]); expect(episodes[0].observation!.learning.every(s => s.passed === s.total)).toBe(true);
  expect(sessions.map(s => s.length)).toEqual([4, 1]); expect(sessions[0][3].input.length).toBe(7); expect(sessions[1][0].input).toHaveLength(1);
  expect(sessions.flat().every(r => r.tools.length === 0)).toBe(true);
  expect(episodes.every(e => e.observation!.resources.costNanoUsd === null)).toBe(true);
  expect(l.publicationFailures(l.store.run(run.id)).join(" ")).toContain("fixtures cannot be published");
}, 30000);
test("structured tools reject traversal, hidden evidence, and forbidden file replacement", async () => {
  process.env.TI_CONTROLS_FIXTURE_KEY = "fixture"; const requests: any[] = [];
  const l = lab((config, system, tools) => new ProviderSession(config, system, tools, (async (_url, init) => {
    const body = JSON.parse(init!.body as string); requests.push(body);
    if (requests.length === 1) return Response.json(response("", { output: [
      { type: "function_call", call_id: "1", name: "read_file", arguments: '{"path":"../../private/corpus.ts"}' },
      { type: "function_call", call_id: "2", name: "replace_file", arguments: '{"path":"grader.trit","content":"changed"}' },
    ] }));
    return Response.json(response(JSON.stringify({ files: { "solution.trit": corpus()[0].reference } })));
  }) as typeof fetch));
  const run = enqueue(l, [{ familyId: "representation-01", condition: "specification", mode: "tools", seed: 1, repeat: 0 }]); await l.pump();
  const outputs = requests[1].input.filter((v: any) => v.type === "function_call_output"); expect(outputs).toHaveLength(2);
  expect(outputs.every((o: any) => JSON.parse(o.output).error)).toBe(true); expect(l.store.episodes(run.id)[0].state).toBe("passed");
  for (const source of ["fn sys_open(a:t40)->t40{return 0;} fn solve(a:t40,b:t40,c:t40)->t40{return sys_open(a);}", "fn solve(a:t40,b:t40,c:t40)->t40{return __read(a);}"]) expect(() => checkSource(source)).toThrow();
}, 15000);
test("budget exhaustion is a model failure; transport interruption leaves coverage incomplete", async () => {
  process.env.TI_CONTROLS_FIXTURE_KEY = "fixture";
  for (const mode of ["length", "transport"]) {
    let requests = 0;
    const l = lab((config, system, tools) => new ProviderSession(config, system, tools, (async () => { requests++; if (mode === "transport") throw new Error("lost"); return Response.json(response("", { status: "incomplete" })); }) as typeof fetch));
    const run = enqueue(l, [{ familyId: "logic-01", condition: "prior", mode: "model-only", seed: 1, repeat: 0 }]); await l.pump();
    expect(l.store.episodes(run.id)[0].observation).toMatchObject(mode === "length" ? { success: false, state: "failed", category: "budget_exhausted" } : { success: null, state: "infrastructure_error", category: "transport_error" }); expect(requests).toBe(1);
  }
});
test("operator role and grants independently protect every private endpoint", async () => {
  const l = lab(), auth = new AuthStore({ identity_path: null, audit_path: null });
  const tokens = new Map<string, string>();
  for (const role of ["participant", "operator", "operator-read-only"]) {
    const id = `tc:identity:${role}`;
    auth.registerIdentity({ id, kind: "human", display_name: role, access_key: "fixture-password", roles: role === "participant" ? ["participant"] : ["benchmark-operator"], grants: [{ project_id: "*", actions: role === "operator-read-only" ? ["read"] : ["read", "edit", "benchmark", "artifact"] }] });
    const login = auth.login(id, "fixture-password"); if (!login.ok) throw new Error("fixture login failed"); tokens.set(role, login.credential.token);
  }
  const app = express(); app.use(express.json()); app.use("/api", intelligenceRouter(l, auth));
  const server = app.listen(0, "127.0.0.1"); await new Promise<void>(resolve => server.once("listening", resolve));
  const address = server.address() as { port: number }, url = `http://127.0.0.1:${address.port}/api`;
  try {
    for (const route of ["/overview", "/methodology", "/examples", "/publications"]) { const res = await fetch(url + route); expect(res.status).toBe(200); expect(JSON.stringify(await res.json())).not.toContain("targetCaseIds"); }
    for (const [method, route] of [["GET", "/operator/readiness"], ["GET", "/operator/configuration"], ["GET", "/operator/runs"], ["GET", "/operator/artifacts/" + "0".repeat(64)], ["POST", "/operator/runs"], ["POST", "/operator/qualify"], ["POST", "/operator/freeze"], ["POST", "/operator/runs/unknown/publish"], ["POST", "/operator/runs/unknown/cancel"]]) {
      const res = await fetch(url + route, { method, headers: { Authorization: `Bearer ${tokens.get("participant")}`, "x-action-nonce": crypto.randomUUID(), "Content-Type": "application/json" }, ...(method === "POST" ? { body: "{}" } : {}) }); expect(res.status).toBe(403);
    }
    const read = await fetch(url + "/operator/readiness", { headers: { Authorization: `Bearer ${tokens.get("operator")}` } }); expect(read.status).toBe(200);
    const denied = await fetch(url + "/operator/configuration", { method: "POST", headers: { Authorization: `Bearer ${tokens.get("operator-read-only")}`, "x-action-nonce": crypto.randomUUID(), "Content-Type": "application/json" }, body: "{}" }); expect(denied.status).toBe(403);
    expect((await l.store.publications()).length).toBe(0);
  } finally { await new Promise<void>((resolve, reject) => server.close(e => e ? reject(e) : resolve())); }
});

test("credentials disappearing after preflight leave coverage incomplete without dispatch", async () => {
  let requests=0;
  delete process.env.TI_CONTROLS_FIXTURE_KEY;
  const l=lab((config,system,tools)=>new ProviderSession(config,system,tools,(async()=>{requests++;return Response.json(response("unused"));}) as typeof fetch));
  const run=enqueue(l,[{familyId:"logic-01",condition:"prior",mode:"model-only",seed:1,repeat:0}]);
  await l.pump();
  expect(requests).toBe(0);
  expect(l.store.episodes(run.id)[0].observation).toMatchObject({success:null,state:"infrastructure_error",category:"credential_missing"});
});
test("bootstrap groups repeat attempts under families and rejects mismatched coverage", () => {
  const l = lab(), protocol = l.protocol({ modelIds: [], purpose: "evaluation", experiment: "default", spendingCeilingUsd: null });
  const resources = { inputTokens: 0, outputTokens: 0, reasoningTokens: null, cachedInputTokens: null, toolCalls: 0, publicTestCalls: 0, modelMs: 0, programMs: null, peakMemoryBytes: null, costNanoUsd: null };
  const obs = (modelConfigId: string, familyId: string, repeat: number, success: boolean): Observation => ({ modelConfigId, familyId, repeat, success, condition: "specification", mode: "tools", state: success ? "passed" : "failed", category: "fixture", resources, learning: [], robustness: null });
  const observations = [obs("a", "logic-01", 0, true), obs("a", "logic-01", 1, true), obs("a", "logic-01", 2, true), obs("a", "logic-02", 0, false)];
  const rows = scoreRows(observations, protocol, ["a"]); expect(rows.find(r => r.capability === "all")!.rate).toBe(0.5); expect(scoreRows(observations, protocol, ["a"])).toEqual(rows);
  const a = { id: "fixture-publication", profileHash: "same", observations, coverage: { complete: true } } as Publication;
  const b = { ...a, observations: observations.map(o => ({ ...o, modelConfigId: "b", success: !o.success })) };
  const compared = compare(a, "a", b, "b"); expect(compared.rows[0].families).toBe(2); expect(compared.rows[0].difference).toBe(0); expect(compare(a, "a", b, "b")).toEqual(compared);
  expect(() => compare(a, "a", { ...b, profileHash: "other" }, "b")).toThrow("profiles"); expect(() => compare(a, "a", { ...b, observations: b.observations.slice(1) }, "b")).toThrow("coverage");
  expect(calibrationSummary([], corpus().map(f => f.id), []).requiresRevision).toBe(false); expect(calibrationSummary([], corpus().map(f => f.id), []).coverageComplete).toBe(false);
});
test("pricing and immutable publication cannot turn fixture observations into public results", () => {
  const l = lab(), run = enqueue(l, [{ familyId: "logic-01", condition: "prior", mode: "model-only", seed: 1, repeat: 0 }]);
  const reservation = l.store.reserve(run.id, 5); expect(() => l.store.settle(reservation, 10)).toThrow("upper bound"); expect(l.store.run(run.id).spentNanoUsd).toBe(10);
  expect(() => l.store.publish({ id: "fixture", runId: run.id } as Publication)).toThrow("live evaluations");
  l.cancel(run.id); expect(l.store.episodes(run.id)[0].state).toBe("cancelled"); expect(l.store.run(run.id).state).toBe("cancelled");
  const p = { id: "immutable-fixture", runId: run.id, label: PILOT_LABEL };
  l.store.db.query("INSERT INTO publications VALUES(?,?,?,?)").run(p.id, run.id, new Date().toISOString(), JSON.stringify(p));
  expect(() => l.store.db.query("UPDATE publications SET payload='{}' WHERE id=?").run(p.id)).toThrow("immutable"); expect(() => l.store.db.query("DELETE FROM publications WHERE id=?").run(p.id)).toThrow("immutable");
});

test("restart preserves finished work, marks exposed work interrupted, and dispatches only pending episodes", async () => {
  process.env.TI_CONTROLS_FIXTURE_KEY="fixture";
  const assignments=["logic-01","logic-02","logic-03"].map((familyId,i)=>({familyId,condition:"prior" as const,mode:"model-only" as const,seed:i,repeat:0}));
  const original=lab(),run=enqueue(original,assignments),episodes=original.store.episodes(run.id);
  original.store.state(run.id,"running");original.store.startEpisode(episodes[0].id);
  const completed:Observation={...assignments[0],modelConfigId:model.id,state:"passed",success:true,category:"correct",learning:[],robustness:{passed:1,total:1},resources:{inputTokens:30,outputTokens:40,reasoningTokens:null,cachedInputTokens:null,toolCalls:0,publicTestCalls:0,modelMs:1,programMs:1,peakMemoryBytes:null,costNanoUsd:null}};
  original.store.finishEpisode(episodes[0].id,completed);original.store.startEpisode(episodes[1].id);original.store.reserve(run.id,77);
  original.close();labs.splice(labs.indexOf(original),1);
  let requests=0;
  const restarted=new IntelligenceLab(repo,original.root,{autoStart:false,sessionFactory:(config,system,tools)=>{
    const session=new ProviderSession(config,system,tools,(async(_url,init)=>{requests++;expect(String(init!.body)).toContain(corpus().find(t=>t.id==="logic-03")!.title);return Response.json(response(JSON.stringify({files:{"solution.trit":corpus().find(t=>t.id==="logic-03")!.reference}})));}) as typeof fetch);session.countInput=async()=>30;return session;
  }});labs.push(restarted);
  expect(restarted.store.episodes(run.id).map(e=>e.state)).toEqual(["passed","interrupted","pending"]);
  expect(restarted.store.run(run.id).spentNanoUsd).toBe(77);await restarted.pump();
  expect(requests).toBe(1);expect(restarted.store.episodes(run.id).map(e=>e.state)).toEqual(["passed","interrupted","passed"]);
  expect(restarted.store.episodes(run.id)[0].observation).toEqual(completed);
  expect(restarted.publicationFailures(restarted.store.run(run.id)).join(" ")).toContain("Coverage is incomplete");
},15000);

test("frozen orchestration mismatch fails before provider dispatch",async()=>{
  let calls=0;const l=lab((config,system,tools)=>new ProviderSession(config,system,tools,(async()=>{calls++;return Response.json(response("unused"));}) as typeof fetch));
  const protocol=l.protocol({modelIds:[model.id],purpose:"calibration",experiment:"default",spendingCeilingUsd:null});protocol.assignments=protocol.assignments.slice(0,1);protocol.promptHash="earlier-build";
  const run=l.store.createRun({protocol,models:[model],ceilingUsd:null,owner:"fixture",requestKey:crypto.randomUUID(),provenance:"fixture"});await l.pump();
  expect(calls).toBe(0);expect(l.store.episodes(run.id)[0].observation).toMatchObject({success:null,state:"infrastructure_error",category:"frozen_mismatch"});
});

test("tool and public-test budgets are enforced independently",async()=>{
  process.env.TI_CONTROLS_FIXTURE_KEY="fixture";
  for(const tool of ["list_files","run_public_tests"]){
    const l=lab((config,system,tools)=>new ProviderSession(config,system,tools,(async()=>Response.json(response("",{output:[0,1].map(i=>({type:"function_call",call_id:String(i),name:tool,arguments:tool==="list_files"?"{}":'{"name":"public"}'}))}))) as typeof fetch));
    const run=enqueue(l,[{familyId:"logic-01",condition:"prior",mode:"tools",seed:1,repeat:0}],tool==="list_files"?{toolCalls:1}:{publicTestCalls:1});await l.pump();
    expect(l.store.episodes(run.id)[0].observation).toMatchObject({success:false,state:"failed",category:"budget_exhausted"});
  }
},15000);

test("pricing changes resource observations without changing correctness or protocol",async()=>{
  process.env.TI_CONTROLS_FIXTURE_KEY="fixture";
  const outcomes:Observation[]=[],profiles:string[]=[];
  for(const pricing of [null,{snapshot:"fixture-pricing",date:"2026-09-22",inputUsdPerMillion:1,outputUsdPerMillion:2}]){
    const l=lab((config,system,tools)=>new ProviderSession(config,system,tools,(async()=>Response.json(response(JSON.stringify({files:{"solution.trit":corpus()[0].reference}})))) as typeof fetch));
    const protocol=l.protocol({modelIds:[model.id],purpose:"calibration",experiment:"default",spendingCeilingUsd:null});protocol.assignments=protocol.assignments.slice(0,1);profiles.push(hash(protocol));
    const run=l.store.createRun({protocol,models:[{...model,pricing}],ceilingUsd:null,owner:"fixture",requestKey:crypto.randomUUID(),provenance:"fixture"});await l.pump();outcomes.push(l.store.episodes(run.id)[0].observation!);
  }
  expect(profiles[0]).toBe(profiles[1]);expect(outcomes.map(o=>o.success)).toEqual([true,true]);expect(outcomes[0].resources.costNanoUsd).toBeNull();expect(outcomes[1].resources.costNanoUsd).toBeGreaterThan(0);
  expect(outcomes[0].providerModels).toEqual([model.model]);
},15000);
