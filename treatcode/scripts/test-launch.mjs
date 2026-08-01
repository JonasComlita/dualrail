import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repoRoot = path.resolve(appRoot, "..");
const evidenceRoot = path.join(repoRoot, "build", "treatcode-plan-evidence", "P13");
const launchReportPath = path.join(evidenceRoot, "launch-readiness.json");
const fullVerificationPath = path.join(evidenceRoot, "full-verification.json");
const publicApiRoot = path.join(appRoot, "public", "api", "v1");
const publicResources = [
  "projects",
  "stack_nodes",
  "components",
  "capabilities",
  "contracts",
  "decisions",
  "sources",
  "symbols",
  "tests",
  "benchmarks",
  "runs",
  "releases",
  "gaps",
];

const checks = [];
const blockers = [];

function relative(filePath) {
  return path.relative(repoRoot, filePath).replaceAll(path.sep, "/");
}

function readText(relativePath) {
  try {
    return fs.readFileSync(path.join(repoRoot, relativePath), "utf8");
  } catch {
    return null;
  }
}

function readJson(relativePath) {
  const source = readText(relativePath);
  if (source === null) return null;
  try {
    return JSON.parse(source);
  } catch {
    return null;
  }
}

function hashFile(filePath) {
  if (!fs.existsSync(filePath) || !fs.statSync(filePath).isFile()) return null;
  return `sha256:${crypto.createHash("sha256").update(fs.readFileSync(filePath)).digest("hex")}`;
}

function assertCheck(id, ok, message, details = {}) {
  const result = { id, ok: Boolean(ok), message, ...details };
  checks.push(result);
  if (!ok) blockers.push({ id, message, ...details });
  return Boolean(ok);
}

function currentCommit(fallback = "unknown") {
  try {
    return execFileSync("git", ["rev-parse", "HEAD"], { cwd: repoRoot, encoding: "utf8" }).trim() || fallback;
  } catch {
    return fallback;
  }
}

function sortedObject(value) {
  return Object.fromEntries(Object.entries(value || {}).sort(([left], [right]) => left.localeCompare(right)));
}

function dependencyReadiness(manifest, p13) {
  const plans = new Map((Array.isArray(manifest?.plans) ? manifest.plans : []).map((plan) => [plan.id, plan]));
  const dependencies = {};
  for (const dependencyId of Array.isArray(p13?.depends_on) ? p13.depends_on : []) {
    const plan = plans.get(dependencyId);
    const completion = plan?.completion_record && typeof plan.completion_record === "object" ? plan.completion_record : {};
    const evidenceReference = completion.evidence_artifact || `build/treatcode-plan-evidence/${dependencyId}/result.json`;
    const evidencePath = path.join(repoRoot, evidenceReference);
    const result = readJson(evidenceReference);
    const complete = plan?.status === "complete"
      && Boolean(completion.verified_commit)
      && Boolean(completion.date)
      && fs.existsSync(evidencePath)
      && result?.complete === true
      && result?.verification_ok === true;
    dependencies[dependencyId] = {
      status: plan?.status || "missing",
      verified_commit: completion.verified_commit || null,
      evidence: evidenceReference,
      evidence_exists: fs.existsSync(evidencePath),
      result_complete: result?.complete === true,
      result_verification_ok: result?.verification_ok === true,
      ready: complete,
    };
  }
  const allReady = Object.values(dependencies).length > 0 && Object.values(dependencies).every((item) => item.ready);
  return { dependencies, allReady };
}

const requiredDocuments = [
  "docs/11_TreatCode_Platform/launch/LAUNCH_READINESS_MATRIX.md",
  "docs/11_TreatCode_Platform/launch/DEPLOYMENT_ROLLBACK_INCIDENT_SUPPORT.md",
  "docs/11_TreatCode_Platform/launch/POLICIES.md",
  "docs/11_TreatCode_Platform/launch/RELEASE_RECORD.md",
  "docs/11_TreatCode_Platform/launch/reports/PERFORMANCE.md",
  "docs/11_TreatCode_Platform/launch/reports/SECURITY.md",
  "docs/11_TreatCode_Platform/launch/reports/RECOVERY.md",
  "docs/11_TreatCode_Platform/launch/reports/FULL_SYSTEM.md",
];

const manifest = readJson("TREATCODE_PLAN_MANIFEST.json");
const plans = Array.isArray(manifest?.plans) ? manifest.plans : [];
const p13 = plans.find((plan) => plan?.id === "P13");
const readiness = dependencyReadiness(manifest, p13);

assertCheck(
  "dependency-completion",
  readiness.allReady,
  readiness.allReady
    ? "P04-P12 have complete, evidence-backed completion records"
    : "P04-P12 must all be complete with verified evidence before launch",
  { dependencies: readiness.dependencies },
);

const indexSource = readText("docs/11_TreatCode_Platform/PLAN_INDEX.md") || "";
const indexRow = indexSource.split(/\r?\n/).find((line) => /^\|\s*P13\s*\|/.test(line));
const indexCells = indexRow ? indexRow.split("|").map((cell) => cell.trim()) : [];
assertCheck(
  "plan-index-consistency",
  Boolean(p13) && indexCells[4] === p13.status,
  "P13 status in the plan index matches the manifest",
  { manifest_status: p13?.status || "missing", index_status: indexCells[4] || "missing" },
);

const documentRecords = requiredDocuments.map((documentPath) => {
  const content = readText(documentPath);
  const unresolved = content === null ? [] : [...content.matchAll(/\b(?:TBD|TODO|FIXME|lorem ipsum)\b|\?\?\?/gi)].map((match) => match[0]);
  return {
    path: documentPath,
    exists: content !== null,
    unresolved,
    sha256: hashFile(path.join(repoRoot, documentPath)),
  };
});
assertCheck(
  "launch-artifacts",
  documentRecords.every((record) => record.exists && record.unresolved.length === 0),
  "launch matrix, procedures, policies, release record, and final reports exist without unresolved placeholders",
  { artifacts: documentRecords },
);

const snapshot = readJson("treatcode/public/api/v1/snapshot.json");
const snapshotCommit = snapshot?.snapshot?.commit || "unknown";
const snapshotShapeOk = Boolean(snapshot)
  && snapshot.schema_version === "treatcode.public.snapshot.v1"
  && /^[0-9a-f]{7,64}$/i.test(snapshotCommit)
  && typeof snapshot.snapshot?.repository === "string"
  && snapshot.snapshot.repository.startsWith("https://");
const computedStatistics = {};
let allEntitiesHaveProvenance = true;
for (const resource of publicResources) {
  const records = Array.isArray(snapshot?.[resource]) ? snapshot[resource] : [];
  for (const record of records) {
    const entityType = record?.entity_type || resource.replace(/s$/, "");
    computedStatistics[entityType] = (computedStatistics[entityType] || 0) + 1;
    if (!record?.id?.startsWith("tc:") || !Array.isArray(record.source_refs) || record.source_refs.length === 0) {
      allEntitiesHaveProvenance = false;
    }
  }
}
computedStatistics.relations = Array.isArray(snapshot?.relations) ? snapshot.relations.length : 0;
const statisticsMatch = JSON.stringify(sortedObject(snapshot?.statistics)) === JSON.stringify(sortedObject(computedStatistics));
assertCheck(
  "public-snapshot-authority",
  snapshotShapeOk && allEntitiesHaveProvenance && statisticsMatch,
  "public snapshot has commit provenance, stable ids, source references, and computed statistics",
  { snapshot_commit: snapshotCommit, statistics: snapshot?.statistics || {}, computed_statistics: computedStatistics },
);

const capabilitiesHaveLabels = Array.isArray(snapshot?.capabilities)
  && snapshot.capabilities.every((record) => ["status", "maturity_status", "evidence_status", "compatibility_status"].every((field) => typeof record?.[field] === "string" && record[field].length > 0));
const contractsHaveLabels = Array.isArray(snapshot?.contracts)
  && snapshot.contracts.every((record) => ["contract_status", "maturity_status", "evidence_status", "compatibility_status"].every((field) => typeof record?.[field] === "string" && record[field].length > 0));
const releasesHaveLabels = Array.isArray(snapshot?.releases)
  && snapshot.releases.every((record) => typeof record?.status === "string" && record.status.length > 0);
assertCheck(
  "public-status-labels",
  capabilitiesHaveLabels && contractsHaveLabels && releasesHaveLabels,
  "public capabilities, contracts, and releases carry explicit status labels",
);

const serverSource = readText("treatcode/server.ts") || "";
const leaderboardMatch = serverSource.match(/const\s+leaderboard\s*:\s*LeaderboardEntry\[\]\s*=\s*\[([\s\S]*?)\];/);
const leaderboardBody = leaderboardMatch ? leaderboardMatch[1].replace(/\/\/[^\r\n]*/g, "").trim() : "not-found";
const publicAppSource = readText("treatcode/src/PublicApp.tsx") || "";
const noSeededPublicData = leaderboardBody === ""
  && !serverSource.includes("TritWizard")
  && !serverSource.includes("balanced_0xff")
  && !/statistics\s*:\s*\{\s*stack_node\s*:\s*3\s*\}/.test(publicAppSource);
assertCheck(
  "no-seeded-public-data",
  noSeededPublicData,
  "runtime leaderboard and public fallback do not expose seeded demo statistics",
  { leaderboard_declaration_found: Boolean(leaderboardMatch), leaderboard_body: leaderboardBody },
);

const dependencyGates = [
  ["challenge-contracts", "P06", "published challenges have validated correctness contracts"],
  ["secure-execution", "P09", "public execution has isolation and abuse evidence"],
  ["contribution-boundary", "P11", "remote contributions preserve review boundaries"],
  ["performance", "P10", "performance evidence is reproducible and approved"],
  ["mobile-recovery", "P12", "mobile operations and recovery evidence is complete"],
];
for (const [id, dependencyId, message] of dependencyGates) {
  assertCheck(id, readiness.dependencies[dependencyId]?.ready === true, message, { dependency: dependencyId });
}

const releaseRecord = readText("docs/11_TreatCode_Platform/launch/RELEASE_RECORD.md") || "";
const explicitDecision = /\*\*(?:NO-GO|GO)\*\*/.test(releaseRecord);
const decisionReady = !readiness.allReady ? explicitDecision : /\*\*GO\*\*/.test(releaseRecord);
assertCheck(
  "explicit-release-decision",
  decisionReady,
  readiness.allReady ? "release record changes to GO only after all dependency gates pass" : "release record contains an explicit current go/no-go decision",
);

const artifactPaths = [
  "TREATCODE_PLAN_MANIFEST.json",
  "TEST_MANIFEST.json",
  "treatcode/package.json",
  "treatcode/public/api/v1/snapshot.json",
  ...requiredDocuments,
];
const artifactRecords = artifactPaths.map((artifactPath) => ({
  path: artifactPath,
  exists: fs.existsSync(path.join(repoRoot, artifactPath)),
  sha256: hashFile(path.join(repoRoot, artifactPath)),
}));

const report = {
  schema: "trit.treatcode.launch_readiness.v1",
  plan_id: "P13",
  verified_commit: currentCommit(snapshotCommit),
  snapshot_commit: snapshotCommit,
  ok: blockers.length === 0,
  checks,
  dependencies: readiness.dependencies,
  blockers,
  artifacts: artifactRecords,
  generated_at: new Date().toISOString(),
};

fs.mkdirSync(evidenceRoot, { recursive: true });
fs.writeFileSync(launchReportPath, `${JSON.stringify(report, null, 2)}\n`);

const fullVerification = {
  schema: "trit.treatcode.full_verification_bundle.v1",
  plan_id: "P13",
  verified_commit: report.verified_commit,
  launch_report: relative(launchReportPath),
  launch_report_sha256: hashFile(launchReportPath),
  artifacts: artifactRecords,
  check_ids: checks.map((check) => ({ id: check.id, ok: check.ok })),
  generated_at: report.generated_at,
};
fs.writeFileSync(fullVerificationPath, `${JSON.stringify(fullVerification, null, 2)}\n`);

console.log(`P13 launch readiness: ${report.ok ? "passed" : "blocked"}`);
for (const check of checks) console.log(`  [${check.ok ? "ok" : "fail"}] ${check.id}: ${check.message}`);
console.log(`evidence: ${relative(launchReportPath)}`);
process.exitCode = report.ok ? 0 : 1;

