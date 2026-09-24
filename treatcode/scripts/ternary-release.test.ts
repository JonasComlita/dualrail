/** Release-gate unit fixtures exist only in a disposable database, never the operator store. */
import { expect, test } from "bun:test";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { IntelligenceLab } from "../private/ternary/service";
import { corpus, REVISION } from "../private/ternary/corpus";
import type { QualificationReport } from "../private/ternary/qualification";
import type { Observation, Protocol } from "../src/ternary/contracts";
import { hash } from "../private/ternary/util";

test("release gates bind calibration, ambiguity, freeze and publication to the accepted revision",()=>{
  const root=fs.mkdtempSync(path.join(os.tmpdir(),"ternary-release-fixture-"));
  const lab=new IntelligenceLab(path.resolve(import.meta.dir,"../.."),root,{autoStart:false});
  try{
    // These synthetic records exercise release policy; native qualification is tested separately.
    // No inference runs here, and this temporary database is never served by the website.
    const tasks=corpus(),ready=lab.worker.readiness();
    const qualification:QualificationReport={revision:REVISION,at:new Date().toISOString(),hash:"",workerHash:ready.workerHash,compilerHash:ready.compilerHash,passed:true,
      families:tasks.map(t=>({id:t.id,packageHash:t.packageHash,passed:true,cases:t.qualificationCases.length,exhaustive:t.exhaustive,checks:[]}))};
    qualification.hash=hash({...qualification,hash:undefined});lab.store.setSetting("qualification",qualification);
    const models=lab.settings().models,modelIds=models.map(m=>m.id);
    const protocol=(purpose:Protocol["purpose"],experiment:Protocol["experiment"],limits={})=>lab.protocol({modelIds,purpose,experiment,limits,spendingCeilingUsd:null});
    const seedRun=(p:Protocol,missing=false)=>{
      const run=lab.store.createRun({protocol:p,models,ceilingUsd:null,owner:"release-unit-fixture",requestKey:crypto.randomUUID(),provenance:"live"});
      const episodes=lab.store.episodes(run.id);
      for(const e of episodes){
        const success=missing&&e.ordinal===0?null:e.assignment.repeat!==1;
        const o:Observation={...e.assignment,modelConfigId:e.modelConfigId,state:success===null?"interrupted":success?"passed":"failed",success,category:"synthetic-release-policy-fixture",learning:[],robustness:null,
          resources:{inputTokens:0,outputTokens:0,reasoningTokens:null,cachedInputTokens:null,toolCalls:0,publicTestCalls:0,modelMs:0,programMs:null,peakMemoryBytes:null,costNanoUsd:null}};
        lab.store.finishEpisode(e.id,o);
      }
      lab.store.state(run.id,"completed");return lab.store.run(run.id);
    };
    expect(()=>lab.freeze()).toThrow("baseline");
    const baseline=seedRun(protocol("calibration","default"));
    expect(lab.calibration().coverageComplete).toBe(true);
    expect(lab.calibration().requiresRevision).toBe(false);
    expect(()=>lab.freeze()).toThrow("controlled calibration");
    seedRun(protocol("calibration","controlled"));
    expect(()=>lab.freeze()).toThrow("ambiguity review");
    const families=tasks.map(t=>({familyId:t.id,ambiguous:false,notes:"Synthetic unit-test review of release policy only."}));
    expect(()=>lab.reviewAmbiguity({families:families.slice(1)},"fixture")).toThrow("all 36");
    lab.reviewAmbiguity({families:families.map((f,i)=>({...f,ambiguous:i===0}))},"fixture");
    expect(()=>lab.freeze()).toThrow("reported ambiguity");
    lab.reviewAmbiguity({families},"fixture");
    const accepted=lab.freeze();expect(accepted.calibration.baselineRunId).toBe(baseline.id);
    const evaluation=seedRun(protocol("evaluation","default"));
    expect(lab.publicationFailures(evaluation)).toEqual([]);
    const published=lab.publish(evaluation.id);
    expect(published.coverage).toEqual({complete:true,expected:144,evaluated:144});
    expect(published.models.every(m=>!("keyEnv" in m))).toBe(true);
    expect(lab.publish(evaluation.id).id).toBe(published.id);
    const incomplete=seedRun(protocol("evaluation","default"),true);
    expect(()=>lab.publish(incomplete.id)).toThrow("Coverage is incomplete");
    expect(()=>lab.publish(baseline.id)).toThrow("Calibration runs");
    const changed=seedRun(protocol("evaluation","default",{outputTokens:12000}));
    expect(()=>lab.publish(changed.id)).toThrow("frozen protocol");
    // New full calibration at changed limits cannot reuse an already accepted revision ID.
    seedRun(protocol("calibration","default",{outputTokens:12000}));
    seedRun(protocol("calibration","controlled",{outputTokens:12000}));
    expect(lab.calibration().ambiguity.reviewed).toBe(false);
    lab.reviewAmbiguity({families},"fixture");
    expect(()=>lab.freeze()).toThrow("new pilot revision");
    expect(lab.store.publications()[0]).toEqual(published);
  }finally{lab.close();fs.rmSync(root,{recursive:true,force:true});}
},30000);
