import fs from "node:fs";
import path from "node:path";
import { corpus } from "../private/ternary/corpus";

const root=path.resolve(import.meta.dir,".."), tasks=corpus();
const markers=["targetCaseIds","qualificationCases",...tasks.flatMap(task=>[task.requirements,task.reference])];
let files=0;
function inspect(directory:string){
  if(!fs.existsSync(directory))throw new Error("Build public assets before running the privacy audit.");
  for(const entry of fs.readdirSync(directory,{withFileTypes:true})){
    const file=path.join(directory,entry.name);
    if(entry.isDirectory()){inspect(file);continue;}
    if(!/\.(?:js|json|html|css|map|txt|md)$/.test(entry.name))continue;
    const source=fs.readFileSync(file,"utf8");files++;
    if(markers.some(marker=>source.includes(marker)))throw new Error(`Private task material found in ${path.relative(root,file)}.`);
    if(/(?:["/]|\\\\)private[/\\]+ternary[/\\]+(?:corpus|qualification|service|providers)/i.test(source))throw new Error(`Private benchmark source path found in ${path.relative(root,file)}.`);
  }
}
inspect(path.join(root,"dist"));inspect(path.join(root,"public/api"));
console.log(`Ternary public privacy audit passed: ${files} text assets and exports contain no private corpus or evaluator material.`);
