import path from "node:path";
import { IntelligenceLab } from "../private/ternary/service";
const experiment=process.argv[2];if(experiment!=="default"&&experiment!=="controlled")throw new Error("Choose default or controlled calibration.");
const repo=path.resolve(import.meta.dir,"../.."),root=process.env.TI_DATA_ROOT||path.join(repo,"build/ternary-intelligence"),lab=new IntelligenceLab(repo,root,{autoStart:false});
let timer:ReturnType<typeof setInterval>|undefined;
let interrupted=false;
const stop=()=>{interrupted=true;if(timer)clearInterval(timer);lab.close();process.exitCode=130;};process.once("SIGINT",stop);process.once("SIGTERM",stop);
try{
  const queued=lab.store.runs().find(r=>r.protocol.purpose==="calibration"&&r.protocol.experiment===experiment&&r.state==="queued");
  const run=queued||lab.create({modelIds:lab.settings().calibrationModelIds,experiment,purpose:"calibration",spendingCeilingUsd:null},"local-operator",crypto.randomUUID());
  const progress=()=>{const r=lab.store.run(run.id),episodes=lab.store.episodes(r.id);console.log(JSON.stringify({id:r.id,state:r.state,finished:r.finished,total:r.total,passed:episodes.filter(e=>e.observation?.success===true).length,failed:episodes.filter(e=>e.observation?.success===false).length,incomplete:episodes.filter(e=>["infrastructure_error","interrupted"].includes(e.state)).length,current:episodes.find(e=>e.id===r.activeEpisodeId)?.assignment.familyId||null}));};
  progress();timer=setInterval(progress,30000);await lab.pump();
  // A signal may have closed the store; successful runs retain it until this finally block.
  if(!interrupted){progress();console.log(JSON.stringify({calibrationFailures:lab.calibration().failures}));}
}finally{if(timer)clearInterval(timer);lab.close();process.removeListener("SIGINT",stop);process.removeListener("SIGTERM",stop);}
