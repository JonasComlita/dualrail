import type { ModelConfig, Usage } from "../../src/ternary/contracts";
import { canonical, integer, invariant, LabError } from "./util";

export interface ToolDefinition { name: string; description: string; parameters: Record<string, unknown> }
export interface ToolCall { id: string; name: string; arguments: unknown }
export interface ToolResult { id: string; name: string; result: unknown }
export interface ProviderTurn {
  text: string; calls: ToolCall[]; status: "complete" | "tools" | "refusal" | "length";
  usage: Usage; responseId: string | null; reportedModel: string | null; raw: Record<string, any>;
}
export interface EvaluationSession {
  user(text:string):void;
  results(results:ToolResult[]):void;
  countInput(maxOutput:number,signal:AbortSignal):Promise<number|null>;
  next(maxOutput:number,signal:AbortSignal):Promise<ProviderTurn>;
  close?():void;
}
export class ProviderFailure extends LabError {
  constructor(code: string, message: string, readonly ambiguous: boolean, readonly httpStatus?: number) { super(code, message, 502); }
}
const SETTING_KEYS = {
  openai: ["temperature", "reasoning.effort", "text.verbosity"],
  anthropic: ["temperature", "thinking.type", "thinking.budget_tokens", "output_config.effort"],
  google: ["temperature", "thinkingConfig.thinkingBudget", "thinkingConfig.thinkingLevel"],
  codex: ["reasoning.effort"],
};
export function validateModel(config: ModelConfig): void {
  invariant(config && /^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$/.test(config.id), "Invalid model configuration ID.");
  invariant(Object.hasOwn(SETTING_KEYS, config.provider), "Unsupported provider.");
  invariant(typeof config.model === "string" && /^[a-zA-Z0-9][a-zA-Z0-9._:-]{0,159}$/.test(config.model), "An exact provider model ID is required.");
  invariant(typeof config.keyEnv === "string" && /^[A-Z][A-Z0-9_]{2,99}$/.test(config.keyEnv), "Configure credentials using an environment variable name.");
  integer(config.maxOutputTokens, 1, 24000, "maxOutputTokens");
  invariant(config.settings && typeof config.settings === "object" && !Array.isArray(config.settings) && config.supportedSettings && typeof config.supportedSettings === "object", "Settings and their supported values must be explicitly declared.");
  for (const [key, value] of Object.entries(config.settings)) {
    invariant(SETTING_KEYS[config.provider].includes(key), `The adapter does not support ${key}.`);
    invariant(Array.isArray(config.supportedSettings[key]) && config.supportedSettings[key].some(x => x === value), `${config.model}: unsupported value for ${key}.`);
  }
  const p = config.pricing;
  if(config.provider==="codex"){
    invariant(["gpt-6-astra","gpt-5.6-sol","gpt-5.6-terra","gpt-5.6-luna"].includes(config.model),"Select a model from the preselected local harness lineup.");
    invariant(p===null,"Subscription harness usage has no API pricing snapshot; report its cost as unavailable.");
  }
  if (p !== null) {
    invariant(p && typeof p.snapshot === "string" && p.snapshot.length > 0 && /^\d{4}-\d{2}-\d{2}/.test(p.date), "Pricing requires a dated snapshot identifier.");
    for (const rate of [p.inputUsdPerMillion, p.outputUsdPerMillion]) invariant(typeof rate === "number" && Number.isFinite(rate) && rate >= 0 && rate <= 10000, "Invalid pricing rate.");
  }
  if (config.provider === "anthropic" && config.settings["thinking.type"] === "enabled") {
    integer(config.settings["thinking.budget_tokens"], 1024, config.maxOutputTokens - 1, "thinking budget");
  }
}
function nestedSettings(settings: Record<string, unknown>): Record<string, any> {
  const result: Record<string, any> = {};
  for (const [key,value] of Object.entries(settings)) { const [a,b] = key.split("."); if (b) (result[a] ||= {})[b] = value; else result[a] = value; }
  return result;
}
function count(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isSafeInteger(value) || value < 0) throw new ProviderFailure("missing_usage", `Provider omitted valid ${label}.`, true);
  return value;
}
export function normalizeResponse(provider: ModelConfig["provider"], raw: Record<string, any>): ProviderTurn {
  if(!raw||typeof raw!=="object"||Array.isArray(raw))throw new ProviderFailure("malformed_response","Provider returned an invalid response object.",true);
  const optionalCount=(value:unknown,label:string)=>value===undefined||value===null?null:count(value,label);
  const calls: ToolCall[] = [];
  let text = "", status: ProviderTurn["status"] = "complete", usage: Usage;
  if (provider === "openai") {
    if (!Array.isArray(raw.output)) throw new ProviderFailure("malformed_response", "Responses API omitted output items.", true);
    for (const item of raw.output) {
      if (item.type === "function_call") {
        let args: unknown; try { args = JSON.parse(item.arguments); } catch { args = { invalid_arguments: true }; }
        calls.push({ id: item.call_id, name: item.name, arguments: args });
      }
      for (const block of item.content || []) { if (block.type === "output_text") text += block.text; if (block.type === "refusal") status = "refusal"; }
    }
    if (raw.status === "failed" || raw.status === "cancelled" || raw.error) throw new ProviderFailure("provider_failure", "Responses API did not complete the request.", true);
    if (raw.status === "incomplete") status = raw.incomplete_details?.reason === "content_filter" ? "refusal" : "length";
    else if(raw.status!=="completed")throw new ProviderFailure("unsupported_completion","Responses API returned an unsupported completion state.",true);
    usage = { inputTokens: count(raw.usage?.input_tokens, "input tokens"), outputTokens: count(raw.usage?.output_tokens, "output tokens"), reasoningTokens: optionalCount(raw.usage?.output_tokens_details?.reasoning_tokens,"reasoning tokens"), cachedInputTokens: optionalCount(raw.usage?.input_tokens_details?.cached_tokens,"cached tokens") };
  } else if (provider === "anthropic") {
    if (!Array.isArray(raw.content)) throw new ProviderFailure("malformed_response", "Messages API omitted content.", true);
    for (const block of raw.content) {
      if (block.type === "text") text += block.text;
      if (block.type === "tool_use") calls.push({ id: block.id, name: block.name, arguments: block.input });
    }
    if (raw.stop_reason === "refusal") status = "refusal";
    else if (raw.stop_reason === "max_tokens") status = "length";
    else if (!["end_turn", "tool_use", "stop_sequence"].includes(raw.stop_reason)) throw new ProviderFailure("unsupported_completion", "Messages API returned an unsupported completion state.", true);
    const cacheRead = raw.usage?.cache_read_input_tokens ?? 0, cacheWrite = raw.usage?.cache_creation_input_tokens ?? 0;
    usage = { inputTokens: count(raw.usage?.input_tokens, "input tokens") + count(cacheRead, "cached tokens") + count(cacheWrite, "cache creation tokens"), outputTokens: count(raw.usage?.output_tokens, "output tokens"), reasoningTokens: null, cachedInputTokens: cacheRead };
  } else {
    const candidate = raw.candidates?.[0];
    for (const part of candidate?.content?.parts || []) {
      if (part.text && !part.thought) text += part.text;
      if (part.functionCall) calls.push({ id: part.functionCall.id || `call-${calls.length}`, name: part.functionCall.name, arguments: part.functionCall.args });
    }
    if (raw.promptFeedback?.blockReason || ["SAFETY", "RECITATION", "BLOCKLIST", "PROHIBITED_CONTENT"].includes(candidate?.finishReason)) status = "refusal";
    else if (candidate?.finishReason === "MAX_TOKENS") status = "length";
    else if (candidate?.finishReason !== "STOP") throw new ProviderFailure("unsupported_completion", "Gemini returned an unsupported completion state.", true);
    const reasoning = optionalCount(raw.usageMetadata?.thoughtsTokenCount,"reasoning tokens");
    usage = { inputTokens: count(raw.usageMetadata?.promptTokenCount, "input tokens"), outputTokens: count(raw.usageMetadata?.candidatesTokenCount ?? (status === "refusal" ? 0 : undefined), "output tokens") + (reasoning??0), reasoningTokens: reasoning, cachedInputTokens: optionalCount(raw.usageMetadata?.cachedContentTokenCount,"cached tokens") };
  }
  if (calls.length && status === "complete") status = "tools";
  return { text, calls, status, usage, responseId: raw.id || raw.responseId || null, reportedModel: raw.model || raw.modelVersion || null, raw };
}

/** One instance per independent episode. Raw reasoning/signatures stay in private continuation state. */
export class ProviderSession {
  private history: any[] = [];
  constructor(readonly config: ModelConfig, readonly system: string, readonly tools: ToolDefinition[], private fetcher: typeof fetch = fetch) { validateModel(config); }
  user(text: string) {
    this.history.push(this.config.provider === "google" ? { role: "user", parts: [{ text }] } : { role: "user", content: text });
  }
  results(results: ToolResult[]) {
    if (this.config.provider === "openai") this.history.push(...results.map(r => ({ type: "function_call_output", call_id: r.id, output: canonical(r.result) })));
    else if (this.config.provider === "anthropic") this.history.push({ role: "user", content: results.map(r => ({ type: "tool_result", tool_use_id: r.id, content: canonical(r.result) })) });
    else this.history.push({ role: "user", parts: results.map(r => ({ functionResponse: { id: r.id, name: r.name, response: { result: r.result } } })) });
  }
  request(maxOutput: number): { url: string; headers: Record<string,string>; body: Record<string,unknown> } {
    invariant(this.config.provider!=="codex","Use the local harness adapter for Codex configurations.");
    const c = this.config, settings = nestedSettings(c.settings), key = process.env[c.keyEnv];
    if (!key) throw new LabError("credential_missing", `${c.id}: ${c.keyEnv} is not configured.`, 503);
    const limit = Math.min(maxOutput, c.maxOutputTokens);
    if(c.provider==="anthropic"&&c.settings["thinking.type"]==="enabled"&&Number(c.settings["thinking.budget_tokens"])>=limit)throw new LabError("budget_exhausted","Remaining output budget cannot support the configured thinking budget.");
    if (c.provider === "openai") return {
      url: "https://api.openai.com/v1/responses", headers: { Authorization: `Bearer ${key}`, "Content-Type": "application/json" },
      body: { model: c.model, instructions: this.system, input: this.history, store: false, include: ["reasoning.encrypted_content"], max_output_tokens: limit, ...settings,
        tools: this.tools.map(t => ({ type: "function", ...t, strict: false })) },
    };
    if (c.provider === "anthropic") return {
      url: "https://api.anthropic.com/v1/messages", headers: { "x-api-key": key, "anthropic-version": "2023-06-01", "Content-Type": "application/json" },
      body: { model: c.model, system: this.system, messages: this.history, max_tokens: limit, ...settings,
        ...(this.tools.length ? { tools: this.tools.map(t => ({ name: t.name, description: t.description, input_schema: t.parameters })) } : {}) },
    };
    return { url: `https://generativelanguage.googleapis.com/v1beta/models/${encodeURIComponent(c.model)}:generateContent`, headers: { "x-goog-api-key": key, "Content-Type": "application/json" },
      body: { systemInstruction: { parts: [{ text: this.system }] }, contents: this.history, generationConfig: { ...settings, maxOutputTokens: limit, candidateCount: 1 },
        ...(this.tools.length ? { tools: [{ functionDeclarations: this.tools.map(t => ({ name: t.name, description: t.description, parameters: t.parameters })) }] } : {}) },
    };
  }
  /** Count the complete native continuation, including system text and tool schemas. */
  async countInput(maxOutput:number,signal:AbortSignal):Promise<number>{
    const req=this.request(maxOutput),b=req.body;
    if(this.config.provider==="openai"){
      const body=Object.fromEntries(["model","instructions","input","tools","reasoning","text"].filter(k=>b[k]!==undefined).map(k=>[k,b[k]]));
      const raw=await this.jsonRequest({...req,url:req.url+"/input_tokens",body},signal,false);return count(raw.input_tokens,"counted input tokens");
    }
    if(this.config.provider==="anthropic"){
      const body=Object.fromEntries(["model","system","messages","tools","thinking"].filter(k=>b[k]!==undefined).map(k=>[k,b[k]]));
      const raw=await this.jsonRequest({...req,url:req.url+"/count_tokens",body},signal,false);return count(raw.input_tokens,"counted input tokens");
    }
    const raw=await this.jsonRequest({...req,url:req.url.replace(/:generateContent$/,":countTokens"),body:{generateContentRequest:{model:`models/${this.config.model}`,...b}}},signal,false);
    return count(raw.totalTokens,"counted input tokens");
  }
  private async jsonRequest(req:ReturnType<ProviderSession["request"]>,signal:AbortSignal,completion:boolean):Promise<Record<string,any>>{
    let response: Response;
    try { response = await this.fetcher(req.url, { method: "POST", headers: req.headers, body: JSON.stringify(req.body), signal, redirect: "error" }); }
    catch { throw new ProviderFailure(signal.aborted ? "request_interrupted" : "transport_error", "Provider response was not received; the request is not retried automatically.", completion); }
    if (!response.ok) {
      // Do not store response bodies: a gateway can echo authorization headers.
      throw new ProviderFailure(response.status === 429 ? "rate_limited" : "provider_http_error", `Provider returned HTTP ${response.status}; no automatic retry.`, completion&&response.status >= 500, response.status);
    }
    let raw: Record<string, any>;
    try {
      const reader = response.body?.getReader(); if (!reader) throw new Error("missing body");
      const chunks: Uint8Array[] = []; let length = 0;
      for (;;) { const item = await reader.read(); if (item.done) break; length += item.value.length; if (length > 4 * 1024 * 1024) { await reader.cancel(); throw new Error("response limit"); } chunks.push(item.value); }
      raw = JSON.parse(Buffer.concat(chunks).toString("utf8"));
    } catch { throw new ProviderFailure("malformed_response", "Provider response was invalid or exceeded the response limit.", completion); }
    return raw;
  }
  async next(maxOutput: number, signal: AbortSignal): Promise<ProviderTurn> {
    const raw=await this.jsonRequest(this.request(maxOutput),signal,true);
    const result = normalizeResponse(this.config.provider, raw);
    // Preserve complete output blocks, including opaque signatures and encrypted reasoning.
    if (this.config.provider === "openai") this.history.push(...raw.output);
    else if (this.config.provider === "anthropic") this.history.push({ role: "assistant", content: raw.content });
    else if (raw.candidates?.[0]?.content) this.history.push(raw.candidates[0].content);
    return result;
  }
}
export function cost(config: ModelConfig, usage: Pick<Usage,"inputTokens" | "outputTokens">): number | null {
  if (!config.pricing) return null;
  return Math.ceil((usage.inputTokens * config.pricing.inputUsdPerMillion + usage.outputTokens * config.pricing.outputUsdPerMillion) * 1000);
}
