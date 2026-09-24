import fs from "node:fs";
import path from "node:path";
import { randomUUID } from "node:crypto";
import { CAPABILITIES, DEFAULT_LIMITS, PILOT_LABEL, type Assignment, type EpisodeView, type Limits, type ModelConfig, type Observation, type Protocol, type Publication, type Resources, type RunView } from "../../src/ternary/contracts";
import { corpus, family, LANGUAGE_REFERENCE, REVISION, type TaskPackage } from "./corpus";
import { LabStore } from "./store";
import { WindowsWorker, type Case } from "./worker";
import { ProviderFailure, ProviderSession, cost, validateModel, type EvaluationSession, type ToolDefinition, type ToolResult } from "./providers";
import { HarnessSession, harnessVersion, HARNESS_POLICY } from "./harness";
import { HARNESS_LINEUP, HARNESS_TIERS } from "../../src/ternary/lineup";
import { qualify, qualificationFailures, type QualificationReport } from "./qualification";
import { calibrationSummary, scoreRows } from "./reporting";
import { canonical, hash, integer, invariant, LabError, now } from "./util";

const SYSTEM = "You are completing one independent ternary intelligence evaluation. Follow the task's explicit semantics. Submit only the requested artifact. Explanatory prose is not graded. For a final textual submission return exactly a JSON object with files: {\"solution.trit\": \"complete source\"}. If tools are offered, you may edit solution.trit and call submit_answer. You have no general terminal or network tool.";
const EXECUTION_CONTRACT = "Execution contract: solution.trit must define solve(a:t40,b:t40,c:t40)->t40. Preserve or replace the supplied helpers; additional helper names must begin with local_. Only numeric functions are accepted: no imports, host calls, strings, assembly, main, or reserved __ names. The grader supplies main. Use exact integer behavior; all declared inputs and correct intermediate values fit t40.";
const TOOLS:ToolDefinition[]=[
  {name:"list_files",description:"List the participant files.",parameters:{type:"object",properties:{},additionalProperties:false}},
  {name:"read_file",description:"Read a participant file.",parameters:{type:"object",properties:{path:{type:"string"}},required:["path"],additionalProperties:false}},
  {name:"replace_file",description:"Replace the entire allowlisted solution.trit file.",parameters:{type:"object",properties:{path:{type:"string"},content:{type:"string"}},required:["path","content"],additionalProperties:false}},
  {name:"run_public_tests",description:"Run the named public test set against your current solution.",parameters:{type:"object",properties:{name:{type:"string",enum:["public"]}},required:["name"],additionalProperties:false}},
  {name:"submit_answer",description:"Submit the current solution file for this stage.",parameters:{type:"object",properties:{},additionalProperties:false}},
];
interface ModelSettings { models:ModelConfig[]; calibrationModelIds:string[]; calibrationTiers:Record<string,"lower"|"middle"|"higher"> }
interface ModelVerification { configHash:string; provenance:"live"|"fixture"; at:string; state:"running"|"passed"|"failed"; failures:string[]; reportedModels:string[]; requestCount:number; usage:{inputTokens:number;outputTokens:number}; costNanoUsd:number|null; artifactHashes:string[] }
interface AmbiguityReview { revision:string; baselineRunId:string; controlledRunId:string; at:string; owner:string; families:Array<{familyId:string;ambiguous:boolean;notes:string}> }
export interface CreateInput { modelIds:string[]; experiment:"default"|"controlled"; purpose:"calibration"|"evaluation"; spendingCeilingUsd:number|null; limits?:Partial<Limits> }
const TERMINAL = ["completed","cancelled","interrupted"];
const emptyResources=():Resources=>({inputTokens:0,outputTokens:0,reasoningTokens:null,cachedInputTokens:null,toolCalls:0,publicTestCalls:0,modelMs:0,programMs:null,peakMemoryBytes:null,costNanoUsd:null});

export class IntelligenceLab {
  readonly store:LabStore;
  readonly worker:WindowsWorker;
  private pumping=false;
  private qualifying=false;
  private verifying=false;
  private controller:AbortController|null=null;
  private activeRun:string|null=null;
  private stopped=false;
  private released=false;
  private readonly lockPath:string;
  private readonly bootId=randomUUID();
  readonly provenance:"live"|"fixture";
  constructor(readonly repo:string,readonly root:string,private options:{sessionFactory?:(config:ModelConfig,system:string,tools:ToolDefinition[])=>EvaluationSession; autoStart?:boolean}={}){
    const realPath=(value:string):string=>{const absolute=path.resolve(value);if(fs.existsSync(absolute))return fs.realpathSync.native(absolute).toLowerCase();return path.join(realPath(path.dirname(absolute)),path.basename(absolute)).toLowerCase();};
    const resolved=realPath(root);
    for(const exposed of ["public","dist","src","learn"]){const prefix=realPath(path.join(repo,"treatcode",exposed));invariant(resolved!==prefix&&!resolved.startsWith(prefix+path.sep),"Private storage must be outside website source and public asset directories.");}
    this.store=new LabStore(root);this.worker=new WindowsWorker(repo,path.join(root,"grading"));
    this.provenance=options.sessionFactory?"fixture":"live";
    this.lockPath=path.join(root,"worker.lock");
    if(fs.existsSync(this.lockPath)){
      let lock:{pid:number}|null=null;try{lock=JSON.parse(fs.readFileSync(this.lockPath,"utf8"));}catch{}
      let live=false;if(lock?.pid){try{process.kill(lock.pid,0);live=true;}catch(error){live=(error as NodeJS.ErrnoException).code!=="ESRCH";}}
      if(live){this.store.close();throw new LabError("worker_in_use","Another live server owns this benchmark store.",503);}
      fs.unlinkSync(this.lockPath);
    }
    fs.writeFileSync(this.lockPath,canonical({pid:process.pid,bootId:this.bootId}),{flag:"wx"});
    this.store.recover();
    if(options.autoStart!==false)setImmediate(()=>void this.pump());
  }
  close(){this.stopped=true;this.controller?.abort();if(!this.pumping&&!this.verifying&&!this.qualifying)this.release();}
  private release(){if(this.released)return;this.released=true;try{const lock=JSON.parse(fs.readFileSync(this.lockPath,"utf8"));if(lock.bootId===this.bootId)fs.unlinkSync(this.lockPath);}catch{}this.store.close();}
  settings():ModelSettings{return this.store.setting<ModelSettings>("model-settings")||{models:HARNESS_LINEUP,calibrationModelIds:HARNESS_LINEUP.map(m=>m.id),calibrationTiers:HARNESS_TIERS};}
  private credentialConfigured(m:ModelConfig){return m.provider==="codex"?Boolean(harnessVersion()):Boolean(process.env[m.keyEnv]);}
  private session(model:ModelConfig,system:string,tools:ToolDefinition[]):EvaluationSession{return this.options.sessionFactory?this.options.sessionFactory(model,system,tools):model.provider==="codex"?new HarnessSession(model,system,tools):new ProviderSession(model,system,tools);}
  configure(value:ModelSettings){
    invariant(!this.verifying&&!this.qualifying,"Finish verification or qualification before editing configuration.");
    invariant(Array.isArray(value.models)&&value.models.length<=20,"Configure at most 20 models.");
    value.models.forEach(validateModel);invariant(new Set(value.models.map(m=>m.id)).size===value.models.length,"Model IDs must be unique.");
    invariant(Array.isArray(value.calibrationModelIds)&&value.calibrationModelIds.every(id=>value.models.some(m=>m.id===id)),"Unknown calibration configuration.");
    invariant(value.calibrationTiers&&typeof value.calibrationTiers==="object","Declare preselected calibration tiers.");
    invariant(!this.store.runs().some(r=>!TERMINAL.includes(r.state)),"Finish or cancel active runs before changing configuration.");
    // A new preselection invalidates the old calibration/freeze, rather than selecting winners afterward.
    if(hash(value)!==hash(this.settings()))this.store.setSetting("freeze",null);
    this.store.setSetting("model-settings",value);
  }
  async qualify(progress?:(message:string)=>void){
    invariant(!this.qualifying&&!this.verifying&&!this.pumping&&!this.store.runs().some(r=>["queued","running"].includes(r.state)),"Finish active work before qualification.");
    this.qualifying=true;
    try{return await qualify(this.store,this.worker,progress);}finally{this.qualifying=false;if(this.stopped)this.release();}
  }
  private verification(m:ModelConfig){
    const v=this.store.setting<ModelVerification>(`model-verification:${m.id}`);
    return v&&v.configHash===this.verificationHash(m)&&v.provenance===this.provenance?v:null;
  }
  private verificationHash(m:ModelConfig){return hash({configuration:m,orchestration:fs.readFileSync(path.join(import.meta.dir,"service.ts"),"utf8"),adapter:fs.readFileSync(path.join(import.meta.dir,"providers.ts"),"utf8"),...(m.provider==="codex"?{harness:fs.readFileSync(path.join(import.meta.dir,"harness.ts"),"utf8"),version:harnessVersion()}: {})});}
  reviewAmbiguity(input:{families:AmbiguityReview["families"]},owner:string){
    const calibration=this.calibration();invariant(calibration.baselineRunId&&calibration.controlledRunId,"Complete baseline and controlled calibration before ambiguity review.");
    invariant(Array.isArray(input.families)&&input.families.length===36,"Review all 36 families.");
    const ids=new Set<string>();
    for(const row of input.families){invariant(corpus().some(f=>f.id===row.familyId)&&!ids.has(row.familyId),"Review every family exactly once.");ids.add(row.familyId);invariant(typeof row.ambiguous==="boolean"&&typeof row.notes==="string"&&row.notes.trim().length>=10&&row.notes.length<=4000,"Each family needs an ambiguity decision and a substantive review note.");}
    const review:AmbiguityReview={revision:REVISION,baselineRunId:calibration.baselineRunId,controlledRunId:calibration.controlledRunId,at:now(),owner,families:input.families};
    this.store.setSetting("ambiguity-review",review);this.store.putArtifact(review);return this.calibration();
  }
  async verifyModel(id:string){
    invariant(!this.qualifying&&!this.verifying&&!this.pumping&&!this.store.runs().some(r=>["queued","running"].includes(r.state)),"Finish active work before provider verification.");
    const model=this.settings().models.find(m=>m.id===id);invariant(model,"Unknown model configuration.");validateModel(model);
    invariant(this.credentialConfigured(model),"Configure the provider credential or install and sign in to the local Codex harness first.");
    const report:ModelVerification={configHash:this.verificationHash(model),provenance:this.provenance,at:now(),state:"running",failures:[],reportedModels:[],requestCount:0,usage:{inputTokens:0,outputTokens:0},costNanoUsd:model.pricing?0:null,artifactHashes:[]};
    this.verifying=true;this.store.setSetting(`model-verification:${id}`,report);
    const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),DEFAULT_LIMITS.episodeMs);
    this.controller=controller;
    try{
      // Explicit operator action: two short, non-scored requests, with the exact
      // configured settings and output cap, before any private task is exposed.
      for(const tools of [[],TOOLS]){
        const session=this.session(model,SYSTEM,tools);
        try{
        const expected=tools.length?53:37;
        session.user(`Non-scored compatibility task: submit solution.trit defining fn solve(a:t40,b:t40,c:t40)->t40 that returns the integer ${expected} for every input. Use this Trit syntax, replacing CHECK_VALUE with that integer: fn solve(a:t40,b:t40,c:t40)->t40 { return CHECK_VALUE; } Return the final files object directly without calling tools. ${EXECUTION_CONTRACT}`);
        const input=await session.countInput(model.maxOutputTokens,controller.signal);
        invariant(input===null||input<=DEFAULT_LIMITS.inputTokens,"Compatibility input exceeds the pilot input budget.");
        report.requestCount++;this.store.setSetting(`model-verification:${id}`,report);
        let result;
        try{result=await session.next(model.maxOutputTokens,controller.signal);}catch(error){report.costNanoUsd=null;throw error;}
        report.usage.inputTokens+=result.usage.inputTokens;report.usage.outputTokens+=result.usage.outputTokens;
        if(report.costNanoUsd!==null)report.costNanoUsd+=cost(model,result.usage)||0;
        if(result.reportedModel&&!report.reportedModels.includes(result.reportedModel))report.reportedModels.push(result.reportedModel);
        report.artifactHashes.push(this.store.putArtifact({requestedModel:model.model,requestedSettings:model.settings,tools:tools.length>0,countedInputTokens:input,response:result.raw}));
        invariant(result.status==="complete"&&result.text.trim().length>0,"Provider did not finish the compatibility response with these settings and output cap.");
        let source:unknown;try{source=JSON.parse(result.text).files?.["solution.trit"];}catch{}
        invariant(typeof source==="string","Compatibility response did not submit solution.trit in the required files object.");
        const check=await this.worker.grade(source,[{id:"public-compatibility",args:[1,2,3],expected}],DEFAULT_LIMITS,controller.signal);
        report.artifactHashes.push(this.store.putArtifact({compatibilityExpected:expected,check}));
        invariant(check.passed,"Compatibility submission did not execute with the requested behavior.");
        }finally{session.close?.();}
      }
      report.state="passed";
    }catch(error){report.state="failed";report.failures.push(error instanceof ProviderFailure?`${error.code}: ${error.message}`:(error as Error).message);}
    finally{clearTimeout(timer);this.verifying=false;this.controller=null;this.store.setSetting(`model-verification:${id}`,report);this.store.putArtifact(report);if(this.stopped)this.release();}
    return report;
  }
  readiness(){
    const settings=this.settings(),worker=this.worker.readiness(),qualification=qualificationFailures(this.store,this.worker);
    return {label:PILOT_LABEL,revision:REVISION,worker,qualificationFailures:qualification,
      qualifiedFamilies:this.store.setting<QualificationReport>("qualification")?.families.filter(f=>f.passed).length||0,
      models:settings.models.map(m=>({id:m.id,provider:m.provider,model:m.model,settings:m.settings,maxOutputTokens:m.maxOutputTokens,pricing:m.pricing,
        credentialConfigured:this.credentialConfigured(m),verification:this.verification(m),ready:this.credentialConfigured(m)&&this.verification(m)?.state==="passed",errors:!this.credentialConfigured(m)?["Provider credential is not configured on the server."]:this.verification(m)?.state==="passed"?[]:["Verify this exact configuration against the provider before evaluation."]})),
      calibrationModelIds:settings.calibrationModelIds,calibrationTiers:settings.calibrationTiers,calibration:this.calibration(),freeze:this.store.setting("freeze"),
      spendingPolicy:"Correctness is independent of spending. A dollar ceiling is optional; pricing is required only when enforcing a ceiling."};
  }
  publicOverview(){
    const q=this.store.setting<QualificationReport>("qualification"),failures=qualificationFailures(this.store,this.worker);
    return {label:PILOT_LABEL,revision:REVISION,capabilities:CAPABILITIES,familyCount:36,qualifiedFamilies:q?.families.filter(f=>f.passed).length||0,
      validation:failures.length?"qualification-pending":"qualified",frozen:Boolean(this.store.setting("freeze")),latestPublication:this.store.publications()[0]||null};
  }
  protocol(input:CreateInput):Protocol{
    const limits={...DEFAULT_LIMITS,...input.limits};
    for(const key of Object.keys(DEFAULT_LIMITS) as (keyof Limits)[])integer(limits[key],1,DEFAULT_LIMITS[key],key);
    invariant(limits.memoryMiB>=8,"Worker memory must be at least 8 MiB.");
    const tasks=corpus(),controlled=CAPABILITIES.flatMap(c=>[`${c.id}-01`,`${c.id}-04`]),assignments:Assignment[]=[];
    for(const task of tasks){
      if(input.experiment==="controlled"&&!controlled.includes(task.id))continue;
      for(const condition of input.experiment==="controlled"?["prior","specification","learning"] as const:["specification"] as const)
        for(const mode of input.experiment==="controlled"?["model-only","tools"] as const:["tools"] as const)
          for(let repeat=0;repeat<(input.purpose==="calibration"?3:1);repeat++)assignments.push({familyId:task.id,condition,mode,repeat,seed:parseInt(hash(`${REVISION}:${task.id}:${repeat}`).slice(0,8),16)});
    }
    const ready=this.worker.readiness();
    const settings=this.settings(),ids=input.modelIds.length?input.modelIds:settings.calibrationModelIds;
    const transport=ids.some(id=>settings.models.some(m=>m.id===id&&m.provider==="codex"))?"codex-harness":"provider-api";
    const harness=transport==="codex-harness"?{version:harnessVersion()||"unavailable",policy:HARNESS_POLICY}:undefined;
    return {transport,...(harness?{harness}:{}),revision:REVISION,profile:hash({limits,transport,harness}),limits,familyHashes:Object.fromEntries(tasks.map(t=>[t.id,t.packageHash])),checkerHash:tasks[0].checkerHash,
      workerHash:ready.workerHash||"unavailable",compilerHash:ready.compilerHash||"unavailable",promptHash:hash({system:SYSTEM,reference:LANGUAGE_REFERENCE,tools:TOOLS,orchestration:fs.readFileSync(path.join(import.meta.dir,"service.ts"),"utf8"),adapters:fs.readFileSync(path.join(import.meta.dir,"providers.ts"),"utf8"),harness:fs.readFileSync(path.join(import.meta.dir,"harness.ts"),"utf8")}),
      controlledFamilies:controlled,assignments,purpose:input.purpose,experiment:input.experiment,bootstrapSamples:10000,label:PILOT_LABEL};
  }
  preview(input:CreateInput){
    invariant(!this.qualifying&&!this.verifying,"Wait for qualification or provider verification to finish.");
    invariant(input.spendingCeilingUsd===null||Number.isFinite(input.spendingCeilingUsd)&&input.spendingCeilingUsd>0&&input.spendingCeilingUsd<=100000,"Invalid optional spending ceiling.");
    invariant(input.experiment==="default"||input.experiment==="controlled","Unknown experiment.");
    invariant(input.purpose==="calibration"||input.purpose==="evaluation","Unknown run purpose.");
    invariant(Array.isArray(input.modelIds)&&input.modelIds.length>0&&new Set(input.modelIds).size===input.modelIds.length,"Select unique configured models.");
    const settings=this.settings(),models=input.modelIds.map(id=>{const m=settings.models.find(x=>x.id===id);invariant(m,"Select a configured model.");return m;});
    const protocol=this.protocol(input),failures=[...this.worker.readiness().errors,...qualificationFailures(this.store,this.worker)];
    if(models.some(m=>m.provider==="codex")&&models.some(m=>m.provider!=="codex"))failures.push("Harness and direct API configurations require separate protocol profiles and runs.");
    for(const m of models){validateModel(m);if(!this.credentialConfigured(m))failures.push(`${m.id}: provider credential missing.`);if(this.verification(m)?.state!=="passed")failures.push(`${m.id}: verify the exact model and supported settings against the provider first.`);if(input.spendingCeilingUsd!==null&&!m.pricing)failures.push(`${m.id}: a pricing snapshot is required to enforce a spending ceiling.`);}
    if(input.purpose==="calibration"){
      if(canonical([...input.modelIds].sort())!==canonical([...settings.calibrationModelIds].sort())||models.length<2)failures.push("Calibration requires every preselected configuration, with at least two configurations.");
      if(new Set(input.modelIds.map(id=>settings.calibrationTiers[id])).size!==3||!input.modelIds.every(id=>["lower","middle","higher"].includes(settings.calibrationTiers[id])))failures.push("Preselect one lower, middle, and higher capability configuration before calibration.");
    }else{
      if(!this.matchesFreeze(protocol))failures.push("Accept calibration and freeze this exact task, prompt, toolchain, assignment, and budget profile before scored evaluation.");
    }
    return {protocol,models,episodes:protocol.assignments.length*models.length,failures,
      maximumCostUsd:models.every(m=>m.pricing)?models.reduce((s,m)=>s+(cost(m,{inputTokens:protocol.limits.inputTokens,outputTokens:protocol.limits.outputTokens})||0)*protocol.assignments.length,0)/1e9:null};
  }
  create(input:CreateInput,owner:string,requestKey:string){
    const preview=this.preview(input);invariant(!preview.failures.length,preview.failures.join(" "));
    const run=this.store.createRun({protocol:preview.protocol,models:preview.models,ceilingUsd:input.spendingCeilingUsd,owner,requestKey,provenance:this.provenance});
    for(const task of corpus())this.store.putArtifact(task);
    if(this.options.autoStart!==false)setImmediate(()=>void this.pump());return this.detail(run.id);
  }
  detail(id:string){const run=this.store.run(id);return {...run,publicationFailures:this.publicationFailures(run),episodes:this.store.episodes(id)};}
  cancel(id:string){
    const run=this.store.run(id);if(TERMINAL.includes(run.state))return this.detail(id);
    this.store.state(id,"cancelled");if(this.activeRun===id)this.controller?.abort();
    for(const e of this.store.episodes(id))if(e.state==="pending")this.store.finishEpisode(e.id,this.observation(e,"cancelled",null,"operator_cancelled",emptyResources()));
    this.store.event(id,null,"run.cancelled",{});return this.detail(id);
  }
  private observation(e:EpisodeView,state:Observation["state"],success:boolean|null,category:string,resources:Resources):Observation{
    return {familyId:e.assignment.familyId,modelConfigId:e.modelConfigId,condition:e.assignment.condition,mode:e.assignment.mode,repeat:e.assignment.repeat,state,success,category,resources,learning:[],robustness:null};
  }
  async pump(){
    if(this.pumping||this.stopped)return;this.pumping=true;
    try{
      for(const run of this.store.runs().reverse()){
        if(this.stopped)break;if(!["queued","running"].includes(run.state))continue;
        this.store.state(run.id,"running");this.activeRun=run.id;
        for(const e of this.store.episodes(run.id)){
          if(this.stopped||this.store.run(run.id).state==="cancelled")break;
          if(e.state!=="pending"||!this.store.startEpisode(e.id))continue;
          this.controller=new AbortController();
          this.store.event(run.id,e.id,"episode.started",{assignment:e.assignment,modelConfigId:e.modelConfigId});
          const observation=await this.evaluate(run,e,this.controller);
          this.store.finishEpisode(e.id,observation);this.store.event(run.id,e.id,"episode.finished",observation);
          if(observation.state==="infrastructure_error"&&["pricing_contract","spending_limit"].includes(observation.category))this.cancel(run.id);
        }
        const state=this.store.run(run.id).state;
        if(state!=="cancelled")this.store.state(run.id,this.stopped?"queued":"completed");
        this.store.event(run.id,null,"run.finished",{state:this.store.run(run.id).state});
      }
    }catch(error){
      if(this.activeRun){this.store.event(this.activeRun,null,"run.infrastructure_error",{message:"The worker loop stopped unexpectedly; undispatched work is preserved."});this.store.state(this.activeRun,"interrupted");}
    }finally{this.pumping=false;this.activeRun=null;this.controller=null;if(this.stopped)this.release();else if(this.options.autoStart!==false&&this.store.runs().some(r=>r.state==="queued"))setImmediate(()=>void this.pump());}
  }
  private async evaluate(run:RunView,e:EpisodeView,controller:AbortController):Promise<Observation>{
    const task=family(e.assignment.familyId),config=run.models.find(m=>m.id===e.modelConfigId)!,resources=emptyResources(),limits=run.protocol.limits;
    resources.costNanoUsd=config.pricing?0:null;
    const timer=setTimeout(()=>controller.abort("episode_deadline"),limits.episodeMs),began=Date.now();
    const obs=this.observation(e,"failed",false,"invalid_submission",resources);
    let source=task.starter["solution.trit"];
    let reasoningKnown=true,cachedKnown=true;
    const tools=e.assignment.mode==="tools"?TOOLS:[];
    const session=this.session(config,SYSTEM,tools);
    const record=(type:string,data:unknown)=>this.store.event(run.id,e.id,type,data);
    const grade=async(cases:Case[])=>{
      const ordered=[...cases].sort((a,b)=>hash(`${e.assignment.seed}:${a.id}`).localeCompare(hash(`${e.assignment.seed}:${b.id}`)));
      const result=await this.worker.grade(source,ordered,{...limits,commandMs:Math.max(1,Math.min(limits.commandMs,limits.episodeMs-(Date.now()-began)))},controller.signal);
      resources.programMs=(resources.programMs||0)+result.programMs;resources.peakMemoryBytes=Math.max(resources.peakMemoryBytes||0,result.peakMemoryBytes);
      record("grader.result",{cases:cases.map(c=>c.id),evidenceHash:this.store.putArtifact(result)});return result;
    };
    const setSource=(value:unknown)=>{invariant(typeof value==="string"&&Buffer.byteLength(value)<=65536,"Only source text up to 64 KiB is accepted.");const before=hash(source);source=value;record("file.changed",{path:"solution.trit",before,after:hash(source),artifactHash:this.store.putArtifact({"solution.trit":source})});};
    try{
      if(run.protocol.familyHashes[task.id]!==task.packageHash)throw new LabError("frozen_mismatch","Frozen task package changed; the run cannot continue.",409);
      const currentProtocol=this.protocol({modelIds:run.models.map(m=>m.id),purpose:run.protocol.purpose,experiment:run.protocol.experiment,spendingCeilingUsd:null,limits:run.protocol.limits});
      if(run.protocol.promptHash!==currentProtocol.promptHash)throw new LabError("frozen_mismatch","Frozen prompt, orchestration, or provider adapter changed; the run cannot continue.",409);
      if(hash(run.protocol.harness||null)!==hash(currentProtocol.harness||null))throw new LabError("frozen_mismatch","Frozen harness version or execution policy changed; the run cannot continue.",409);
      const ready=this.worker.readiness();if(ready.workerHash!==run.protocol.workerHash||ready.compilerHash!==run.protocol.compilerHash)throw new LabError("frozen_mismatch","Frozen worker/compiler changed; the run cannot continue.",409);
      const prompt=`${task.title}\n${task.requirements}\n${EXECUTION_CONTRACT}\n${e.assignment.condition==="specification"?task.specification:"The starter contains integer helper functions. Task semantics above are authoritative."}\nPublic cases: ${canonical(task.publicCases.map(c=>({args:c.args,expected:c.expected})))}\nStarter:\n${source}\nLimits: ${canonical(limits)}`;
      record("episode.prompt",{artifactHash:this.store.putArtifact({system:SYSTEM,prompt,tools:e.assignment.mode==="tools"?TOOLS:[]})});
      session.user(prompt);
      for(const round of e.assignment.condition==="learning"?[0,1,2,3]:[3]){
        if(e.assignment.condition==="learning"&&round>0){
          const feedback=task.feedback[round-1],check=await grade(feedback);
          session.user(`Feedback round ${round}: Your current source ${check.passed?"passed":"failed"} these examples. Their exact behavior is ${canonical(feedback.map(c=>({args:c.args,expected:c.expected})))}. Revise your source and submit again. Transfer probes remain unseen.`);
        }
        let submitted=false;
        while(!submitted){
          const maxOutput=Math.min(config.maxOutputTokens,limits.outputTokens-resources.outputTokens);
          if(maxOutput<=0||resources.inputTokens>=limits.inputTokens||controller.signal.aborted)throw new LabError("budget_exhausted","Episode budget exhausted.");
          const inputBound=await session.countInput(maxOutput,controller.signal);
          if(inputBound!==null&&inputBound>limits.inputTokens-resources.inputTokens)throw new LabError("budget_exhausted","The next prompt exceeds the remaining input budget.");
          const reservedInput=limits.inputTokens-resources.inputTokens;
          const reserve=cost(config,{inputTokens:reservedInput,outputTokens:maxOutput})||0,reservation=this.store.reserve(run.id,reserve),start=Date.now();
          record("provider.dispatch",{provider:config.provider,requestedModel:config.model,requestedSettings:config.settings,inputUpperBound:inputBound,maxOutput,reservation});
          let turn;
          try{turn=await session.next(maxOutput,controller.signal);}
          catch(error){if(!(error instanceof ProviderFailure)||error.ambiguous)resources.costNanoUsd=null;try{this.store.settle(reservation,error instanceof ProviderFailure&&!error.ambiguous?0:null);}catch{}throw error;}
          finally{resources.modelMs+=Date.now()-start;}
          resources.inputTokens+=turn.usage.inputTokens;resources.outputTokens+=turn.usage.outputTokens;
          reasoningKnown=reasoningKnown&&turn.usage.reasoningTokens!==null;cachedKnown=cachedKnown&&turn.usage.cachedInputTokens!==null;
          resources.reasoningTokens=reasoningKnown?(resources.reasoningTokens||0)+turn.usage.reasoningTokens!:null;
          resources.cachedInputTokens=cachedKnown?(resources.cachedInputTokens||0)+turn.usage.cachedInputTokens!:null;
          record("provider.response",{provider:config.provider,requestedModel:config.model,reportedModel:turn.reportedModel,responseId:turn.responseId,usage:turn.usage,status:turn.status,artifactHash:this.store.putArtifact(turn.raw)});
          if(turn.reportedModel){obs.providerModels??=[];if(!obs.providerModels.includes(turn.reportedModel))obs.providerModels.push(turn.reportedModel);}
          const measured=cost(config,turn.usage);if(resources.costNanoUsd!==null)resources.costNanoUsd+=measured||0;
          this.store.settle(reservation,measured);
          if(resources.inputTokens>limits.inputTokens||resources.outputTokens>limits.outputTokens||turn.usage.outputTokens>maxOutput||turn.status==="length")throw new LabError("budget_exhausted","Provider output exhausted the task budget.");
          if(turn.status==="refusal")throw new LabError("refusal","Model declined the task.");
          if(turn.calls.length){
            invariant(e.assignment.mode==="tools","Model-only episodes cannot call tools.");
            const results:ToolResult[]=[];
            for(const call of turn.calls){
              if(++resources.toolCalls>limits.toolCalls)throw new LabError("budget_exhausted","Tool-call budget exhausted.");
              let result:unknown;
              try{
                invariant(call.arguments&&typeof call.arguments==="object"&&!Array.isArray(call.arguments),"Tool arguments must be an object.");
                const args=call.arguments as Record<string,unknown>;
                if(submitted)result={error:"This stage has already been submitted."};
                else if(call.name==="list_files")result={files:["solution.trit"]};
                else if(call.name==="read_file"){invariant(args.path==="solution.trit","Only participant files may be read.");result={content:source};}
                else if(call.name==="replace_file"){invariant(args.path==="solution.trit","Only solution.trit is editable.");setSource(args.content);result={saved:true};}
                else if(call.name==="run_public_tests"){
                  invariant(args.name==="public","Unknown public test set.");if(++resources.publicTestCalls>limits.publicTestCalls)throw new LabError("budget_exhausted","Public-test budget exhausted.");
                  const g=await grade(task.publicCases);result={passed:g.passed,category:g.category,feedback:g.details};
                }else if(call.name==="submit_answer"){submitted=true;result={submitted:true};}
                else result={error:"Unknown structured tool."};
              }catch(error){if(error instanceof LabError&&["budget_exhausted","worker_failure","worker_unavailable","cancelled"].includes(error.code))throw error;result={error:(error as Error).message};}
              record("tool.event",{name:call.name,callId:call.id,arguments:call.arguments,result});results.push({id:call.id,name:call.name,result});
            }
            session.results(results);
          }else{
            let answer:any;try{answer=JSON.parse(turn.text);}catch{throw new LabError("invalid_submission","Final answer must be the declared JSON files object.");}
            invariant(answer?.files&&Object.keys(answer.files).length===1&&Object.hasOwn(answer.files,"solution.trit"),"Submit only solution.trit.");setSource(answer.files["solution.trit"]);submitted=true;
          }
        }
        if(round!==2){
          const probes=e.assignment.condition==="learning"?task.probes[round===0?0:round===1?1:2]:task.privateCases;
          const g=await grade(probes);
          // Binary correctness remains separate from learning-stage observations.
          if(e.assignment.condition==="learning")obs.learning.push({round:round as 0|1|3,passed:g.passedCases,total:probes.length,success:g.passed});
          if(round===3){obs.success=g.passed;obs.state=g.passed?"passed":"failed";obs.category=g.category;obs.robustness={passed:g.passedCases,total:probes.length};}
        }
      }
    }catch(error){
      const code=error instanceof LabError?error.code:"worker_failure";
      const cancelled=controller.signal.aborted&&controller.signal.reason!=="episode_deadline";
      const infrastructure=controller.signal.reason!=="episode_deadline"&&(error instanceof ProviderFailure||["worker_failure","worker_unavailable","credential_missing","pricing_contract","frozen_mismatch","spending_limit"].includes(code));
      obs.state=cancelled?(this.stopped?"interrupted":"cancelled"):infrastructure?"infrastructure_error":"failed";
      obs.success=infrastructure||cancelled?null:false;obs.category=controller.signal.reason==="episode_deadline"?"budget_exhausted":code;
      record("episode.error",{category:obs.category,message:(error as Error).message});
    }finally{session.close?.();clearTimeout(timer);record("submission.final",{artifactHash:this.store.putArtifact({"solution.trit":source})});}
    return obs;
  }
  calibration(){
    const settings=this.settings(),sorted=(models:ModelConfig[])=>[...models].sort((a,b)=>a.id.localeCompare(b.id));
    const configured=settings.calibrationModelIds.flatMap(id=>settings.models.filter(m=>m.id===id));
    const runs=this.store.runs().filter(r=>r.protocol.purpose==="calibration"&&r.provenance==="live"&&r.state==="completed"&&hash(sorted(r.models))===hash(sorted(configured))&&
      hash(r.protocol)===hash(this.protocol({modelIds:settings.calibrationModelIds,experiment:r.protocol.experiment,purpose:"calibration",spendingCeilingUsd:null,limits:r.protocol.limits})));
    // Use the newest complete run for each experiment; never cherry-pick individual successful attempts.
    const baseline=runs.find(r=>r.protocol.experiment==="default"),controlled=runs.find(r=>r.protocol.experiment==="controlled");
    const observations=baseline?this.store.episodes(baseline.id).flatMap(e=>e.observation?[e.observation]:[]):[];
    const report=calibrationSummary(observations,corpus().map(f=>f.id),settings.calibrationModelIds);
    const savedReview=this.store.setting<AmbiguityReview>("ambiguity-review");
    const review=savedReview&&savedReview.revision===REVISION&&savedReview.baselineRunId===baseline?.id&&savedReview.controlledRunId===controlled?.id?savedReview:null;
    const failures=[];
    if(!baseline||!report.coverageComplete)failures.push("Complete three fresh baseline attempts per family for every preselected model.");
    if(!controlled||this.store.episodes(controlled.id).some(e=>!e.observation||e.observation.success===null))failures.push("Complete the controlled calibration across all three conditions and both tool modes.");
    if(baseline&&controlled&&baseline.protocol.profile!==controlled.protocol.profile)failures.push("Baseline and controlled calibration must use matching resource limits.");
    if(report.requiresRevision)failures.push("More than 25% of families are universally passed or failed; revise the draft corpus and recalibrate.");
    if(!review)failures.push("Record an ambiguity review for all 36 families against the current calibration runs.");
    else if(review.families.some(f=>f.ambiguous))failures.push("Resolve reported ambiguity, revise affected task packages, and recalibrate before freezing.");
    return {...report,ambiguity:{reviewed:Boolean(review),review,template:{families:corpus().map(f=>({familyId:f.id,ambiguous:null,notes:""}))}},failures,baselineRunId:baseline?.id||null,controlledRunId:controlled?.id||null,
      conditionScores:controlled?scoreRows(this.store.episodes(controlled.id).flatMap(e=>e.observation?[e.observation]:[]),controlled.protocol,settings.calibrationModelIds):[]};
  }
  freeze(){
    const failures=[...qualificationFailures(this.store,this.worker),...this.calibration().failures];invariant(!failures.length,failures.join(" "));
    const calibration=this.calibration(),limits=this.store.run(calibration.baselineRunId!).protocol.limits;
    const evaluationProtocols=Object.fromEntries((["default","controlled"] as const).map(experiment=>[experiment,hash(this.protocol({modelIds:[],purpose:"evaluation",experiment,spendingCeilingUsd:null,limits}))]));
    const accepted=this.store.setting<Record<string,string>>("accepted-revision-protocols")||{};
    const fingerprint=hash(evaluationProtocols);
    invariant(!accepted[REVISION]||accepted[REVISION]===fingerprint,"This revision was already accepted with different content or limits. Assign a new pilot revision and recalibrate.");
    const value={revision:REVISION,at:now(),familyHashes:Object.fromEntries(corpus().map(t=>[t.id,t.packageHash])),qualificationHash:this.store.setting<QualificationReport>("qualification")!.hash,evaluationProtocols,calibration,exposure:"Calibration-exposed pilot; cannot be represented as a future unseen set."};
    this.store.setSetting("accepted-revision-protocols",{...accepted,[REVISION]:fingerprint});this.store.setSetting("freeze",value);this.store.putArtifact(value);return value;
  }
  publicationFailures(run:RunView){
    const failures:string[]=[];
    if(run.provenance!=="live")failures.push("Deterministic provider fixtures cannot be published as model results.");
    if(run.protocol.purpose!=="evaluation")failures.push("Calibration runs are not scored publications.");
    if(run.state!=="completed")failures.push("Run has not completed.");
    if(this.store.episodes(run.id).some(e=>!e.observation||e.observation.success===null))failures.push("Coverage is incomplete or includes infrastructure errors/interrupted episodes.");
    if(!this.matchesFreeze(run.protocol))failures.push("This run does not match the accepted frozen protocol.");
    return failures;
  }
  private matchesFreeze(protocol:Protocol){
    const frozen=this.store.setting<{revision:string;qualificationHash:string;evaluationProtocols:Record<string,string>}>("freeze");
    return Boolean(frozen&&frozen.revision===protocol.revision&&frozen.qualificationHash===this.store.setting<QualificationReport>("qualification")?.hash&&frozen.evaluationProtocols?.[protocol.experiment]===hash(protocol));
  }
  publish(id:string):Publication{
    const run=this.store.run(id),failures=this.publicationFailures(run);invariant(!failures.length,failures.join(" "));
    const observations=this.store.episodes(id).map(e=>e.observation!);
    const models=run.models.map(({keyEnv,supportedSettings,...publicConfig})=>publicConfig);
    const p:Publication={id:randomUUID(),createdAt:now(),label:PILOT_LABEL,protocolHash:run.protocolHash,profileHash:hash(run.protocol),revision:run.protocol.revision,models,
      scores:scoreRows(observations,run.protocol,models.map(m=>m.id)),observations,coverage:{complete:true,expected:run.total,evaluated:observations.length},
      protocol:{limits:run.protocol.limits,experiment:run.protocol.experiment,transport:run.protocol.transport,harness:run.protocol.harness,promptHash:run.protocol.promptHash,checkerHash:run.protocol.checkerHash},
      environment:{platform:"win32",compilerHash:run.protocol.compilerHash,workerHash:run.protocol.workerHash},runId:id};
    return this.store.publish(p);
  }
}
