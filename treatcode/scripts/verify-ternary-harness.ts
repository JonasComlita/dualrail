import path from "node:path";
import { IntelligenceLab } from "../private/ternary/service";
import { HARNESS_LINEUP, HARNESS_TIERS } from "../src/ternary/lineup";
const repo=path.resolve(import.meta.dir,"../.."),root=process.env.TI_DATA_ROOT||path.join(repo,"build/ternary-intelligence");
const lab=new IntelligenceLab(repo,root,{autoStart:false});
try{
  lab.configure({models:HARNESS_LINEUP,calibrationModelIds:HARNESS_LINEUP.map(m=>m.id),calibrationTiers:HARNESS_TIERS});
  for(const model of HARNESS_LINEUP){
    console.log(`Verifying ${model.id} using the local harness (two non-scored requests).`);
    const report=await lab.verifyModel(model.id);console.log(JSON.stringify({model:model.id,state:report.state,reportedModels:report.reportedModels,usage:report.usage,failures:report.failures}));
    if(report.state!=="passed"){process.exitCode=1;break;}
  }
}finally{lab.close();}
