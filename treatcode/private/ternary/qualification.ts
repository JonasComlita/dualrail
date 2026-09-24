import { DEFAULT_LIMITS } from "../../src/ternary/contracts";
import { corpus, REVISION } from "./corpus";
import { WindowsWorker } from "./worker";
import { LabStore } from "./store";
import { now, hash } from "./util";

export interface QualificationReport {
  revision: string; at: string; hash: string; workerHash: string | null; compilerHash: string | null;
  passed: boolean; families: Array<{ id: string; packageHash: string; passed: boolean; cases: number; exhaustive: boolean;
    checks: Array<{ name: string; passed: boolean; category: string; evidenceHash: string }> }>;
}
export async function qualify(store: LabStore, worker: WindowsWorker, progress?: (message: string)=>void): Promise<QualificationReport> {
  const readiness=worker.readiness();
  if(!readiness.ready) throw new Error(readiness.errors.join(" "));
  const report:QualificationReport={revision:REVISION,at:now(),hash:"",workerHash:readiness.workerHash,compilerHash:readiness.compilerHash,passed:false,families:[]};
  for(const task of corpus()){
    const checks:QualificationReport["families"][number]["checks"]=[];
    const candidates=[{name:"reference",source:task.reference,expected:true,cases:task.qualificationCases},
      {name:"starter",source:task.starter["solution.trit"],expected:false,cases:task.qualificationCases},
      ...task.mutants.map(m=>({name:m.name,source:m.source,expected:false,cases:task.qualificationCases.filter(c=>m.targetCaseIds.includes(c.id))}))];
    for(const candidate of candidates){
      const result=await worker.grade(candidate.source,candidate.cases,{...DEFAULT_LIMITS},new AbortController().signal);
      const passed=candidate.expected?result.passed:!result.passed&&result.category==="wrong_answer";
      const evidenceHash=store.putArtifact({family:task.id,candidate:candidate.name,sourceHash:hash(candidate.source),cases: candidate.cases,result});
      checks.push({name:candidate.name,passed,category:result.category,evidenceHash});
    }
    const passed=checks.every(c=>c.passed);
    report.families.push({id:task.id,packageHash:task.packageHash,passed,cases:task.qualificationCases.length,exhaustive:task.exhaustive,checks});
    progress?.(`${passed?"PASS":"FAIL"} ${task.id} (${task.qualificationCases.length} cases): ${checks.filter(c=>!c.passed).map(c=>`${c.name}/${c.category}`).join(", ")}`);
  }
  report.passed=report.families.length===36&&report.families.every(f=>f.passed);
  report.hash=hash({...report,hash:undefined});
  store.putArtifact(report);store.setSetting("qualification",report);
  return report;
}
export function qualificationFailures(store:LabStore,worker:WindowsWorker):string[]{
  const report=store.setting<QualificationReport>("qualification"), current=worker.readiness();
  if(!report)return ["The 36 private families have not yet been qualified on this worker."];
  const failures:string[]=[];
  if(report.hash!==hash({...report,hash:undefined}))failures.push("Qualification report integrity check failed.");
  if(!report.passed)failures.push("One or more family qualification checks failed.");
  if(report.revision!==REVISION||report.workerHash!==current.workerHash||report.compilerHash!==current.compilerHash)failures.push("Qualification is stale for this revision or toolchain.");
  for(const task of corpus())if(!report.families.some(f=>f.id===task.id&&f.packageHash===task.packageHash&&f.passed))failures.push(`${task.id} requires qualification.`);
  return failures;
}
