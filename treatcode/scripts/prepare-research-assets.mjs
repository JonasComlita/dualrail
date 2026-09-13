import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const appRoot = path.resolve(scriptDirectory, "..");
const defaultReferenceRoot = "C:\\Users\\jonas\\Desktop\\etc\\references";
const referenceRoot = path.resolve(process.env.TREATCODE_RESEARCH_ROOT || defaultReferenceRoot);
const catalogPath = path.join(appRoot, "src", "content", "research", "catalog.json");
const outputDirectory = path.join(appRoot, "public", "research", "papers");

if (!fs.existsSync(catalogPath)) {
  console.warn("Research catalog is missing; run npm run generate:research before preparing the site.");
  process.exit(0);
}

if (!fs.existsSync(referenceRoot)) {
  console.warn(`Research source folder is unavailable at ${referenceRoot}; linked-only entries remain available.`);
  process.exit(0);
}

const catalog = JSON.parse(fs.readFileSync(catalogPath, "utf8"));
fs.mkdirSync(outputDirectory, { recursive: true });

let copied = 0;
let unchanged = 0;
let missing = 0;
for (const entry of catalog.records || []) {
  if (entry.status !== "local" || !entry.sourceFile || !entry.fileUrl) continue;
  const relativeSource = path.normalize(entry.sourceFile);
  const sourcePath = path.resolve(referenceRoot, relativeSource);
  const relativeCheck = path.relative(referenceRoot, sourcePath);
  if (relativeCheck.startsWith("..") || path.isAbsolute(relativeCheck)) {
    console.warn(`Skipping research file outside the reference folder: ${entry.sourceFile}`);
    missing += 1;
    continue;
  }
  if (!fs.existsSync(sourcePath)) {
    console.warn(`Research PDF is missing: ${entry.sourceFile}`);
    missing += 1;
    continue;
  }

  const destinationPath = path.join(outputDirectory, `${entry.id}.pdf`);
  const sourceStat = fs.statSync(sourcePath);
  const destinationStat = fs.existsSync(destinationPath) ? fs.statSync(destinationPath) : null;
  if (destinationStat && destinationStat.size === sourceStat.size && destinationStat.mtimeMs >= sourceStat.mtimeMs) {
    unchanged += 1;
    continue;
  }
  fs.copyFileSync(sourcePath, destinationPath);
  copied += 1;
}

console.log(`Research assets prepared: ${copied} copied, ${unchanged} unchanged, ${missing} missing.`);
