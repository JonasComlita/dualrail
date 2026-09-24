import { interpretExample } from "./example-interpreter";
self.onmessage=(event:MessageEvent<{requestId:number;id:string;input:string}>)=>{
  const {requestId,id,input}=event.data;
  try{self.postMessage({requestId,result:interpretExample(id,input)});}catch(error){self.postMessage({requestId,error:(error as Error).message});}
};
