import fs from "node:fs";
import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { deflateRawSync } from "node:zlib";

export const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
export const repoRoot = path.resolve(appRoot, "..");
export const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P11");

export async function loadContributionModule() {
  const sourcePath = path.join(appRoot, "src", "contributionService.ts");
  const runtimeRoot = path.join(evidenceRoot, "runtime", `${process.pid}-${Date.now()}-${Math.random().toString(36).slice(2)}`);
  fs.mkdirSync(runtimeRoot, { recursive: true });
  const tscPath = path.join(appRoot, "node_modules", ".bin", process.platform === "win32" ? "tsc.exe" : "tsc");
  const result = spawnSync(tscPath, [
    "--ignoreConfig",
    "--target", "ES2020",
    "--module", "Node16",
    "--moduleResolution", "Node16",
    "--skipLibCheck",
    "--esModuleInterop",
    "--types", "node",
    "--typeRoots", path.join(appRoot, "node_modules", "@types"),
    "--rootDir", path.join(appRoot, "src"),
    "--outDir", runtimeRoot,
    sourcePath,
  ], { encoding: "utf8" });
  if (result.status !== 0) throw new Error(`P11 service compilation failed:\n${result.stdout || ""}\n${result.stderr || ""}`);
  const runtimePath = path.join(runtimeRoot, "contributionService.js");
  const require = createRequire(import.meta.url);
  return require(runtimePath);
}

export function assert(condition, message) {
  if (!condition) throw new Error(message);
}

export async function expectFailure(action, expectedCode) {
  try {
    await action();
  } catch (error) {
    assert(error && error.code === expectedCode, `expected ${expectedCode}, received ${error?.code || error}`);
    return error;
  }
  throw new Error(`expected ${expectedCode} to be thrown`);
}

export function actor(module, overrides = {}) {
  const scopes = overrides.scopes || module.CONTRIBUTION_ACTIONS.filter((scope) => scope !== "merge");
  return {
    actorId: "agent-p11",
    actorType: "agent",
    taskId: "task-p11-e2e",
    projectId: "project-trit",
    scopes: [...scopes],
    ...overrides,
  };
}

export function sha256(bytes) {
  return `sha256:${createHash("sha256").update(Buffer.from(bytes)).digest("hex")}`;
}

export function writeReport(fileName, schema, checks, errors = [], extra = {}) {
  fs.mkdirSync(evidenceRoot, { recursive: true });
  const report = {
    schema,
    ok: errors.length === 0,
    generated_at: new Date().toISOString(),
    checks,
    errors,
    ...extra,
  };
  const outputPath = path.join(evidenceRoot, fileName);
  fs.writeFileSync(outputPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
  console.log(`${schema}: ${report.ok ? "passed" : "failed"}`);
  for (const check of checks) console.log(`  [ok] ${check}`);
  for (const error of errors) console.error(`  [fail] ${error}`);
  return report;
}

const CRC_TABLE = (() => {
  const table = [];
  for (let index = 0; index < 256; index += 1) {
    let value = index;
    for (let bit = 0; bit < 8; bit += 1) value = (value & 1) ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
    table[index] = value >>> 0;
  }
  return table;
})();

function crc32(bytes) {
  let value = 0xffffffff;
  for (const byte of bytes) value = CRC_TABLE[(value ^ byte) & 0xff] ^ (value >>> 8);
  return (value ^ 0xffffffff) >>> 0;
}

function u16(value) {
  return Buffer.from([value & 0xff, (value >>> 8) & 0xff]);
}

function u32(value) {
  return Buffer.from([value & 0xff, (value >>> 8) & 0xff, (value >>> 16) & 0xff, (value >>> 24) & 0xff]);
}

export function buildZip(entries) {
  const localParts = [];
  const centralParts = [];
  let offset = 0;
  for (const entry of entries) {
    const name = Buffer.from(entry.name, "utf8");
    const raw = Buffer.isBuffer(entry.data) ? entry.data : Buffer.from(entry.data);
    const method = entry.method ?? 8;
    const compressed = method === 0 ? raw : deflateRawSync(raw);
    const checksum = crc32(raw);
    const local = Buffer.concat([
      u32(0x04034b50), u16(20), u16(0), u16(method), u16(0), u16(0), u32(checksum), u32(compressed.length), u32(raw.length), u16(name.length), u16(0), name, compressed,
    ]);
    localParts.push(local);
    const central = Buffer.concat([
      u32(0x02014b50), u16(20), u16(20), u16(0), u16(method), u16(0), u16(0), u32(checksum), u32(compressed.length), u32(raw.length), u16(name.length), u16(0), u16(0), u16(0), u16(0), u32(entry.externalAttributes || 0), u32(offset), name,
    ]);
    centralParts.push(central);
    offset += local.length;
  }
  const central = Buffer.concat(centralParts);
  const locals = Buffer.concat(localParts);
  const end = Buffer.concat([u32(0x06054b50), u16(0), u16(0), u16(entries.length), u16(entries.length), u32(central.length), u32(locals.length), u16(0)]);
  return Buffer.concat([locals, central, end]);
}
