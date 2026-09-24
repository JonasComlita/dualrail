import { expect, test } from "bun:test";
import { renderToStaticMarkup } from "react-dom/server";
import { ComparisonView, LearningCurves } from "../src/ternary/report-views";
import type { Publication } from "../src/ternary/contracts";

test("learning curves expose numeric alternatives and give each family equal weight",()=>{
  // In-memory renderer inputs only: these are never inserted into the publication store.
  const p={models:[{id:"render-fixture"}],observations:[
    {modelConfigId:"render-fixture",condition:"learning",mode:"tools",familyId:"a",learning:[{round:0,passed:0,total:1},{round:1,passed:1,total:1},{round:3,passed:1,total:1}]},
    {modelConfigId:"render-fixture",condition:"learning",mode:"tools",familyId:"b",learning:[{round:0,passed:0,total:100},{round:1,passed:0,total:100},{round:3,passed:100,total:100}]},
  ]} as Publication;
  const html=renderToStaticMarkup(<LearningCurves publication={p}/>);
  expect(html).toContain('role="img"');expect(html).toContain("50.0% after 1 feedback rounds");expect(html).toContain("100.0% after 3 feedback rounds");expect(html).toContain("<caption>");
  expect(renderToStaticMarkup(<LearningCurves publication={{...p,observations:[]}}/>)).toContain("No learning episodes");
});

test("paired comparisons label direction, uncertainty and unavailable measurements",()=>{
  const html=renderToStaticMarkup(<ComparisonView comparison={{modelA:"a",modelB:"b",unit:"task family",samples:10000,confidence:.95,rows:[{condition:"prior",mode:"tools",families:12,difference:.1,interval:[-.1,.3]},{condition:"learning",mode:"model-only",families:12,difference:0,interval:null}]}}/>);
  expect(html).toContain("a minus b");expect(html).toContain("+10.0 pp");expect(html).toContain("-10.0 pp to +30.0 pp");expect(html).toContain("Unavailable");
});
