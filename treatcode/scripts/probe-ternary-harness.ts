/** Read-only harness discovery. No model turn, task, or inference request is created. */
import { spawn, spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
const requested=["gpt-6-astra","gpt-5.6-sol","gpt-5.6-terra","gpt-5.6-luna"];
const child=spawn("codex",["app-server","--stdio","-c","features.apps=false","-c","features.remote_plugin=false"],{windowsHide:true,stdio:["pipe","pipe","pipe"]});
let sequence=0,buffer="",diagnosticBytes=0;
const pending=new Map<number,{resolve:(value:any)=>void;reject:(error:Error)=>void}>();
const stop=(reason:string)=>{for(const request of pending.values())request.reject(new Error(reason));pending.clear();child.kill();};
const timer=setTimeout(()=>stop("Harness discovery timed out."),30000);
child.stderr.on("data",chunk=>diagnosticBytes+=chunk.length);
child.on("error",()=>stop("The local Codex harness could not be started."));
child.on("exit",()=>{for(const request of pending.values())request.reject(new Error("Harness stopped during discovery."));pending.clear();});
const send=(message:unknown)=>child.stdin.write(JSON.stringify(message)+"\n");
child.stdout.on("data",chunk=>{
  buffer+=String(chunk);
  while(buffer.includes("\n")){
    const end=buffer.indexOf("\n"),line=buffer.slice(0,end);buffer=buffer.slice(end+1);
    if(!line.trim())continue;let message:any;try{message=JSON.parse(line);}catch{continue;}
    if(message.method&&message.id!==undefined){send({id:message.id,error:{code:-32601,message:"Read-only discovery does not handle interactive requests."}});continue;}
    const handler=pending.get(message.id);if(!handler)continue;pending.delete(message.id);
    if(message.error)handler.reject(new Error(`Harness rejected discovery: ${message.error.message}`));else handler.resolve(message.result);
  }
});
function rpc(method:string,params:unknown){const id=++sequence;return new Promise<any>((resolve,reject)=>{pending.set(id,{resolve,reject});send({id,method,params});});}
try{
  await rpc("initialize",{clientInfo:{name:"ternary_benchmark_discovery",version:"1"},capabilities:{experimentalApi:true}});send({method:"initialized"});
  const catalog=await rpc("model/list",{limit:100,includeHidden:true});
  const account=await rpc("account/read",{refreshToken:false});
  const report={at:new Date().toISOString(),version:spawnSync("codex",["--version"],{windowsHide:true,encoding:"utf8"}).stdout.trim(),signedIn:Boolean(account.account),authType:account.account?.type||null,
    models:requested.map(id=>{const entry=catalog.data.find((m:any)=>m.model===id);return {requested:id,available:Boolean(entry),defaultReasoningEffort:entry?.defaultReasoningEffort||null,supportedReasoningEfforts:entry?.supportedReasoningEfforts?.map((r:any)=>r.reasoningEffort)||[]};}),
    inferenceRequests:0,diagnosticBytes};
  const destination=path.resolve(import.meta.dir,"../../build/ternary-intelligence/harness-discovery.json");fs.mkdirSync(path.dirname(destination),{recursive:true});fs.writeFileSync(destination,JSON.stringify(report,null,2));console.log(JSON.stringify(report,null,2));
}finally{clearTimeout(timer);child.stdin.end();child.kill();}
