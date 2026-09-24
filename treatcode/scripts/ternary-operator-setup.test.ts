import { expect, test } from "bun:test";
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { AuthStore, DEFAULT_PROJECT_ID } from "../src/auth";

test("operator setup requires a local password, persists explicit grants, and never replaces identities",()=>{
  const root=fs.mkdtempSync(path.join(os.tmpdir(),"ternary-operator-setup-")),identityPath=path.join(root,"auth.json");
  const password="isolated-fixture-password-only";
  const setup=(handle:string,secret=password)=>spawnSync(process.execPath,[path.join(import.meta.dir,"setup-ternary-operator.ts"),handle],{
    encoding:"utf8",windowsHide:true,env:{...process.env,TREATCODE_AUTH_STATE_PATH:identityPath,TI_OPERATOR_PASSWORD:secret},timeout:10000,
  });
  try{
    expect(setup("fixture_operator","").status).not.toBe(0);expect(fs.existsSync(identityPath)).toBe(false);
    expect(setup("invalid-handle").status).not.toBe(0);expect(fs.existsSync(identityPath)).toBe(false);
    const created=setup("fixture_operator");if(created.status!==0)throw new Error(created.stderr.replaceAll(password,"[redacted]"));expect(created.status).toBe(0);expect(created.stdout+created.stderr).not.toContain(password);
    const auth=new AuthStore({identity_path:identityPath,audit_path:null});
    const login=auth.login({handle:"fixture_operator",password});expect(login.ok).toBe(true);
    if(!login.ok)throw new Error("Fixture operator cannot log in.");
    const identity=auth.identity("tc:identity:benchmark-fixture-operator")!;
    expect(identity.roles).toEqual(["benchmark-operator"]);
    for(const action of ["read","edit","benchmark","artifact"] as const)expect(auth.authorize({token:login.credential.token,action,project_id:DEFAULT_PROJECT_ID,require_nonce:false}).allowed).toBe(true);
    const before=fs.readFileSync(identityPath,"utf8");
    expect(setup("fixture_operator").status).not.toBe(0);expect(fs.readFileSync(identityPath,"utf8")).toBe(before);
    auth.registerParticipant({handle:"existing_person",password});
    const participantBefore=fs.readFileSync(identityPath,"utf8");
    expect(setup("existing_person").status).not.toBe(0);expect(fs.readFileSync(identityPath,"utf8")).toBe(participantBefore);
    expect(auth.identityForHandle("existing_person")?.roles).toEqual(["participant"]);
  }finally{fs.rmSync(root,{recursive:true,force:true});}
});
