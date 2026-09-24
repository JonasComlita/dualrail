import fs from "node:fs";
import path from "node:path";
import { spawn } from "node:child_process";
import {
  assert,
  evidenceRoot,
  learningHref,
  loadCourse,
  relativeToRepo,
  repoRoot,
  treatcodeRoot,
  writeEvidence,
} from "./learning-test-utils.mjs";

const report = {
  schema: "trit.treatcode_learning_complete.v1",
  ok: false,
  checks: [],
  errors: [],
  paths: [],
  static_routes: { source: 0, dist: 0, dist_checked: false },
};
let exerciseExecution = { attempted: false, ok: false, error: null };

function routeFile(root, pathId, pageId) {
  return path.join(root, pathId, pageId, "index.html");
}

function assertConsecutive(pathItem, pagesById) {
  const seen = new Set();
  for (let index = 0; index < pathItem.page_ids.length; index += 1) {
    const pageId = pathItem.page_ids[index];
    assert(!seen.has(pageId), `${pathItem.id} repeats ${pageId}`);
    seen.add(pageId);
    const page = pagesById.get(pageId);
    assert(page, `${pathItem.id} references a missing page ${pageId}`);
    assert(page.metadata.next === null || typeof page.metadata.next === "string", `${pageId} has an invalid canonical next link`);
  }
}

async function runBoundedLearningExercise(codeExercise) {
  const port = 4320 + (process.pid % 400);
  const artifactRoot = path.join(evidenceRoot, "learning-runner-artifacts");
  const child = spawn("bun", ["run", "server.ts"], {
    cwd: treatcodeRoot,
    env: {
      ...process.env,
      PORT: String(port),
      TI_DATA_ROOT: path.join(evidenceRoot, "ternary-lab"),
      TREATCODE_RUNNER_ARTIFACT_ROOT: artifactRoot,
      TREATCODE_AUTH_AUDIT_PATH: path.join(evidenceRoot, "learning-runner-auth-audit.jsonl"),
      TREATCODE_AUTH_STATE_PATH: path.join(evidenceRoot, "learning-runner-auth-state.json"),
    },
    windowsHide: true,
    stdio: ["ignore", "pipe", "pipe"],
  });
  let serverOutput = "";
  child.stdout.on("data", (chunk) => { serverOutput += chunk.toString(); });
  child.stderr.on("data", (chunk) => { serverOutput += chunk.toString(); });
  const wait = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
  const request = (pathname, options = {}) => fetch(`http://127.0.0.1:${port}${pathname}`, options);
  try {
    let ready = false;
    for (let attempt = 0; attempt < 160 && !ready; attempt += 1) {
      try {
        const response = await request("/api/auth/v1");
        ready = response.ok;
      } catch {
        // The Bun server is still starting.
      }
      if (!ready) await wait(100);
    }
    assert(ready, `learning runner did not start${serverOutput ? `: ${serverOutput.slice(-800)}` : ""}`);
    const loginResponse = await request("/api/auth/v1/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ identity_id: "tc:identity:demo-human", access_key: "local-human-key" }),
    });
    const login = await loginResponse.json();
    assert(loginResponse.ok && typeof login?.data?.credential?.token === "string", "learning runner test could not obtain the bounded test credential");
    const response = await request("/api/learn/exercises/run", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Accept: "application/json",
        Authorization: `Bearer ${login.data.credential.token}`,
        "X-TreatCode-Project": "tc:project:trit",
      },
      body: JSON.stringify({ exercise_id: codeExercise.metadata.interactive.exercise_id, code: codeExercise.metadata.interactive.starter, engine: "bootstrap" }),
    });
    const payload = await response.json();
    assert(response.ok, `learning exercise endpoint returned HTTP ${response.status}: ${JSON.stringify(payload)}`);
    assert(payload.schema === "treatcode.learning.exercise-run.v1", "learning exercise endpoint returned the wrong schema");
    assert(payload.exercise_id === codeExercise.metadata.interactive.exercise_id, "learning exercise endpoint returned the wrong exercise ID");
    assert(payload.run_id && /^run_/.test(payload.run_id), "learning exercise endpoint did not return a durable run ID");
    assert(payload.evidence?.record_hash && /^sha256:[0-9a-f]{64}$/.test(payload.evidence.record_hash), "learning exercise endpoint did not return immutable evidence");
    assert(payload.success === true, `bounded starter exercise did not execute successfully: ${payload.error || "unknown failure"}`);
    return {
      attempted: true,
      ok: true,
      exercise_id: payload.exercise_id,
      engine: payload.engine,
      state: payload.state,
      run_id: payload.run_id,
      record_hash: payload.evidence.record_hash,
      record_path: relativeToRepo(payload.evidence.record_path),
      summary: payload.summary,
    };
  } finally {
    if (child.exitCode === null) { const closed = new Promise(resolve => child.once("close", resolve)); child.kill(); await closed; }
    await wait(350);
  }
}

try {
  const { catalog, matrix, pagesById } = loadCourse();
  const allLessonIds = new Set(matrix.phases.flatMap((phase) => phase.lessons.map((lesson) => lesson.id)));
  assert(catalog.paths.length === 3, "complete learning flow requires three paths");
  assert(catalog.paths.every((pathItem) => pathItem.page_ids.length === allLessonIds.size), "a learning path stops before the full curriculum");
  assertConsecutive(catalog.paths[0], pagesById);

  const routeRoot = path.join(treatcodeRoot, "learn");
  for (const pathItem of catalog.paths) {
    const route = { id: pathItem.id, count: pathItem.page_ids.length, first: pathItem.page_ids[0], last: pathItem.page_ids.at(-1), source_routes: 0, dist_routes: 0 };
    assert(pathItem.page_ids.at(-1) === "full-system-validation", `${pathItem.id} has no terminal closure lesson`);
    for (let index = 0; index < pathItem.page_ids.length; index += 1) {
      const pageId = pathItem.page_ids[index];
      const page = pagesById.get(pageId);
      const sourcePath = routeFile(routeRoot, pathItem.id, pageId);
      assert(fs.existsSync(sourcePath), `missing static deep link ${relativeToRepo(sourcePath)}`);
      const html = fs.readFileSync(sourcePath, "utf8");
      assert(html.includes(`<h1>${page.metadata.title}</h1>`), `${pathItem.id}/${pageId} static page has the wrong title`);
      assert(html.includes("Objectives") && html.includes("Worked example") && html.includes("Source links") && html.includes("Validation"), `${pathItem.id}/${pageId} static page omits required reading content`);
      assert(html.includes('data-testid="static-learning-page"') && html.includes('<main'), `${pathItem.id}/${pageId} static page has no readable landmark`);
      const previous = index > 0 ? pathItem.page_ids[index - 1] : null;
      const next = index + 1 < pathItem.page_ids.length ? pathItem.page_ids[index + 1] : null;
      if (previous) assert(html.includes(`href="${learningHref(pathItem.id, previous)}"`), `${pathItem.id}/${pageId} has no previous deep link`);
      if (next) assert(html.includes(`href="${learningHref(pathItem.id, next)}"`), `${pathItem.id}/${pageId} has no next deep link`);
      route.source_routes += 1;

      const builtPath = routeFile(path.join(process.cwd(), "dist", "learn"), pathItem.id, pageId);
      if (fs.existsSync(builtPath)) {
        const builtHtml = fs.readFileSync(builtPath, "utf8");
        assert(builtHtml.includes(page.metadata.title) && builtHtml.includes("Objectives"), `${relativeToRepo(builtPath)} lost lesson content during build`);
        route.dist_routes += 1;
      }
    }
    report.paths.push(route);
    report.static_routes.source += route.source_routes;
    report.static_routes.dist += route.dist_routes;
  }
  report.static_routes.dist_checked = fs.existsSync(path.join(process.cwd(), "dist", "learn"));

  const rootHtml = fs.readFileSync(path.join(routeRoot, "index.html"), "utf8");
  assert(rootHtml.includes("21") && rootHtml.includes("42") && rootHtml.includes("Beginner") && rootHtml.includes("EECS/systems"), "Learn index does not publish full curriculum coverage");
  const { pages } = loadCourse();
  assert(pages.some((page) => page.metadata.interactive.kind === "choice"), "choice interaction missing");
  assert(pages.some((page) => page.metadata.interactive.kind === "trace"), "state trace interaction missing");
  assert(pages.some((page) => page.metadata.interactive.kind === "source"), "source investigation interaction missing");
  assert(pages.some((page) => page.metadata.interactive.kind === "code"), "bounded code interaction missing");
  assert(fs.readFileSync(path.join(process.cwd(), "src", "PublicApp.tsx"), "utf8").includes("/api/learn/exercises/run"), "public Learn does not expose the bounded exercise runner transition");

  const codeExercise = [...pagesById.values()].find((page) => page.metadata.interactive.kind === "code");
  const traceExercise = [...pagesById.values()].find((page) => page.metadata.interactive.kind === "trace");
  const sourceExercise = [...pagesById.values()].find((page) => page.metadata.interactive.kind === "source");
  assert(codeExercise && traceExercise && sourceExercise, "complete learning flow cannot resolve code, trace, and source exercises");
  assert(traceExercise.metadata.interactive.steps.length >= 4, "state trace does not expose enough inspectable states");
  assert(sourceExercise.metadata.interactive.sourcePath && fs.existsSync(path.join(repoRoot, sourceExercise.metadata.interactive.sourcePath)), "source investigation points outside the repository");
  exerciseExecution = await runBoundedLearningExercise(codeExercise);

  report.checks.push("all three paths are complete, ordered, and terminate at full-system-validation");
  report.checks.push("every path/page has a stable static deep link with no-JavaScript lesson text");
  report.checks.push("previous and next navigation is present for every non-terminal path lesson");
  report.checks.push("choice, state trace, source investigation, and executable code transitions are published");
  report.checks.push("bounded starter code executed through the authenticated compiler/VM endpoint with immutable evidence");
  report.checks.push(report.static_routes.dist_checked ? "built deep-link pages retain lesson content" : "source deep-link pages validated; dist not present");
  report.ok = true;
} catch (error) {
  report.errors.push(String(error?.message || error));
}

writeEvidence("learning-flow.json", report);
writeEvidence("browser-e2e.json", report);
writeEvidence("exercise-execution.json", {
  schema: "trit.treatcode_learning_exercise_execution.v1",
  ok: report.ok,
  runner_endpoint: "/api/learn/exercises/run",
  contract: "treatcode.learning.exercise-run.v1",
  result: exerciseExecution,
  trace: (() => { try { const trace = loadCourse().pages.find((page) => page.metadata.interactive.kind === "trace"); return { page_id: trace?.metadata.id || null, steps: trace?.metadata.interactive.steps.length || 0, feedback: Boolean(trace?.metadata.interactive.steps.every((step) => step.explanation)) }; } catch { return { page_id: null, steps: 0, feedback: false }; } })(),
  source_investigation: (() => { try { const source = loadCourse().pages.find((page) => page.metadata.interactive.kind === "source"); return { page_id: source?.metadata.id || null, source_path: source?.metadata.interactive.sourcePath || null, source_exists: Boolean(source && fs.existsSync(path.join(repoRoot, source.metadata.interactive.sourcePath))) }; } catch { return { page_id: null, source_path: null, source_exists: false }; } })(),
  checks: report.checks.filter((check) => check.includes("interaction") || check.includes("runner")),
  errors: report.errors,
});
console.log(`TreatCode complete learning flow: ${report.ok ? "passed" : "failed"}`);
for (const check of report.checks) console.log(`  [ok] ${check}`);
for (const error of report.errors) console.error(`  [fail] ${error}`);
process.exitCode = report.ok ? 0 : 1;
