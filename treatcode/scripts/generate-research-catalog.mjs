import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const appRoot = path.resolve(scriptDirectory, "..");
const defaultReferenceRoot = "C:\\Users\\jonas\\Desktop\\etc\\references";
const referenceRoot = path.resolve(process.env.TREATCODE_RESEARCH_ROOT || defaultReferenceRoot);
const manifestPath = path.join(referenceRoot, "MASTER_MANIFEST.csv");
const unavailablePath = path.join(referenceRoot, "MASTER_NOT_DOWNLOADED.csv");
const outputPath = path.join(appRoot, "src", "content", "research", "catalog.json");

function parseCsv(text) {
  const rows = [];
  let row = [];
  let field = "";
  let quoted = false;

  for (let index = 0; index < text.length; index += 1) {
    const character = text[index];
    const next = text[index + 1];
    if (character === '"') {
      if (quoted && next === '"') {
        field += '"';
        index += 1;
      } else {
        quoted = !quoted;
      }
    } else if (character === "," && !quoted) {
      row.push(field);
      field = "";
    } else if ((character === "\n" || character === "\r") && !quoted) {
      if (character === "\r" && next === "\n") index += 1;
      row.push(field);
      field = "";
      if (row.some((value) => value.length > 0)) rows.push(row);
      row = [];
    } else {
      field += character;
    }
  }

  if (field.length > 0 || row.length > 0) {
    row.push(field);
    if (row.some((value) => value.length > 0)) rows.push(row);
  }

  if (rows.length === 0) return [];
  const headers = rows[0].map((header) => header.trim());
  return rows.slice(1).map((values) => Object.fromEntries(headers.map((header, index) => [header, (values[index] || "").trim()])));
}

function readRows(filePath) {
  return fs.existsSync(filePath) ? parseCsv(fs.readFileSync(filePath, "utf8")) : [];
}

function value(row, key) {
  return String(row[key] || "").trim();
}

function localEntry(row, index) {
  const id = `local-${String(index + 1).padStart(3, "0")}`;
  return {
    id,
    collection: value(row, "collection") || "uncategorized",
    status: "local",
    title: value(row, "title") || value(row, "local_file"),
    authors: value(row, "authors"),
    year: value(row, "year"),
    venue: value(row, "venue_or_publisher"),
    kind: value(row, "kind"),
    identifier: value(row, "doi_or_identifier"),
    sourceFile: value(row, "local_file"),
    fileUrl: `/research/papers/${id}.pdf`,
    sourceUrl: value(row, "source_url"),
    landingUrl: value(row, "landing_url"),
    notes: value(row, "notes"),
    duplicateOf: value(row, "duplicate_of") || null,
  };
}

function externalEntry(row, index) {
  const id = `external-${String(index + 1).padStart(3, "0")}`;
  return {
    id,
    collection: value(row, "collection") || "uncategorized",
    status: "external",
    title: value(row, "title"),
    authors: value(row, "authors"),
    year: value(row, "year"),
    venue: value(row, "venue_or_publisher"),
    kind: value(row, "kind"),
    identifier: value(row, "doi_or_identifier"),
    sourceFile: null,
    fileUrl: null,
    sourceUrl: value(row, "source_url"),
    landingUrl: value(row, "landing_url"),
    notes: [value(row, "reason"), value(row, "notes")].filter(Boolean).join(" "),
    duplicateOf: null,
  };
}

if (!fs.existsSync(manifestPath) || !fs.existsSync(unavailablePath)) {
  console.error(`Research manifests were not found under ${referenceRoot}.`);
  console.error("Set TREATCODE_RESEARCH_ROOT to the folder containing MASTER_MANIFEST.csv and MASTER_NOT_DOWNLOADED.csv.");
  process.exit(1);
}

const localRows = readRows(manifestPath).filter((row) => value(row, "file_status") === "valid_pdf");
const externalRows = readRows(unavailablePath).filter((row) => value(row, "status") === "not_downloaded");
const records = [
  ...localRows.map(localEntry),
  ...externalRows.map(externalEntry),
];

fs.mkdirSync(path.dirname(outputPath), { recursive: true });
fs.writeFileSync(outputPath, `${JSON.stringify({
  schemaVersion: "treatcode.research.catalog.v1",
  localCount: localRows.length,
  externalCount: externalRows.length,
  records,
}, null, 2)}\n`);

console.log(`Research catalog written to ${path.relative(appRoot, outputPath)} (${localRows.length} local PDFs, ${externalRows.length} linked-only records).`);
