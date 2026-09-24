/** Explicit live, non-scored compatibility check using the user's local harness sign-in. */
import fs from "node:fs";
import path from "node:path";
import { HarnessSession } from "../private/ternary/harness";
import { HARNESS_LINEUP } from "../src/ternary/lineup";
const model=HARNESS_LINEUP.find(m=>m.model===(process.argv[2]||"gpt-5.6-luna"));if(!model)throw new Error("Unknown selected harness model.");
const session=new HarnessSession(model,"Provider compatibility check. Submit a solve function returning zero; do not call tools.",[]),controller=new AbortController();
const timer=setTimeout(()=>controller.abort(),120000);
try{
  session.user("Compatibility check only. Submit numeric Trit source: fn solve(a:t40,b:t40,c:t40)->t40{return 0;}");
  const result=await session.next(24000,controller.signal);
  const report={at:new Date().toISOString(),kind:"non-scored-compatibility",requested:model.model,reported:result.reportedModel,status:result.status,usage:result.usage,text:result.text,raw:result.raw};
  const file=path.resolve(import.meta.dir,`../../build/ternary-intelligence/harness-${model.id}-smoke.json`);fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,JSON.stringify(report,null,2));
  console.log(JSON.stringify({requested:model.model,reported:result.reportedModel,status:result.status,usage:result.usage,answer:result.text}));
}finally{clearTimeout(timer);session.close();}
