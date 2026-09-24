/** Local-only browser harness. It has no live provider transport or publishable scores. */
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import express from "express";
import { AuthStore, bearerToken, DEFAULT_PROJECT_ID } from "../src/auth";
import { IntelligenceLab } from "../private/ternary/service";
import { LabStore } from "../private/ternary/store";
import { intelligenceRouter } from "../private/ternary/routes";
import { ProviderSession } from "../private/ternary/providers";
import { corpus } from "../private/ternary/corpus";
import type { ModelConfig } from "../src/ternary/contracts";
const repo=path.resolve(import.meta.dir,"../.."),root=fs.mkdtempSync(path.join(os.tmpdir(),"ternary-browser-fixture-"));
const auth=new AuthStore({identity_path:path.join(root,"auth.json"),audit_path:path.join(root,"audit.jsonl")});
auth.registerIdentity({id:"tc:identity:browser-fixture",kind:"human",handle:"benchmark_browser",display_name:"Local fixture operator",roles:["benchmark-operator"],access_key:"fixture-only-local-login",grants:[{project_id:"*",actions:["read","edit","benchmark","artifact"]}]});
const models:ModelConfig[]=(["openai","anthropic","google"] as const).map(provider=>({id:`fixture-${provider}`,provider,model:`fixture-only-${provider}`,keyEnv:`TI_BROWSER_FIXTURE_${provider.toUpperCase()}`,settings:{},supportedSettings:{},maxOutputTokens:1024,pricing:null}));
models.forEach(m=>process.env[m.keyEnv]="not-a-live-provider-key");
const lab=new IntelligenceLab(repo,root,{sessionFactory:(model,system,tools)=>new ProviderSession(model,system,tools,(async(url,init)=>{
  if(String(url).includes("input_tokens")||String(url).includes("count_tokens"))return Response.json({input_tokens:100});
  if(String(url).includes(":countTokens"))return Response.json({totalTokens:100});
  const body=JSON.parse(init!.body as string);
  const prompt=model.provider==="openai"?body.input?.[0]?.content:model.provider==="anthropic"?body.messages?.[0]?.content:body.contents?.[0]?.parts?.[0]?.text;
  const task=corpus().find(t=>String(prompt).startsWith(t.title));
  const compatibilityInteger=String(prompt).match(/returns the integer (\d+)/)?.[1];
  const text=task?JSON.stringify({files:{"solution.trit":task.reference}}):JSON.stringify({files:{"solution.trit":`fn solve(a:t40,b:t40,c:t40)->t40{return ${compatibilityInteger||0};}`}});
  // Keep this deterministic response isolated from all real provider networks.
  if(model.provider==="openai")return Response.json({id:crypto.randomUUID(),model:model.model,status:"completed",output:[{type:"message",content:[{type:"output_text",text}]}],usage:{input_tokens:100,output_tokens:200}});
  if(model.provider==="anthropic")return Response.json({id:crypto.randomUUID(),model:model.model,content:[{type:"text",text}],stop_reason:"end_turn",usage:{input_tokens:100,output_tokens:200}});
  return Response.json({responseId:crypto.randomUUID(),modelVersion:model.model,candidates:[{content:{role:"model",parts:[{text}]},finishReason:"STOP"}],usageMetadata:{promptTokenCount:100,candidatesTokenCount:200,thoughtsTokenCount:0}});
}) as typeof fetch)});
// Reuse actual executable qualification evidence for the same corpus/toolchain.
// Fixtures never supply or fabricate qualification outcomes.
const qualified=new LabStore(path.join(repo,"build/ternary-intelligence"));
try{lab.store.setSetting("qualification",qualified.setting("qualification"));}finally{qualified.close();}
lab.configure({models,calibrationModelIds:models.map(m=>m.id),calibrationTiers:{"fixture-openai":"lower","fixture-anthropic":"middle","fixture-google":"higher"}});
for(const model of models)await lab.verifyModel(model.id);
const app=express();app.use(express.json({limit:"1mb"}));
app.post("/api/auth/v1/participants/login",(req,res)=>{const result=auth.login(req.body);res.status(result.ok?200:401).json(result.ok?{data:result}:{error:{reason:"Invalid fixture login"}});});
app.get("/api/auth/v1/capabilities",(req,res)=>{const result=auth.capabilities({token:bearerToken(req.header("authorization")),project_id:DEFAULT_PROJECT_ID});res.status(result.ok?200:403).json({data:result});});
app.use("/api/ternary-intelligence/v1",intelligenceRouter(lab,auth));
const dist=path.join(repo,"treatcode/dist");app.get(["/intelligence","/intelligence/*"],(_req,res)=>res.sendFile(path.join(dist,"intelligence/index.html")));app.get("/account",(_req,res)=>res.sendFile(path.join(dist,"account/index.html")));app.use(express.static(dist));
const port=Number(process.env.TI_FIXTURE_PORT||4352);app.listen(port,"127.0.0.1",()=>console.log(`LOCAL FIXTURE ONLY: http://localhost:${port}/account · benchmark_browser / fixture-only-local-login`));
process.once("exit",()=>lab.close());
