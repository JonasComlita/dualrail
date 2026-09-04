import fs from "node:fs";
import path from "node:path";
import { execFile, spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const viteCli = path.join(appRoot, "node_modules", "vite", "bin", "vite.js");
const apiPort = process.env.TREATCODE_API_PORT || "3000";
const apiUrl = `http://127.0.0.1:${apiPort}/api/public/v1/snapshot.json`;
const bunCommand = process.env.BUN_BIN || (process.platform === "win32" ? "bun.exe" : "bun");
const children = new Set();

if (!fs.existsSync(viteCli)) {
  throw new Error(`Vite is not installed at ${viteCli}. Run npm install in treatcode first.`);
}

let shuttingDown = false;

function stopChild(child) {
  if (child && child.exitCode === null && !child.killed) {
    if (process.platform === "win32") {
      execFile(
        "taskkill",
        ["/PID", String(child.pid), "/T", "/F"],
        { windowsHide: true, stdio: "ignore" },
        () => {},
      );
    } else {
      child.kill("SIGTERM");
    }
  }
}

function shutdown(code = 0) {
  process.exitCode = code;
  if (shuttingDown) return;
  shuttingDown = true;
  for (const child of children) stopChild(child);
}

function startChild(label, command, args, env) {
  const child = spawn(command, args, {
    cwd: appRoot,
    env,
    stdio: "inherit",
    windowsHide: true,
  });
  children.add(child);
  child.on("error", (error) => {
    console.error(`[dev] ${label} failed to start: ${error.message}`);
    shutdown(1);
  });
  child.on("exit", (code, signal) => {
    children.delete(child);
    if (shuttingDown) return;
    const status = signal ? `signal ${signal}` : `code ${code ?? "unknown"}`;
    console.error(`[dev] ${label} stopped with ${status}; stopping the other dev process.`);
    shutdown(code === 0 ? 0 : 1);
  });
  return child;
}

async function waitForApi(child) {
  const deadline = Date.now() + 15_000;
  let lastFailure = "no response";

  while (Date.now() < deadline) {
    if (child.exitCode !== null) {
      throw new Error(`API server exited before becoming ready (code ${child.exitCode ?? "unknown"}).`);
    }

    try {
      const response = await fetch(apiUrl, { signal: AbortSignal.timeout(500) });
      if (response.ok) return;
      lastFailure = `HTTP ${response.status}`;
    } catch (error) {
      lastFailure = error instanceof Error ? error.message : String(error);
    }

    await new Promise((resolve) => setTimeout(resolve, 100));
  }

  throw new Error(`API server did not become ready at ${apiUrl} (${lastFailure}).`);
}

process.on("SIGINT", () => shutdown(130));
process.on("SIGTERM", () => shutdown(143));

const apiProcess = startChild("API server", bunCommand, ["run", "server.ts"], {
  ...process.env,
  PORT: apiPort,
});

try {
  await waitForApi(apiProcess);
  console.log(`[dev] API server is ready at http://127.0.0.1:${apiPort}`);

  startChild("Vite", process.execPath, [viteCli], {
    ...process.env,
    TREATCODE_API_PORT: apiPort,
  });

  await new Promise(() => {});
} catch (error) {
  console.error(`[dev] ${error instanceof Error ? error.message : String(error)}`);
  shutdown(1);
}
