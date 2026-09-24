import { expect, test } from "bun:test";
import { HarnessSession } from "../private/ternary/harness";
import { HARNESS_LINEUP } from "../src/ternary/lineup";
import { validateModel, type ProviderTurn } from "../private/ternary/providers";

// Inject protocol events only; these tests never start the CLI or make inference requests.
function pending(session:HarnessSession){
  const internal=session as any;internal.threadId="fixture-thread";
  const result=new Promise<ProviderTurn>((resolve,reject)=>{internal.finished=resolve;internal.failed=reject;});
  return {result,event:(method:string,params:any)=>internal.consume({method,params:{threadId:"fixture-thread",...params}})};
}
test("harness response events preserve structured actions and cumulative usage differences",async()=>{
  const session=new HarnessSession(HARNESS_LINEUP[3],"fixture",[]);
  let turn=pending(session);
  turn.event("item/completed",{item:{type:"agentMessage",phase:"final_answer",text:JSON.stringify({files:null,tool_calls:[{name:"read_file",arguments:'{"path":"solution.trit"}'}]})}});
  turn.event("thread/tokenUsage/updated",{tokenUsage:{total:{inputTokens:100,outputTokens:40,reasoningOutputTokens:10,cachedInputTokens:0}}});
  turn.event("turn/completed",{turn:{id:"turn-1",status:"completed"}});
  const first=await turn.result;expect(first.calls[0]).toMatchObject({name:"read_file",arguments:{path:"solution.trit"}});expect(first.usage.outputTokens).toBe(40);
  // A second model turn in the same learning episode counts only new consumption.
  (session as any).responseItems=[];turn=pending(session);
  turn.event("item/completed",{item:{type:"agentMessage",phase:"final_answer",text:'{"files":{"solution.trit":"source"},"tool_calls":[]}'}});
  turn.event("thread/tokenUsage/updated",{tokenUsage:{total:{inputTokens:250,outputTokens:100,reasoningOutputTokens:35,cachedInputTokens:50}}});
  turn.event("turn/completed",{turn:{id:"turn-2",status:"completed"}});
  expect((await turn.result).usage).toEqual({inputTokens:150,outputTokens:60,reasoningTokens:25,cachedInputTokens:50});
  expect((new HarnessSession(HARNESS_LINEUP[3],"fresh",[]) as any).consumed.inputTokens).toBe(0);
  session.close();
});
test("harness reroutes, native tools, errors, and missing usage invalidate the response",async()=>{
  for(const [method,params,code] of [
    ["model/rerouted",{},"model_substitution"],
    ["item/started",{item:{type:"commandExecution"}},"harness_tool_violation"],
    ["error",{willRetry:true},"harness_inference_error"],
    ["turn/completed",{turn:{id:"x",status:"completed"}},"missing_usage"],
  ] as const){const session=new HarnessSession(HARNESS_LINEUP[3],"fixture",[]),turn=pending(session);turn.event(method,params);await expect(turn.result).rejects.toMatchObject({code,ambiguous:true});expect((session as any).closed).toBe(true);}
});
test("harness lineup is exact and subscription usage cannot be priced as API billing",()=>{
  expect(HARNESS_LINEUP.map(m=>m.model)).toEqual(["gpt-6-astra","gpt-5.6-sol","gpt-5.6-terra","gpt-5.6-luna"]);
  HARNESS_LINEUP.forEach(validateModel);
  expect(()=>validateModel({...HARNESS_LINEUP[0],pricing:{snapshot:"wrong-billing",date:"2026-09-23",inputUsdPerMillion:1,outputUsdPerMillion:1}})).toThrow("Subscription");
});
