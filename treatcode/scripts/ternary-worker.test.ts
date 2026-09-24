import { afterAll, beforeAll, expect, test } from "bun:test";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawn, spawnSync } from "node:child_process";
import { WindowsWorker } from "../private/ternary/worker";
import { DEFAULT_LIMITS } from "../src/ternary/contracts";
import { EXAMPLES } from "../src/ternary/public";
const repo = path.resolve(import.meta.dir, "../.."), root = fs.mkdtempSync(path.join(os.tmpdir(), "ternary-job-tests-"));
const fixture = path.join(root, "fixture.exe"), supervisor = path.join(repo, "build/treatcode-ternary-worker.exe");
beforeAll(() => {
  const compiler = fs.readFileSync(path.join(repo, "build/CMakeCache.txt"), "utf8").match(/^CMAKE_CXX_COMPILER:(?:FILEPATH|STRING)=(.+)$/m)![1].trim();
  const build = spawnSync(compiler, ["-std=c++17", "-O2", "-static", "-municode", path.join(import.meta.dir, "fixtures/ternary-job-fixture.cpp"), "-o", fixture], { windowsHide: true, encoding: "utf8" });
  if (build.status !== 0) throw new Error(build.stderr);
}, 30000);
afterAll(() => fs.rmSync(root, { recursive: true, force: true }));
function run(mode: string, time = 5000, memory = 64, processes = 8, bytes = 4096): any {
  const result = spawnSync(supervisor, [String(time), String(memory), String(processes), String(bytes), root, fixture, mode], { windowsHide: true, encoding: "utf8", timeout: time + 7000 });
  expect(result.status).toBe(0); return JSON.parse(result.stdout);
}
test("Job Objects enforce command time, bounded output, memory, and process count", () => {
  expect(run("sleep", 100).reason).toBe("timeout");
  const flooded = run("flood"); expect(flooded.reason).toBe("output_limit"); expect(flooded.stdout.length + flooded.stderr.length).toBeLessThanOrEqual(4096);
  const memory = run("memory", 5000, 32); expect(memory.stdout).toContain("allocation-denied:");
  // Windows' peak counter can include the denied allocation attempt. Verify that
  // successful committed blocks stay below the job cap, including runtime overhead.
  expect(Number(memory.stdout.split(":")[1])).toBeLessThan(32); expect(memory.peakMemoryBytes).toBeGreaterThan(0);
  expect(run("child", 5000, 64, 1).stdout).toBe("child-denied");
});
test("descendants cannot outlive a normally completed root", async () => {
  const result = run("child"); const pid = Number(result.stdout.match(/child-pid:(\d+)/)?.[1]); expect(pid).toBeGreaterThan(0);
  await new Promise(resolve => setTimeout(resolve, 100));
  let alive = false; try { process.kill(pid, 0); alive = true; } catch {}
  expect(alive).toBe(false);
});
test("aborting a native worker command closes its process tree", async () => {
  const worker = new WindowsWorker(repo, path.join(root, "work")); const controller = new AbortController();
  const grade = worker.grade("fn solve(a:t40,b:t40,c:t40)->t40{while(a==a){}return 0;}", [{ id: "loop", args: [1, 2, 3], expected: 0 }], { ...DEFAULT_LIMITS }, controller.signal);
  controller.abort(); await expect(grade).rejects.toMatchObject({ code: "cancelled" });
  expect(fs.readdirSync(worker.workspaceRoot)).toHaveLength(0);
});
test("supervisor stops its job when the server process exits", async () => {
  const watched=spawn(fixture,["sleep"],{windowsHide:true,stdio:"ignore"});
  await new Promise<void>((resolve,reject)=>{watched.once("spawn",resolve);watched.once("error",reject);});
  const supervisorProcess=spawn(supervisor,["30000","64","8","4096",root,fixture,"signal"],{windowsHide:true,env:{...process.env,TI_SUPERVISOR_PARENT_PID:String(watched.pid)},stdio:["ignore","pipe","pipe"]});
  let output="";supervisorProcess.stdout.on("data",chunk=>output+=String(chunk));
  const closed=new Promise<number|null>((resolve,reject)=>{supervisorProcess.once("close",resolve);supervisorProcess.once("error",reject);});
  try{
    const marker=path.join(root,"started.pid"),deadline=Date.now()+5000;
    while(!fs.existsSync(marker)&&Date.now()<deadline)await new Promise(resolve=>setTimeout(resolve,20));
    expect(fs.existsSync(marker)).toBe(true);
    const pid=Number(fs.readFileSync(marker,"utf8"));expect(pid).toBeGreaterThan(0);
    watched.kill();expect(await closed).toBe(0);expect(JSON.parse(output).reason).toBe("parent_exit");
    let alive=false;try{process.kill(pid,0);alive=true;}catch{}expect(alive).toBe(false);
  }finally{watched.kill();supervisorProcess.kill();}
},15000);
test("public coding examples reproduce their recorded outputs on the fixed compiler", async () => {
  const worker = new WindowsWorker(repo, path.join(root, "public-examples"));
  for (const [id, expected] of [["median", 2], ["clamp", -8], ["alignment", 18]] as const) {
    const cwd = path.join(worker.workspaceRoot, id); fs.mkdirSync(cwd, { recursive: true }); const example = EXAMPLES.find(e => e.id === id)!;
    fs.writeFileSync(path.join(cwd, "program.trit"), example.source!);
    const compile = await worker.command(["program.trit", "-O0", "-o", "program.txe"], cwd, { ...DEFAULT_LIMITS }, new AbortController().signal); expect(compile.exitCode).toBe(0);
    const result = await worker.command(["run", "program.txe", "--no-ansi", "--dump-registers"], cwd, { ...DEFAULT_LIMITS }, new AbortController().signal);
    expect(Number(result.stdout.match(/Return Register r13:\s*(-?\d+)/)?.[1])).toBe(expected);
  }
}, 15000);
