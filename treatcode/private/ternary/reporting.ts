import { CAPABILITIES, type Observation, type Protocol, type Publication, type ScoreRow } from "../../src/ternary/contracts";
import { hash, invariant } from "./util";

function random(seed: number) { let x=seed>>>0; return ()=>{x=(Math.imul(x,1664525)+1013904223)>>>0;return x/4294967296;}; }
function mean(values:number[]) { return values.reduce((s,x)=>s+x,0)/values.length; }
function interval(values:number[],seed:string):[number,number]|null {
  if(!values.length)return null;
  const rng=random(parseInt(hash(seed).slice(0,8),16)),samples:number[]=[];
  for(let i=0;i<10000;i++){let sum=0;for(let j=0;j<values.length;j++)sum+=values[Math.floor(rng()*values.length)];samples.push(sum/values.length);}
  samples.sort((a,b)=>a-b);return [samples[249],samples[9749]];
}
export function scoreRows(observations:Observation[],protocol:Protocol,modelIds:string[]):ScoreRow[]{
  const rows:ScoreRow[]=[];
  for(const modelConfigId of modelIds)for(const capability of ["all",...CAPABILITIES.map(c=>c.id)] as const){
    for(const condition of ["prior","specification","learning"] as const)for(const mode of ["model-only","tools"] as const){
      const assignments=protocol.assignments.filter(a=>a.condition===condition&&a.mode===mode&&(capability==="all"||a.familyId.startsWith(capability+"-")));
      if(!assignments.length)continue;
      const data=observations.filter(o=>o.modelConfigId===modelConfigId&&o.condition===condition&&o.mode===mode&&(capability==="all"||o.familyId.startsWith(capability+"-"))&&o.success!==null);
      const families=[...new Set(data.map(o=>o.familyId))];
      const rates=families.map(f=>mean(data.filter(o=>o.familyId===f).map(o=>Number(o.success))));
      rows.push({modelConfigId,capability,condition,mode,successes:data.filter(o=>o.success).length,evaluated:data.length,expected:assignments.length,rate:rates.length?mean(rates):null,interval:interval(rates,`${protocol.revision}:${capability}:${condition}:${mode}`)});
    }
  }
  return rows;
}
function coverageKeys(p:Publication,modelId:string){return p.observations.filter(o=>o.modelConfigId===modelId).map(o=>`${o.familyId}:${o.condition}:${o.mode}:${o.repeat}:${o.success===null?"missing":"measured"}`).sort();}
export function compare(a:Publication,modelA:string,b:Publication,modelB:string){
  invariant(a.profileHash===b.profileHash,"Comparisons require matching protocol profiles.");
  const ca=coverageKeys(a,modelA),cb=coverageKeys(b,modelB);
  invariant(ca.length>0&&JSON.stringify(ca)===JSON.stringify(cb)&&!ca.some(k=>k.endsWith(":missing")),"Comparisons require matching, complete task coverage.");
  const rows=[];
  for(const condition of ["prior","specification","learning"] as const)for(const mode of ["model-only","tools"] as const){
    const left=a.observations.filter(o=>o.modelConfigId===modelA&&o.condition===condition&&o.mode===mode);
    const right=b.observations.filter(o=>o.modelConfigId===modelB&&o.condition===condition&&o.mode===mode);
    if(!left.length)continue;
    const families=[...new Set(left.map(o=>o.familyId))];
    const deltas=families.map(f=>mean(left.filter(o=>o.familyId===f).map(o=>Number(o.success)))-mean(right.filter(o=>o.familyId===f).map(o=>Number(o.success))));
    rows.push({condition,mode,families:families.length,difference:mean(deltas),interval:interval(deltas,`${a.profileHash}:${condition}:${mode}:paired`)});
  }
  return {modelA,modelB,unit:"task family",samples:10000,confidence:0.95,rows};
}
export function calibrationSummary(observations:Observation[],families:string[],models:string[]){
  const perFamily=families.map(f=>{
    const values=observations.filter(o=>o.familyId===f&&o.condition==="specification"&&o.mode==="tools"&&o.success!==null);
    const complete=models.length>=2&&models.every(m=>new Set(values.filter(o=>o.modelConfigId===m).map(o=>o.repeat)).size===3);
    return {familyId:f,complete,universallyPassed:complete&&values.every(o=>o.success),universallyFailed:complete&&values.every(o=>!o.success),
      repeatability:models.map(m=>{const attempts=values.filter(o=>o.modelConfigId===m);return {modelConfigId:m,successes:attempts.filter(o=>o.success).length,attempts:attempts.length};})};
  });
  const ceiling=perFamily.filter(f=>f.universallyPassed).length,floor=perFamily.filter(f=>f.universallyFailed).length;
  return {perFamily,ceilingFamilies:ceiling,floorFamilies:floor,requiresRevision:(ceiling+floor)>families.length*0.25,coverageComplete:perFamily.every(f=>f.complete),
    interpretation:"Pilot task difficulty; no desired provider ranking or IQ conversion."};
}
