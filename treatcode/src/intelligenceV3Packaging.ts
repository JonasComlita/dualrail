import { createHash } from "node:crypto";
import { lstat, readdir, readFile, realpath } from "node:fs/promises";
import path from "node:path";

export const INTELLIGENCE_V3_PACKAGE_AUDIT_SCHEMA = "treatcode.intelligence.package-audit.v3" as const;

export interface IntelligenceV3PackageAuditInput {
  task_id: string;
  participant_root: string;
  grader_root: string;
  /** The complete filesystem tree exposed to the benchmark subject. */
  subject_workspace_root?: string;
  maximum_file_bytes?: number;
}

export interface IntelligenceV3PackageFile {
  path: string;
  bytes: number;
  sha256: string;
}

export interface IntelligenceV3PackageAudit {
  schema: typeof INTELLIGENCE_V3_PACKAGE_AUDIT_SCHEMA;
  task_id: string;
  passed: boolean;
  participant_root: string;
  grader_root: string;
  participant_files: IntelligenceV3PackageFile[];
  grader_files: IntelligenceV3PackageFile[];
  participant_bundle_hash: string;
  grader_bundle_hash: string;
  issues: string[];
}

const FORBIDDEN_PARTICIPANT_PATH = /(?:^|\/)(?:hidden(?:\.|\/|$)|graders?(?:\.|\/|$)|oracles?(?:\.|\/|$)|references?[-_.]?solutions?(?:\.|\/|$)|answers?(?:\.|\/|$))/i;

function slash(value: string): string {
  return value.replace(/\\/g, "/");
}

function rootsOverlap(left: string, right: string): boolean {
  const a = slash(path.resolve(left)).replace(/\/+$/, "").toLowerCase();
  const b = slash(path.resolve(right)).replace(/\/+$/, "").toLowerCase();
  return a === b || a.startsWith(`${b}/`) || b.startsWith(`${a}/`);
}

function sha256(value: Buffer | string): string {
  return createHash("sha256").update(value).digest("hex");
}

async function collectFiles(root: string, maximumFileBytes: number, issues: string[], participant: boolean): Promise<IntelligenceV3PackageFile[]> {
  const files: IntelligenceV3PackageFile[] = [];
  const visit = async (directory: string): Promise<void> => {
    const entries = await readdir(directory, { withFileTypes: true });
    entries.sort((left, right) => left.name.localeCompare(right.name));
    for (const entry of entries) {
      const absolute = path.join(directory, entry.name);
      const relative = slash(path.relative(root, absolute));
      const metadata = await lstat(absolute);
      if (metadata.isSymbolicLink()) {
        issues.push(`${participant ? "participant" : "grader"} package contains symlink: ${relative}`);
        continue;
      }
      if (entry.isDirectory()) {
        await visit(absolute);
        continue;
      }
      if (!entry.isFile()) {
        issues.push(`${participant ? "participant" : "grader"} package contains unsupported entry: ${relative}`);
        continue;
      }
      if (participant && FORBIDDEN_PARTICIPANT_PATH.test(`/${relative}`)) issues.push(`participant package exposes a reserved grader/reference path: ${relative}`);
      if (metadata.size > maximumFileBytes) issues.push(`${participant ? "participant" : "grader"} file exceeds ${maximumFileBytes} bytes: ${relative}`);
      const content = await readFile(absolute);
      files.push({ path: relative, bytes: content.byteLength, sha256: sha256(content) });
    }
  };
  await visit(root);
  return files;
}

function bundleHash(files: IntelligenceV3PackageFile[]): string {
  return sha256(files.map((file) => `${file.path}\0${file.bytes}\0${file.sha256}\n`).join(""));
}

export async function auditIntelligenceV3TaskPackage(input: IntelligenceV3PackageAuditInput): Promise<IntelligenceV3PackageAudit> {
  if (!/^[A-Za-z0-9._-]+$/.test(input.task_id)) throw new Error("Invalid v3 task id");
  const maximumFileBytes = input.maximum_file_bytes ?? 1_048_576;
  if (!Number.isSafeInteger(maximumFileBytes) || maximumFileBytes <= 0) throw new Error("maximum_file_bytes must be a positive safe integer");
  const [participantRoot, graderRoot] = await Promise.all([realpath(input.participant_root), realpath(input.grader_root)]);
  const issues: string[] = [];
  if (rootsOverlap(participantRoot, graderRoot)) issues.push("grader root must be outside the participant root");
  if (input.subject_workspace_root) {
    const subjectWorkspaceRoot = await realpath(input.subject_workspace_root);
    if (rootsOverlap(subjectWorkspaceRoot, graderRoot)) issues.push("grader root must be outside the complete subject workspace");
    if (!rootsOverlap(subjectWorkspaceRoot, participantRoot)) issues.push("participant root must be inside the declared subject workspace");
  }
  const [participantFiles, graderFiles] = await Promise.all([
    collectFiles(participantRoot, maximumFileBytes, issues, true),
    collectFiles(graderRoot, maximumFileBytes, issues, false),
  ]);
  if (participantFiles.length === 0) issues.push("participant package is empty");
  if (graderFiles.length === 0) issues.push("grader package is empty");
  return {
    schema: INTELLIGENCE_V3_PACKAGE_AUDIT_SCHEMA,
    task_id: input.task_id,
    passed: issues.length === 0,
    participant_root: participantRoot,
    grader_root: graderRoot,
    participant_files: participantFiles,
    grader_files: graderFiles,
    participant_bundle_hash: bundleHash(participantFiles),
    grader_bundle_hash: bundleHash(graderFiles),
    issues,
  };
}
