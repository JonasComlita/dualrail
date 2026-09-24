/** Live non-scored integration check, using a public constant-return exercise. */
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { HarnessSession } from "../private/ternary/harness";
import { WindowsWorker } from "../private/ternary/worker";
import { HARNESS_LINEUP } from "../src/ternary/lineup";
import { DEFAULT_LIMITS } from "../src/ternary/contracts";
const repo=path.resolve(import.meta.dir,"../.."),root=fs.mkdtempSync(path.join(os.tmpdir(),"ternary-harness-tools-"));
const session=new HarnessSession(HARNESS_LINEUP[3],"This is a non-scored structured-action integration exercise.",[{name:"list_files",description:"List participant files and a test integer.",parameters:{type:"object",properties:{},additionalProperties:false}}]);
const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),120000);
try{
  session.user("First request list_files. Once its result supplies test_integer, submit solve(a:t40,b:t40,c:t40)->t40 returning that exact integer. This is not a compatibility check; do not guess the integer before receiving the tool result.");
  const first=await session.next(24000,controller.signal);if(first.calls.length!==1||first.calls[0].name!=="list_files")throw new Error("Harness did not request the declared structured action.");
  session.results([{id:first.calls[0].id,name:"list_files",result:{files:["solution.trit"],test_integer:37}}]);
  const second=await session.next(24000,controller.signal),source=JSON.parse(second.text).files?.["solution.trit"];
  const grade=await new WindowsWorker(repo,path.join(root,"grading")).grade(source,[{id:"public-control",args:[0,0,0],expected:37}],{...DEFAULT_LIMITS},controller.signal);
  if(!grade.passed)throw new Error("Harness did not retain the tool result and submit compilable code.");
  const report={at:new Date().toISOString(),kind:"non-scored-public-tool-control",model:HARNESS_LINEUP[3].model,passed:true,first,second,grade};
  fs.writeFileSync(path.join(repo,"build/ternary-intelligence/harness-tool-control.json"),JSON.stringify(report,null,2));
  console.log(JSON.stringify({passed:true,model:HARNESS_LINEUP[3].model,structuredActions:first.calls.length,modelTurns:2,inputTokens:first.usage.inputTokens+second.usage.inputTokens,outputTokens:first.usage.outputTokens+second.usage.outputTokens,nativeGrader:grade.category}));
}finally{clearTimeout(timer);session.close();const target=path.resolve(root);if(path.dirname(target)===path.resolve(os.tmpdir())&&path.basename(target).startsWith("ternary-harness-tools-"))fs.rmSync(target,{recursive:true,force:true});}
