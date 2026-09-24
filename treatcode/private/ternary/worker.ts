import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { randomUUID } from "node:crypto";
import type { Limits } from "../../src/ternary/contracts";
import { hash, invariant, LabError } from "./util";

export interface CommandResult { exitCode: number; durationMs: number; peakMemoryBytes: number; reason: string | null; stdout: string; stderr: string }
export interface Case { id: string; args: [number, number, number]; expected: number }
export interface GradeResult { passed: boolean; passedCases: number; totalCases: number; category: string; programMs: number; peakMemoryBytes: number; details: string; privateDiagnostics?: { exitCode: number; stdout: string; stderr: string } }

export function checkSource(source: string): void {
  invariant(typeof source === "string" && Buffer.byteLength(source) <= 64 * 1024, "Source exceeds the 64 KiB file limit.");
  const code = source.replace(/\/\*[\s\S]*?\*\//g, " ").replace(/\/\/[^\n]*/g, " ");
  invariant(!/[#"'`\\]/.test(code), "This task accepts numeric Trit source without literals, directives, or imports.");
  invariant(!/\b(import|include|asm|extern|syscall|main|__\w*)\b/.test(code), "Source uses a reserved entry point or unsupported host feature.");
  const functions = new Set([...code.matchAll(/\bfn\s+([A-Za-z_]\w*)\s*\(/g)].map(m => m[1]));
  for (const name of functions) invariant(["solve","candidate","q","mod","p3","digit","sg","lo","hi","wrap"].includes(name)||/^local_[a-zA-Z0-9_]+$/.test(name), "Use local_ prefixes for additional helper functions; host builtin names cannot be declared.");
  invariant(functions.has("solve"), "Source must declare solve(a: t40, b: t40, c: t40) -> t40.");
  for (const m of code.matchAll(/\b([A-Za-z_]\w*)\s*\(/g)) invariant(functions.has(m[1]) || ["if", "while", "for", "return"].includes(m[1]), `Call to ${m[1]} is not allowlisted.`);
}

export class WindowsWorker {
  readonly executable: string;
  readonly compiler: string;
  readonly runtimeDirectory: string;
  constructor(readonly repositoryRoot: string, readonly workspaceRoot: string) {
    this.executable = path.join(repositoryRoot, "build/treatcode-ternary-worker.exe");
    this.compiler = path.join(repositoryRoot, "build/tritc.exe");
    const cache = path.join(repositoryRoot, "build/CMakeCache.txt");
    const toolchain = fs.existsSync(cache) ? fs.readFileSync(cache, "utf8").match(/^CMAKE_CXX_COMPILER:(?:STRING|FILEPATH)=(.+)$/m)?.[1].trim() : null;
    this.runtimeDirectory = toolchain && path.isAbsolute(toolchain) ? path.dirname(toolchain) : path.dirname(this.compiler);
  }
  readiness() {
    const errors = [];
    if (process.platform !== "win32") errors.push("Windows is required for this pilot.");
    if (!fs.existsSync(this.executable)) errors.push("Build the Windows worker with npm run build:ternary-worker.");
    if (!fs.existsSync(this.compiler)) errors.push("Build the repository tritc compiler.");
    const runtimeHashes = ["libgcc_s_seh-1.dll","libstdc++-6.dll","libwinpthread-1.dll"].map(name=>{const file=path.join(this.runtimeDirectory,name);return {name,hash:fs.existsSync(file)?hash(fs.readFileSync(file)):null};});
    return { ready: errors.length === 0, errors, workerHash: fs.existsSync(this.executable) ? hash({native:hash(fs.readFileSync(this.executable)),host:hash(fs.readFileSync(path.join(import.meta.dir,"worker.ts")))}) : null, compilerHash: fs.existsSync(this.compiler) ? hash({compiler:hash(fs.readFileSync(this.compiler)),runtimeHashes}) : null };
  }
  private safeDirectory(name: string): string {
    invariant(/^[a-zA-Z0-9-]+$/.test(name), "Invalid workspace name.");
    const parent = path.resolve(this.workspaceRoot), target = path.resolve(parent, name);
    invariant(target.startsWith(parent + path.sep), "Workspace escaped its root.");
    fs.mkdirSync(parent, { recursive: true });
    fs.mkdirSync(target, { recursive: false });
    return target;
  }
  async command(args: string[], cwd: string, limits: Limits, signal: AbortSignal): Promise<CommandResult> {
    if (signal.aborted) throw new LabError("cancelled", "Execution cancelled.");
    if (!this.readiness().ready) throw new LabError("worker_unavailable", this.readiness().errors.join(" "), 503);
    invariant(path.resolve(cwd).startsWith(path.resolve(this.workspaceRoot) + path.sep), "Invalid command directory.");
    const systemRoot = process.env.SystemRoot || "C:\\Windows";
    return new Promise((resolve, reject) => {
      const child = spawn(this.executable, [String(limits.commandMs), String(limits.memoryMiB), String(limits.processes), String(limits.outputBytes), cwd, this.compiler, ...args], {
        cwd, windowsHide: true, shell: false, stdio: ["ignore", "pipe", "pipe"],
        env: { SystemRoot: systemRoot, WINDIR: systemRoot, PATH: `${path.dirname(this.compiler)};${this.runtimeDirectory};${systemRoot}\\System32`, TEMP: cwd, TMP: cwd, LANG: "C", TZ: "UTC", TI_SUPERVISOR_PARENT_PID:String(process.pid) },
      });
      let output = "", diagnostics = "", tooLarge = false;
      const abort = () => child.kill();
      signal.addEventListener("abort", abort, { once: true });
      const watchdog = setTimeout(() => child.kill(), limits.commandMs + 6000);
      child.stdout.on("data", chunk => { if (output.length + chunk.length > limits.outputBytes * 6 + 8192) { tooLarge = true; child.kill(); } else output += chunk.toString(); });
      child.stderr.on("data", chunk => { diagnostics = (diagnostics + chunk.toString()).slice(0, 8192); });
      const cleanup = () => { clearTimeout(watchdog); signal.removeEventListener("abort", abort); };
      child.once("error", error => { cleanup(); reject(new LabError("worker_failure", error.message, 503)); });
      child.once("close", code => {
        cleanup();
        if (signal.aborted) return reject(new LabError("cancelled", "Execution cancelled."));
        if (code !== 0 || tooLarge) return reject(new LabError("worker_failure", "Windows supervisor failed: " + diagnostics, 503));
        try { const result = JSON.parse(output); if (result.infrastructureError) throw new Error(result.infrastructureError); resolve(result); }
        catch { reject(new LabError("worker_failure", "Windows supervisor returned invalid evidence.", 503)); }
      });
    });
  }
  async grade(source: string, cases: Case[], limits: Limits, signal: AbortSignal): Promise<GradeResult> {
    try { checkSource(source); } catch (e) { return { passed: false, passedCases:0, totalCases:cases.length, category: "invalid_submission", programMs: 0, peakMemoryBytes: 0, details: (e as Error).message }; }
    invariant(cases.length > 0 && cases.length <= 1000, "Invalid grading case count.");
    for (const c of cases) for (const n of [...c.args, c.expected]) invariant(Number.isSafeInteger(n) && Math.abs(n) <= 1_000_000_000, "Case exceeds exact integer limits.");
    // Hidden cases and checkers are compiled in a fresh private grading directory,
    // never in the participant file workspace and never returned by tools.
    const cwd = this.safeDirectory(randomUUID());
    const harness = `\nfn main() -> t40 {\nvar errors:t40=0;\n${cases.map(c => `if (solve(${c.args.join(",")}) != ${c.expected}) { errors=errors+1; }`).join("\n")}\nreturn errors;\n}\n`;
    fs.writeFileSync(path.join(cwd, "program.trit"), source + harness, { flag: "wx" });
    try {
      const compile = await this.command(["program.trit", "-O0", "-o", "program.txe"], cwd, limits, signal);
      if (compile.exitCode === 3221225781) throw new LabError("worker_failure", "Compiler runtime DLLs are unavailable.", 503);
      if (compile.reason || compile.exitCode !== 0) return { passed: false, passedCases:0, totalCases:cases.length, category: compile.reason || "compile_error", programMs: 0, peakMemoryBytes: compile.peakMemoryBytes, details: "Submission did not compile within the command limits.", privateDiagnostics: { exitCode: compile.exitCode, stdout: compile.stdout, stderr: compile.stderr } };
      const run = await this.command(["run", "program.txe", "--steps", "10000000", "--no-ansi", "--dump-registers"], cwd, limits, signal);
      const returned = run.stdout.match(/Return Register r13:\s*(-?\d+)/i);
      const status = run.stdout.match(/Final CPU Status:\s*(\S+)/i)?.[1];
      const passed = run.exitCode === 0 && !run.reason && returned?.[1] === "0" && status?.toUpperCase() === "HALTED";
      const count=Number(returned?.[1]);
      const measured=run.exitCode===0&&!run.reason&&status?.toUpperCase()==="HALTED"&&Number.isInteger(count)&&count>=0&&count<=cases.length;
      return { passed, passedCases:measured?cases.length-count:0,totalCases:cases.length, category: passed ? "correct" : run.reason || (measured ? "wrong_answer" : "runtime_failure"), programMs: run.durationMs, peakMemoryBytes: Math.max(compile.peakMemoryBytes, run.peakMemoryBytes), details: passed ? "All cases passed." : "Submission failed one or more cases." };
    } finally {
      const parent = path.resolve(this.workspaceRoot), target = path.resolve(cwd);
      invariant(target.startsWith(parent + path.sep), "Cleanup escaped grading root.");
      fs.rmSync(target, { recursive: true, force: true });
    }
  }
}
