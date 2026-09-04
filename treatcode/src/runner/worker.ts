import fs from "node:fs";
import { readFile, writeFile, mkdir } from "node:fs/promises";
import path from "node:path";
import {
  DEFAULT_RUN_LIMITS,
  EXECUTION_REQUEST_SCHEMA,
  RUNNER_PROTOCOL_VERSION,
  type BoundedCommandResult,
  type RunLimits,
  type SafeCommand,
  type TritExecutionRequest,
  type WorkerOutput,
  createSanitizedEnvironment,
  normalizeRunLimits,
  sanitizeCommand,
  spawnBounded,
  stableJson,
  validateExecutionRequest,
} from "./secure-runner";

interface WorkerEnvelope {
  protocol: typeof RUNNER_PROTOCOL_VERSION;
  request: TritExecutionRequest;
  workspaceRoot: string;
  repositoryRoot: string;
  limits: RunLimits;
}

interface ParsedVmResult {
  cycles: number | null;
  status: string | null;
  r13: number | null;
  consoleOutput: string;
  registers: Record<string, number>;
}

function readStdin(): Promise<string> {
  return new Promise((resolve, reject) => {
    const chunks: string[] = [];
    let length = 0;
    process.stdin.setEncoding("utf8");
    process.stdin.on("data", (chunk: string) => {
      length += chunk.length;
      if (length > 768 * 1024) {
        reject(new Error("worker request exceeds the protocol input limit"));
        process.stdin.destroy();
        return;
      }
      chunks.push(chunk);
    });
    process.stdin.once("end", () => resolve(chunks.join("")));
    process.stdin.once("error", reject);
  });
}

function parseVmOutput(stdout: string): ParsedVmResult {
  const cyclesMatch = stdout.match(/Total CPU Cycles:\s*(\d+)/i);
  const statusMatch = stdout.match(/Final CPU Status:\s*(\S+)/i);
  const r13Match = stdout.match(/Return Register r13:\s*(-?\d+)/i);
  let consoleOutput = "";
  const consoleHeaderIndex = stdout.indexOf("Console Output:");
  if (consoleHeaderIndex !== -1) {
    const afterHeader = stdout.substring(consoleHeaderIndex + "Console Output:".length);
    let startIndex = 0;
    while (startIndex < afterHeader.length && (afterHeader[startIndex] === "\r" || afterHeader[startIndex] === "\n")) startIndex += 1;
    let endIndex = afterHeader.length;
    const returnRegIndex = afterHeader.indexOf("Return Register");
    if (returnRegIndex !== -1) endIndex = returnRegIndex;
    else {
      const dividerIndex = afterHeader.indexOf("====");
      if (dividerIndex !== -1) endIndex = dividerIndex;
    }
    consoleOutput = afterHeader.substring(startIndex, endIndex).replace(/[\s\r\n]+$/, "");
  }
  const registers: Record<string, number> = {};
  for (const match of stdout.matchAll(/^\s*r(\d+)(?:\s*\(sp\))?:\s*(-?\d+)/gm)) {
    registers[`r${match[1]}`] = Number.parseInt(match[2], 10);
  }
  return {
    cycles: cyclesMatch ? Number.parseInt(cyclesMatch[1], 10) : null,
    status: statusMatch ? statusMatch[1] : null,
    r13: r13Match ? Number.parseInt(r13Match[1], 10) : null,
    consoleOutput,
    registers,
  };
}

function safeSource(code: string): string {
  return code.replace(/^\s*import\b[^;]*;\s*$/gm, "// import removed by the isolated runner").replace(/\bpub\s+fn\b/g, "fn");
}

function findCompiler(repositoryRoot: string): string {
  const candidates = process.platform === "win32"
    ? [path.join(repositoryRoot, "build", "tritc.exe"), path.join(repositoryRoot, "build", "tritc")]
    : [path.join(repositoryRoot, "build", "tritc"), path.join(repositoryRoot, "build", "tritc.exe")];
  const compiler = candidates.find((candidate) => fs.existsSync(candidate));
  if (!compiler) throw new Error("the fixed Trit compiler binary is unavailable");
  return compiler;
}

function displayCommand(command: SafeCommand, workspaceRoot: string, repositoryRoot: string): SafeCommand {
  return sanitizeCommand(command, workspaceRoot, repositoryRoot);
}

function currentPeakMemoryKib(): number | null {
  try {
    const usage = typeof process.resourceUsage === "function" ? process.resourceUsage() : null;
    return usage && typeof usage.maxRSS === "number" && Number.isFinite(usage.maxRSS)
      ? Math.max(0, Math.round(usage.maxRSS))
      : null;
  } catch {
    return null;
  }
}

function peakMemoryKib(commandResults: BoundedCommandResult[]): number | null {
  const sampled = commandResults
    .map((result) => result.peakMemoryKib)
    .filter((value): value is number => typeof value === "number" && Number.isFinite(value));
  return sampled.length ? Math.max(...sampled) : currentPeakMemoryKib();
}

function executionRuntimeMs(commandResults: BoundedCommandResult[]): number | null {
  const last = commandResults[commandResults.length - 1];
  return last && Number.isFinite(last.durationMs) ? Math.max(0, Math.round(last.durationMs)) : null;
}

function emptyOutput(commands: SafeCommand[], commandResults: BoundedCommandResult[], error: string, terminationReason?: WorkerOutput["terminationReason"]): WorkerOutput {
  return {
    success: false,
    runtimeMs: executionRuntimeMs(commandResults),
    peakMemoryKib: peakMemoryKib(commandResults),
    compilerOutput: "",
    stdout: "",
    stderr: "",
    error,
    commands,
    commandResults,
    workerPid: process.pid,
    terminationReason,
  };
}

export async function executeTritRequest(envelope: WorkerEnvelope): Promise<WorkerOutput> {
  if (envelope.protocol !== RUNNER_PROTOCOL_VERSION) throw new Error("unsupported worker protocol");
  validateExecutionRequest(envelope.request);
  const limits = normalizeRunLimits(envelope.limits || DEFAULT_RUN_LIMITS);
  const workspaceRoot = path.resolve(envelope.workspaceRoot);
  const repositoryRoot = path.resolve(envelope.repositoryRoot);
  if (!path.isAbsolute(envelope.workspaceRoot) || !path.isAbsolute(envelope.repositoryRoot)) throw new Error("worker paths must be absolute");
  await mkdir(workspaceRoot, { recursive: true });

  const sourcePath = path.join(workspaceRoot, "submission.trit");
  const txePath = path.join(workspaceRoot, "submission.txe");
  const tasmPath = path.join(workspaceRoot, "submission.tasm");
  if (!path.relative(workspaceRoot, sourcePath) || path.relative(workspaceRoot, sourcePath).startsWith("..")) throw new Error("source path escaped workspace");
  const ulibPath = path.join(repositoryRoot, "ulib_mini.trit");
  const driverCandidates = [
    path.join(repositoryRoot, "treatcode", "compiler_driver.txe"),
    path.join(repositoryRoot, "compiler_driver.txe"),
    path.join(path.dirname(repositoryRoot), "treatcode", "compiler_driver.txe"),
  ];
  const driverPath = driverCandidates.find((candidate) => fs.existsSync(candidate));
  if (!driverPath) throw new Error("the fixed compiler driver image is unavailable");
  const compiler = findCompiler(repositoryRoot);
  const ulib = await readFile(ulibPath, "utf8");
  await writeFile(sourcePath, `${ulib}\n${safeSource(envelope.request.code)}`, { encoding: "utf8", mode: 0o600 });

  const environment = createSanitizedEnvironment(workspaceRoot, [path.join(repositoryRoot, "build")]).values;
  const commands: SafeCommand[] = [];
  const commandResults: BoundedCommandResult[] = [];
  const stdoutParts: string[] = [];
  const stderrParts: string[] = [];
  const compilerOutputParts: string[] = [];

  const run = async (executable: string, args: string[]): Promise<BoundedCommandResult> => {
    const command: SafeCommand = { executable, args, cwd: workspaceRoot };
    commands.push(displayCommand(command, workspaceRoot, repositoryRoot));
    const result = await spawnBounded(command, {
      env: environment,
      limits: { ...limits, wallTimeMs: Math.min(limits.wallTimeMs, limits.cpuMs) },
    });
    commandResults.push(result);
    stdoutParts.push(result.stdout);
    stderrParts.push(result.stderr);
    compilerOutputParts.push(result.stdout, result.stderr);
    return result;
  };

  const failFor = (result: BoundedCommandResult, label: string): WorkerOutput => emptyOutput(
    commands,
    commandResults,
    result.terminationReason === "timeout"
      ? `${label} exceeded the CPU/wall-time limit`
      : result.terminationReason === "output-limit"
        ? `${label} exceeded the output limit`
        : `${label} failed${result.spawnError ? `: ${result.spawnError}` : ` with exit code ${result.code}`}`,
    result.terminationReason,
  );

  let assembly = "";
  let compileTimeCycles = 0;
  if (envelope.request.engine === "bootstrap") {
    const compile = await run(compiler, [sourcePath, "-o", txePath, envelope.request.optLevel]);
    if (compile.code !== 0 || compile.signal || compile.terminationReason || compile.spawnError) return failFor(compile, "bootstrap compilation");
    const asm = await run(compiler, [sourcePath, "-S", envelope.request.optLevel]);
    if (asm.code === 0 && fs.existsSync(tasmPath)) assembly = await readFile(tasmPath, "utf8");
  } else {
    const nativeCompile = await run(compiler, [
      "run",
      driverPath,
      "--imem",
      "262144",
      "--dmem",
      "16777216",
      "--seed-file",
      sourcePath,
      "1600000",
      "--steps",
      "200000000",
      "--no-ansi",
    ]);
    if (nativeCompile.code !== 0 || nativeCompile.signal || nativeCompile.terminationReason || nativeCompile.spawnError) return failFor(nativeCompile, "native compiler");
    const parsedCompiler = parseVmOutput(nativeCompile.stdout);
    compileTimeCycles = parsedCompiler.cycles || 0;
    assembly = parsedCompiler.consoleOutput || "";
    if (!assembly.includes("_start") && !assembly.includes("RET")) {
      return {
        ...emptyOutput(commands, commandResults, "native compiler did not produce a valid assembly image"),
        compilerOutput: compilerOutputParts.join(""),
        stdout: stdoutParts.join(""),
        stderr: stderrParts.join(""),
        compileTimeCycles,
        assembly,
      };
    }
    await writeFile(tasmPath, assembly, { encoding: "utf8", mode: 0o600 });
    const assemble = await run(compiler, [tasmPath, "-o", txePath]);
    if (assemble.code !== 0 || assemble.signal || assemble.terminationReason || assemble.spawnError) return failFor(assemble, "native assembly");
  }

  const execution = await run(compiler, ["run", txePath, "--steps", "1000000", "--no-ansi", "--dump-registers"]);
  if (execution.code !== 0 || execution.signal || execution.terminationReason || execution.spawnError) return failFor(execution, "VM execution");
  const parsed = parseVmOutput(execution.stdout);
  return {
    success: true,
    runtimeMs: executionRuntimeMs(commandResults),
    peakMemoryKib: peakMemoryKib(commandResults),
    compileTimeCycles,
    assembly,
    cycles: parsed.cycles,
    status: parsed.status,
    r13: parsed.r13,
    consoleOutput: parsed.consoleOutput,
    registers: parsed.registers,
    compilerOutput: compilerOutputParts.join(""),
    stdout: stdoutParts.join(""),
    stderr: stderrParts.join(""),
    commands,
    commandResults,
    workerPid: process.pid,
  };
}

async function main(): Promise<void> {
  const raw = await readStdin();
  const envelope = JSON.parse(raw) as WorkerEnvelope;
  if (envelope.request?.schema !== EXECUTION_REQUEST_SCHEMA) throw new Error("invalid worker request");
  const result = await executeTritRequest(envelope);
  process.stdout.write(`${stableJson(result)}\n`);
}

if (process.argv[1] && path.basename(process.argv[1]) === "worker.ts") {
  main().catch((error) => {
    process.stderr.write(`${String(error)}\n`);
    process.exitCode = 1;
  });
}
