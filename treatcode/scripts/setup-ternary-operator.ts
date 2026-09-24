import path from "node:path";
import { AuthStore } from "../src/auth";
const handle=process.argv[2];
if(!handle||!/^[a-zA-Z0-9_]{3,24}$/.test(handle))throw new Error("Usage: bun run scripts/setup-ternary-operator.ts <handle> (3–24 letters, digits, underscores)");
const password=process.env.TI_OPERATOR_PASSWORD;
if(!password||password.length<12||password.length>128)throw new Error("Set TI_OPERATOR_PASSWORD locally to a 12–128 character password before running setup.");
const repo=path.resolve(import.meta.dir,"../..");
const identityPath=process.env.TREATCODE_AUTH_STATE_PATH||path.join(repo,"build/treatcode-auth/state.json");
const store=new AuthStore({identity_path:identityPath,audit_path:null});
// Login handles allow underscores; identity IDs use hyphens in their slug.
const id=`tc:identity:benchmark-${handle.toLowerCase().replaceAll("_","-")}`;
if(store.identity(id))throw new Error("This operator identity already exists. Setup will not overwrite an account.");
store.registerIdentity({id,kind:"human",handle,display_name:`Benchmark operator ${handle}`,access_key:password,roles:["benchmark-operator"],grants:[{project_id:"*",actions:["read","edit","benchmark","artifact"]}]});
console.log(`Created benchmark operator ${handle}. Restart the server, then sign in at /account. Passwords and provider keys are not printed.`);
