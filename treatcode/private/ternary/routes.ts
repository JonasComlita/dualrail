import { Router, type Request, type Response } from "express";
import type { AuthStore, PermissionAction } from "../../src/auth";
import { bearerToken, DEFAULT_PROJECT_ID } from "../../src/auth";
import { TI_SCHEMA } from "../../src/ternary/contracts";
import { EXAMPLES, METHODOLOGY } from "../../src/ternary/public";
import { IntelligenceLab } from "./service";
import { compare } from "./reporting";
import { invariant, LabError } from "./util";

export function intelligenceRouter(lab:IntelligenceLab,auth:AuthStore){
  const router=Router();
  const send=(res:Response,data:unknown,status=200)=>res.status(status).json({schema_version:TI_SCHEMA,data});
  const failure=(res:Response,error:unknown)=>{const e=error instanceof LabError?error:new LabError("internal_error","The benchmark operation could not be completed.",500);res.status(e.status).json({schema_version:TI_SCHEMA,error:{code:e.code,reason:e.message}});};
  router.use((_req,res,next)=>{res.setHeader("Cache-Control","no-store");next();});
  router.get("/overview",(_req,res)=>send(res,lab.publicOverview()));
  router.get("/methodology",(_req,res)=>send(res,METHODOLOGY));
  router.get("/examples",(_req,res)=>send(res,EXAMPLES));
  router.get("/publications",(_req,res)=>send(res,lab.store.publications()));
  router.get("/comparisons",(req,res)=>{try{
    const all=lab.store.publications(),a=all.find(p=>p.id===req.query.a),b=all.find(p=>p.id===req.query.b);invariant(a&&b,"Select two published results.");
    send(res,compare(a,String(req.query.modelA||""),b,String(req.query.modelB||"")));
  }catch(error){failure(res,error);}});
  // A role is required in addition to the ordinary action grant. Public participant
  // registration and existing demo identities never receive this role.
  const operator=(req:Request,res:Response,action:PermissionAction,mutating=false)=>{
    const decision=auth.authorize({token:bearerToken(req.header("authorization")),project_id:DEFAULT_PROJECT_ID,task_id:null,action,
      request_nonce:req.header("x-action-nonce"),require_nonce:mutating,resource:req.path});
    if(!decision.allowed){res.status(decision.denial.http_status).json({schema_version:TI_SCHEMA,error:{code:decision.denial.code,reason:decision.denial.reason}});return null;}
    if(!decision.actor.roles.includes("benchmark-operator")){res.status(403).json({schema_version:TI_SCHEMA,error:{code:"operator_required",reason:"An explicit benchmark-operator role is required."}});return null;}
    return decision.actor;
  };
  const get=(url:string,fn:(req:Request)=>unknown)=>router.get(url,(req,res)=>{if(!operator(req,res,"read"))return;try{send(res,fn(req));}catch(error){failure(res,error);}});
  get("/operator/readiness",()=>lab.readiness());
  get("/operator/configuration",()=>lab.settings());
  get("/operator/runs",()=>lab.store.runs().map(r=>lab.detail(r.id)));
  get("/operator/runs/:id",req=>lab.detail(req.params.id));
  get("/operator/runs/:id/events",req=>{lab.store.run(req.params.id);const after=Number(req.query.after||0);invariant(Number.isSafeInteger(after)&&after>=0,"Invalid event cursor.");return lab.store.events(req.params.id,after);});
  get("/operator/artifacts/:hash",req=>lab.store.artifact(req.params.hash));
  const post=(url:string,action:PermissionAction,fn:(req:Request,owner:string)=>unknown|Promise<unknown>)=>router.post(url,async(req,res)=>{
    const actor=operator(req,res,action,true);if(!actor)return;
    try{send(res,await fn(req,actor.id));}catch(error){failure(res,error);}
  });
  post("/operator/configuration","edit",req=>{lab.configure(req.body);return lab.readiness();});
  post("/operator/models/:id/verify","benchmark",req=>lab.verifyModel(req.params.id));
  post("/operator/preview","read",req=>lab.preview({...req.body,spendingCeilingUsd:req.body.spendingCeilingUsd??null}));
  post("/operator/runs","benchmark",(req,owner)=>lab.create({...req.body,spendingCeilingUsd:req.body.spendingCeilingUsd??null},owner,req.header("idempotency-key")||""));
  post("/operator/runs/:id/cancel","benchmark",req=>lab.cancel(req.params.id));
  post("/operator/runs/:id/publish","artifact",req=>lab.publish(req.params.id));
  post("/operator/freeze","artifact",()=>lab.freeze());
  post("/operator/calibration-review","artifact",(req,owner)=>lab.reviewAmbiguity(req.body,owner));
  post("/operator/qualify","benchmark",()=>lab.qualify());
  router.use((_req,res)=>res.status(404).json({schema_version:TI_SCHEMA,error:{code:"not_found",reason:"Unknown benchmark route."}}));
  return router;
}
