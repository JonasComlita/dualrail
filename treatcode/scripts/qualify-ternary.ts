import path from "node:path";
import { IntelligenceLab } from "../private/ternary/service";
const repo=path.resolve(import.meta.dir,"../..");
const root=process.env.TI_DATA_ROOT||path.join(repo,"build/ternary-intelligence");
const lab=new IntelligenceLab(repo,root,{autoStart:false});
try {
  const report=await lab.qualify(console.log);
  console.log(`Qualification ${report.passed?"passed":"failed"}: ${report.families.filter(f=>f.passed).length}/36 families; ${report.hash}`);
  process.exitCode=report.passed?0:1;
}finally{lab.close();}
