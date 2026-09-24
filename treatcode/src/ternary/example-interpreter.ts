/** Restricted, total interpreter over structured example inputs; never evaluates source. */
export function interpretExample(id:string,text:string):unknown{
  if(text.length>2048)throw new Error("Example input is limited to 2,048 characters.");
  const input=JSON.parse(text);
  if(!input||typeof input!=="object"||Array.isArray(input))throw new Error("Enter a JSON object.");
  const bounded=(x:unknown,min:number,max:number)=>{if(typeof x!=="number"||!Number.isSafeInteger(x)||x<min||x>max)throw new Error(`Use integers from ${min} to ${max}.`);return x;};
  if(id==="addition"){
    const sum=bounded(input.a,-13,13)+bounded(input.b,-13,13),carry=sum>13?1:sum< -13?-1:0,word=sum-27*carry;
    let n=word,encoded="";
    for(let i=0;i<3;i++){let rem=((n%3)+3)%3;if(rem===2)rem=-1;encoded=(rem===-1?"T":String(rem))+encoded;n=(n-rem)/3;}
    return {sum,carry,word,balancedDigits:encoded};
  }
  if(id==="consensus"){
    if(!Array.isArray(input.votes)||input.votes.length!==3)throw new Error("Enter exactly three votes.");
    const votes=input.votes.map((x:unknown)=>bounded(x,-1,1));return {decision:votes.filter((x:number)=>x===1).length>=2?1:votes.filter((x:number)=>x===-1).length>=2?-1:0};
  }
  if(id==="orbit"){
    const colors=["moss","amber","violet"],start=colors.indexOf(input.color),steps=bounded(input.steps,-1000,1000);
    if(start<0)throw new Error("Choose moss, amber, or violet.");return {color:colors[((start+steps)%3+3)%3]};
  }
  throw new Error("This example provides source and recorded outputs for local use.");
}
