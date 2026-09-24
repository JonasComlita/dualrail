import { spawn, spawnSync, type ChildProcessWithoutNullStreams } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import type { ModelConfig, Usage } from "../../src/ternary/contracts";
import { ProviderFailure, type EvaluationSession, type ProviderTurn, type ToolDefinition, type ToolResult } from "./providers";
import { canonical, invariant } from "./util";

const DISABLED_FEATURES=["apps","plugins","remote_plugin","hooks","memories","multi_agent","multi_agent_v2","shell_tool","unified_exec","browser_use","browser_use_external","computer_use","view_image","image_generation","workspace_dependencies","skill_search","skill_mcp_dependency_install","goals","sleep_tool","tool_suggest","code_mode","code_mode_host","unbounded_connection_retries"];
export const HARNESS_POLICY={
  transport:"codex-app-server-structured-actions-v1",reasoningEffort:"high",environmentAccess:false,
  inputPreflight:"unavailable; cumulative reported usage enforced after each turn",
  outputEnforcement:"cumulative reported usage and harness rollout budget; over-budget answers fail",
  cost:"unavailable for local subscription usage",hostRetries:false,
  harnessRetries:"unbounded retries disabled; any reported error or retry invalidates the episode",
};
let version:string|null|undefined;
export function harnessVersion(){
  if(version===undefined){const p=spawnSync("codex",["--version"],{windowsHide:true,encoding:"utf8",timeout:5000});version=p.status===0?p.stdout.trim():null;}
  return version;
}

/** A new environment-free ephemeral thread per episode, retaining context across rounds.
 * Participant actions are structured JSON, executed only by the benchmark host.
 * The model has no native filesystem, terminal, browsing, MCP, or delegation tools.
 */
export class HarnessSession implements EvaluationSession {
  private child:ChildProcessWithoutNullStreams|null=null;
  private workspace:string|null=null;
  private sequence=0;
  private pending=new Map<number,{resolve:(value:any)=>void;reject:(error:Error)=>void}>();
  private buffer="";
  private diagnostics="";
  private threadId:string|null=null;
  private turnId:string|null=null;
  private queued:string[]=[];
  private consumed:Usage={inputTokens:0,outputTokens:0,reasoningTokens:0,cachedInputTokens:0};
  private total:Usage|null=null;
  private responseItems:any[]=[];
  private responseModel:string|null=null;
  private finished:((value:ProviderTurn)=>void)|null=null;
  private failed:((error:Error)=>void)|null=null;
  private closed=false;
  private activeSignal:AbortSignal|null=null;
  private abortListener:(()=>void)|null=null;
  constructor(readonly config:ModelConfig,readonly system:string,readonly tools:ToolDefinition[]){}
  user(text:string){this.queued.push(text);}
  results(results:ToolResult[]){this.queued.push(`Results of your structured tool requests:\n${canonical(results)}`);}
  async countInput():Promise<null>{return null;}
  private send(message:unknown){if(!this.child||this.closed)throw new ProviderFailure("harness_unavailable","Local harness is closed.",false);this.child.stdin.write(JSON.stringify(message)+"\n");}
  private rpc(method:string,params:unknown):Promise<any>{const id=++this.sequence;return new Promise((resolve,reject)=>{this.pending.set(id,{resolve,reject});this.send({id,method,params});});}
  private fail(error:Error){
    this.failed?.(error);this.finished=null;this.failed=null;
    for(const p of this.pending.values())p.reject(error);this.pending.clear();
    this.close();
  }
  private consume(message:any){
    if(message.id!==undefined){
      if(message.method){this.send({id:message.id,error:{code:-32601,message:"Interactive and native tools are disabled for benchmark episodes."}});this.fail(new ProviderFailure("harness_tool_violation","Harness requested an unapproved native tool or interaction.",true));return;}
      const p=this.pending.get(message.id);if(p){this.pending.delete(message.id);message.error?p.reject(new ProviderFailure("harness_request_error",`Harness request failed: ${message.error.message}`,Boolean(this.turnId))):p.resolve(message.result);}return;
    }
    const p=message.params;
    if(message.method==="model/rerouted")return this.fail(new ProviderFailure("model_substitution","Harness rerouted the selected model; this episode is incomplete.",true));
    if(message.method==="error")return this.fail(new ProviderFailure("harness_inference_error","Harness reported an inference error. The episode is not retried.",true));
    if(message.method==="thread/tokenUsage/updated"&&p?.threadId===this.threadId){
      const u=p.tokenUsage?.total;
      if(!u||![u.inputTokens,u.outputTokens,u.reasoningOutputTokens,u.cachedInputTokens].every(n=>Number.isSafeInteger(n)&&n>=0))return this.fail(new ProviderFailure("missing_usage","Harness omitted valid token usage.",true));
      this.total={inputTokens:u.inputTokens,outputTokens:u.outputTokens,reasoningTokens:u.reasoningOutputTokens,cachedInputTokens:u.cachedInputTokens};
    }
    if(message.method==="item/started"&&p?.threadId===this.threadId&&!['userMessage','agentMessage','reasoning'].includes(p.item?.type))return this.fail(new ProviderFailure("harness_tool_violation","A native harness tool or context compaction was invoked outside the benchmark protocol.",true));
    if(message.method==="item/completed"&&p?.threadId===this.threadId)this.responseItems.push(p.item);
    if(message.method==="turn/completed"&&p?.threadId===this.threadId){
      if(p.turn?.status!=="completed")return this.fail(new ProviderFailure("harness_interrupted","Harness turn did not complete; it is not replayed.",true));
      if(!this.total)return this.fail(new ProviderFailure("missing_usage","Harness completed without usage evidence.",true));
      const usage:Usage={inputTokens:this.total.inputTokens-this.consumed.inputTokens,outputTokens:this.total.outputTokens-this.consumed.outputTokens,reasoningTokens:(this.total.reasoningTokens||0)-(this.consumed.reasoningTokens||0),cachedInputTokens:(this.total.cachedInputTokens||0)-(this.consumed.cachedInputTokens||0)};
      if(Object.values(usage).some(n=>n===null||n<0))return this.fail(new ProviderFailure("missing_usage","Harness cumulative usage moved backwards.",true));
      this.consumed={...this.total};
      const text=this.responseItems.filter(item=>item.type==="agentMessage"&&item.phase!=="commentary").map(item=>item.text).join("\n");
      let calls:ProviderTurn["calls"]=[];
      try{const value=JSON.parse(text);if(Array.isArray(value.tool_calls))calls=value.tool_calls.map((c:any,i:number)=>({id:`${p.turn.id}:${i}`,name:c.name,arguments:JSON.parse(c.arguments)}));}catch{/* Invalid final format is graded as an invalid submission. */}
      const turn:ProviderTurn={text,calls,status:calls.length?"tools":"complete",usage,responseId:p.turn.id,reportedModel:this.responseModel,raw:{harnessVersion:harnessVersion(),policy:HARNESS_POLICY,threadId:this.threadId,turn:p.turn,items:this.responseItems,usage,requestedModel:this.config.model,reportedModel:this.responseModel,requestedSettings:this.config.settings}};
      const resolve=this.finished;this.finished=null;this.failed=null;this.turnId=null;resolve?.(turn);
    }
  }
  private async start(){
    this.workspace=fs.mkdtempSync(path.join(os.tmpdir(),"ternary-harness-"));
    const args=["app-server","--stdio",...DISABLED_FEATURES.flatMap(f=>["--disable",f]),"-c","features.skip_host_skill_discovery=true","-c","mcp_servers={}","-c","web_search=\"disabled\"","-c","project_doc_max_bytes=0"];
    this.child=spawn("codex",args,{cwd:this.workspace,windowsHide:true,stdio:["pipe","pipe","pipe"]});
    this.child.on("error",()=>this.fail(new ProviderFailure("harness_unavailable","The local Codex harness could not start.",false)));
    this.child.on("exit",()=>{if(!this.closed){if(!this.threadId)fs.writeFileSync(path.join(import.meta.dir,"../../..","build/ternary-harness-startup-error.txt"),this.diagnostics);this.fail(new ProviderFailure("harness_interrupted","Local Codex harness exited unexpectedly.",Boolean(this.turnId)));}});
    this.child.stderr.on("data",chunk=>{this.diagnostics=(this.diagnostics+String(chunk)).slice(-8192);});
    this.child.stdout.on("data",chunk=>{this.buffer+=String(chunk);if(this.buffer.length>8*1024*1024)return this.fail(new ProviderFailure("harness_output_limit","Harness response exceeded its evidence limit.",true));while(this.buffer.includes("\n")){const end=this.buffer.indexOf("\n"),line=this.buffer.slice(0,end);this.buffer=this.buffer.slice(end+1);if(!line.trim())continue;try{this.consume(JSON.parse(line));}catch{return this.fail(new ProviderFailure("malformed_response","Harness returned malformed protocol evidence.",true));}}});
    await this.rpc("initialize",{clientInfo:{name:"ternary_intelligence",version:"1"},capabilities:{experimentalApi:true}});this.send({method:"initialized"});
    const account=await this.rpc("account/read",{refreshToken:false});invariant(account.account,"Sign in to the local Codex harness before evaluation.");
    const catalog=await this.rpc("model/list",{limit:100,includeHidden:true}),entry=catalog.data.find((m:any)=>m.model===this.config.model);
    invariant(entry&&entry.supportedReasoningEfforts.some((r:any)=>r.reasoningEffort===this.config.settings["reasoning.effort"]),"Requested model or reasoning effort is unavailable in this harness.");
    const contract=`${this.system}\nHarness response contract: return a JSON object with files and tool_calls. To submit, set files to {\"solution.trit\":\"complete source\"} and tool_calls to []. To request tools, set files to null and tool_calls to an array of {\"name\":\"tool name\",\"arguments\":\"JSON-encoded arguments\"}. Only these tools are available through the benchmark host: ${canonical(this.tools)}. All native harness tools are disabled.`;
    const started=await this.rpc("thread/start",{model:this.config.model,allowProviderModelFallback:false,baseInstructions:contract,developerInstructions:"",cwd:this.workspace,environments:[],runtimeWorkspaceRoots:[],selectedCapabilityRoots:[],dynamicTools:[],ephemeral:true,approvalPolicy:"never",sandbox:"read-only",config:{model_reasoning_effort:this.config.settings["reasoning.effort"],model_auto_compact_token_limit:1000000,"features.rollout_budget.enabled":true,"features.rollout_budget.limit_tokens":24000,"features.rollout_budget.reminder_at_remaining_tokens":[2000],"features.rollout_budget.prefill_token_weight":0,"features.rollout_budget.sampling_token_weight":1},threadSource:"ternary-benchmark"});
    invariant(started.model===this.config.model&&started.reasoningEffort===this.config.settings["reasoning.effort"],"Harness changed the requested model or reasoning setting.");
    invariant(!started.instructionSources?.length,"Harness loaded external instructions; benchmark context is not isolated.");
    this.threadId=started.thread.id;this.responseModel=started.model;
  }
  async next(_maxOutput:number,signal:AbortSignal):Promise<ProviderTurn>{
    if(signal.aborted)throw new ProviderFailure("request_interrupted","Harness request was interrupted.",false);
    this.activeSignal=signal;this.abortListener=()=>this.fail(new ProviderFailure("request_interrupted","Harness request was interrupted and will not be retried.",true));signal.addEventListener("abort",this.abortListener,{once:true});
    try{
      if(!this.child)await this.start();
      this.responseItems=[];this.total=null;
      const result=new Promise<ProviderTurn>((resolve,reject)=>{this.finished=resolve;this.failed=reject;});
      const outputSchema={type:"object",additionalProperties:false,properties:{files:{anyOf:[{type:"null"},{type:"object",additionalProperties:false,properties:{"solution.trit":{type:"string"}},required:["solution.trit"]}]},tool_calls:{type:"array",maxItems:64,items:{type:"object",additionalProperties:false,properties:{name:{type:"string",enum:this.tools.length?this.tools.map(t=>t.name):["unavailable"]},arguments:{type:"string"}},required:["name","arguments"]}}},required:["files","tool_calls"]};
      const started=await this.rpc("turn/start",{threadId:this.threadId,input:[{type:"text",text:this.queued.splice(0).join("\n\n")}],model:this.config.model,effort:this.config.settings["reasoning.effort"],environments:[],outputSchema});this.turnId=started.turn.id;
      return await result;
    }catch(error){this.close();throw error;}
    finally{if(this.abortListener)signal.removeEventListener("abort",this.abortListener);this.abortListener=null;this.activeSignal=null;}
  }
  close(){
    if(this.closed)return;this.closed=true;
    const cleanup=()=>{if(!this.workspace)return;const target=path.resolve(this.workspace);if(path.dirname(target)===path.resolve(os.tmpdir())&&path.basename(target).startsWith("ternary-harness-"))fs.rm(target,{recursive:true,force:true,maxRetries:10,retryDelay:100},()=>{});};
    if(this.child&&this.child.exitCode===null){this.child.once("close",cleanup);this.child.stdin.end();this.child.kill();}else cleanup();
  }
}
