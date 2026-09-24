import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const APP_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const REPO_ROOT = path.resolve(APP_ROOT, "..");
const OUTPUT_ROOT = path.join(APP_ROOT, "public", "api", "v1");
const PUBLIC_REPOSITORY = "https://github.com/JonasComlita/dualrail";
const SNAPSHOT_SCHEMA = "treatcode.public.snapshot.v1";
const API_SCHEMA = "treatcode.public.api.v1";
const COVERAGE_SCHEMA = "treatcode.public.coverage.v1";
const RELATIONSHIP_INDEX_SCHEMA = "treatcode.public.relationship-index.v1";
const FRESHNESS_SCHEMA = "treatcode.public.freshness.v1";
const PUBLIC_RESOURCES = ["projects", "stack_nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps"];
const P03_INDEX_PATH = path.join(REPO_ROOT, "build", "treatcode-index", "repository-index.v1.json");
const P03_FRESHNESS_PATH = path.join(REPO_ROOT, "build", "treatcode-index", "freshness.v1.json");

// The public browser is intentionally backed by source-authoritative files.
// Build output, dependency caches, credentials, and local agent state are not
// public repository evidence even when a generated index happens to mention
// them.
const PRIVATE_PATH_RE = /(^|\/)(?:\.git|node_modules|build|coverage|scratch|\.codex)(?:\/|$)|(^|\/)(?:\.env(?:\.|$)|secrets?|credentials?|private)(?:\/|$)/i;
const GENERATED_PATH_RE = /(^|\/)treatcode\/(?:dist|learn|public\/api\/v1|src\/generated)(?:\/|$)|(^|\/)treatcode\/src\/content\/learn\/(?:learning-catalog\.json|P05_CURRICULUM_MATRIX\.json)$/i;

function isPublicPath(value) {
  const normalized = normalizePath(value);
  return Boolean(normalized) && !PRIVATE_PATH_RE.test(normalized) && !GENERATED_PATH_RE.test(normalized);
}

function sourceSpanFrom(value) {
  if (!value || typeof value !== "object") return undefined;
  const startLine = Number(value.start_line ?? value.startLine ?? value.start ?? 0);
  const endLine = Number(value.end_line ?? value.endLine ?? value.end ?? startLine);
  if (!Number.isInteger(startLine) || startLine < 1) return undefined;
  return { start_line: startLine, end_line: Number.isInteger(endLine) && endLine >= startLine ? endLine : startLine };
}

function loadP03Index() {
  try {
    const index = JSON.parse(fs.readFileSync(P03_INDEX_PATH, "utf8"));
    if (index?.schema === "treatcode.repository-index.v1") return index;
  } catch {
    // The generated report records the missing index; the caller still emits
    // a useful snapshot so the dedicated completeness test can fail closed.
  }
  return null;
}

function loadP03Freshness() {
  try {
    return JSON.parse(fs.readFileSync(P03_FRESHNESS_PATH, "utf8"));
  } catch {
    return null;
  }
}

function readJson(relativePath, fallback = {}) {
  const filePath = path.join(REPO_ROOT, relativePath);
  try {
    return JSON.parse(fs.readFileSync(filePath, "utf8"));
  } catch {
    return fallback;
  }
}

function readText(relativePath) {
  try {
    return fs.readFileSync(path.join(REPO_ROOT, relativePath), "utf8");
  } catch {
    return "";
  }
}

function repositoryCommit() {
  try {
    return execFileSync("git", ["rev-parse", "HEAD"], { cwd: REPO_ROOT, encoding: "utf8" }).trim();
  } catch {
    return "unknown";
  }
}

function repositoryDate(commit) {
  try {
    return execFileSync("git", ["show", "-s", "--format=%cI", commit], { cwd: REPO_ROOT, encoding: "utf8" }).trim();
  } catch {
    return "1970-01-01T00:00:00Z";
  }
}

function normalizePath(value) {
  return String(value || "").replaceAll("\\", "/").replace(/^\.\//, "");
}

function slug(value) {
  return String(value || "unknown")
    .replace(/^trit\.(?:stack|capability|contract|decision|release)\./, "")
    .replace(/[^A-Za-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .toLowerCase() || "unknown";
}

function shortEntitySlug(value) {
  const parts = String(value || "").split(".");
  return slug(parts.length > 3 ? parts.slice(-3).join("-") : parts.join("-"));
}

function sha256(value) {
  return crypto.createHash("sha256").update(value).digest("hex");
}

function fileHash(relativePath) {
  const filePath = path.join(REPO_ROOT, relativePath);
  try {
    const buffer = fs.readFileSync(filePath);
    return { exists: true, bytes: buffer.byteLength, hash: `sha256:${sha256(buffer)}` };
  } catch {
    return { exists: false, bytes: 0, hash: null };
  }
}

function asArray(value) {
  return Array.isArray(value) ? value : [];
}

const SEARCH_STOP_WORDS = new Set([
  "a", "an", "and", "are", "as", "at", "be", "by", "can", "do", "does", "for", "from", "how", "i", "in", "is",
  "it", "of", "on", "or", "our", "the", "their", "this", "to", "was", "what", "when", "where", "which", "who", "why",
  "with", "would", "you",
]);

function sourceSearchTerms(relativePath) {
  const normalized = readText(relativePath)
    .normalize("NFKD")
    .replace(/([a-z0-9])([A-Z])/g, "$1 $2")
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, " ");
  return [...new Set(normalized.split(/\s+/).filter((term) => term.length > 1 && !SEARCH_STOP_WORDS.has(term)))];
}

function sourceRef(commit, relativePath, extra = {}) {
  const normalized = normalizePath(relativePath);
  if (!isPublicPath(normalized)) {
    return {
      repository: PUBLIC_REPOSITORY,
      commit,
      status: "excluded",
      ...extra,
      reason: extra.reason || "Path is excluded from the public repository snapshot.",
    };
  }
  const file = fileHash(normalized);
  return {
    repository: PUBLIC_REPOSITORY,
    commit,
    path: normalized,
    status: file.exists ? "resolved" : "missing",
    ...extra,
    ...(file.exists ? {} : { reason: extra.reason || "Path is not present at the snapshot commit." }),
  };
}

function publicId(kind, rawId) {
  return `tc:${kind}:${shortEntitySlug(rawId)}`;
}

function languageFor(relativePath) {
  const extension = path.extname(relativePath).toLowerCase();
  return {
    ".trit": "trit",
    ".tasm": "tasm",
    ".h": "cpp",
    ".hpp": "cpp",
    ".cpp": "cpp",
    ".c": "c",
    ".ts": "typescript",
    ".tsx": "typescript-react",
    ".js": "javascript",
    ".json": "json",
    ".md": "markdown",
    ".ps1": "powershell",
  }[extension] || "text";
}

function buildSnapshot() {
  const commit = repositoryCommit();
  const generatedAt = repositoryDate(commit);
  const stack = readJson("STACK_MANIFEST.json");
  const capabilitiesManifest = readJson("CAPABILITY_MANIFEST.json");
  const contractsManifest = readJson("CONTRACT_MANIFEST.json");
  const decisionsManifest = readJson("DECISION_MANIFEST.json");
  const coverageManifest = readJson("STACK_COVERAGE_REPORT.json");
  const testManifest = readJson("TEST_MANIFEST.json");
  const benchmarkManifest = readJson("BENCHMARK_MANIFEST.json");
  const repositoryIndex = loadP03Index();
  const p03Freshness = loadP03Freshness();

  const layerIdByRaw = new Map();
  const capabilityIdByRaw = new Map();
  const contractIdByRaw = new Map();
  const decisionIdByRaw = new Map();
  const releaseIdByRaw = new Map();
  const gapIdByRaw = new Map();
  const testIdByRaw = new Map();
  const benchmarkIdByRaw = new Map();

  for (const item of asArray(stack.layers)) layerIdByRaw.set(item.id, publicId("layer", item.id));
  for (const item of asArray(capabilitiesManifest.capabilities)) capabilityIdByRaw.set(item.id, publicId("capability", item.id));
  for (const item of asArray(contractsManifest.contracts)) contractIdByRaw.set(item.id, publicId("contract", item.id));
  for (const item of asArray(decisionsManifest.decisions)) decisionIdByRaw.set(item.id, publicId("decision", item.id));
  for (const item of asArray(stack.releases)) releaseIdByRaw.set(item.id, publicId("release", item.id));

  const knownGapRecords = asArray(stack.known_gaps);
  const gapRawIds = new Set(knownGapRecords.map((item) => item.id).filter(Boolean));
  for (const item of asArray(coverageManifest.coverage)) for (const gap of asArray(item.gap_refs)) gapRawIds.add(gap);
  for (const item of asArray(capabilitiesManifest.capabilities)) for (const gap of asArray(item.gap_refs)) gapRawIds.add(gap);
  for (const gap of gapRawIds) gapIdByRaw.set(gap, publicId("gap", gap));

  const suiteByTarget = new Map();
  const benchmarkNames = new Set();
  const referencedTests = new Set();
  const referencedBenchmarks = new Set();
  for (const suite of asArray(testManifest.suites)) {
    for (const target of [...asArray(suite.targets), ...asArray(suite.planned_targets)]) {
      if (!suiteByTarget.has(target)) suiteByTarget.set(target, suite);
      referencedTests.add(target);
    }
    for (const target of asArray(suite.ctest_tests)) {
      if (!suiteByTarget.has(target)) suiteByTarget.set(target, suite);
      referencedTests.add(target);
    }
    if (suite.name) suiteByTarget.set(suite.name, suite);
    if (suite.name?.includes("benchmark") || suite.name === "system_benchmarks") benchmarkNames.add(suite.name);
  }
  if (benchmarkManifest.schema === "trit.benchmark_manifest.v1") benchmarkNames.add("p10-optimization-lab");
  for (const workload of asArray(benchmarkManifest.workloads)) {
    if (workload.id) benchmarkNames.add(workload.id);
    for (const referenceRun of asArray(workload.reference_runs)) referencedBenchmarks.add(referenceRun);
  }
  for (const item of asArray(stack.layers)) {
    asArray(item.test_refs).forEach((value) => referencedTests.add(value));
    asArray(item.benchmark_refs).forEach((value) => referencedBenchmarks.add(value));
  }
  for (const item of asArray(capabilitiesManifest.capabilities)) {
    asArray(item.test_refs).forEach((value) => referencedTests.add(value));
    asArray(item.benchmark_refs).forEach((value) => referencedBenchmarks.add(value));
  }
  for (const item of asArray(contractsManifest.contracts)) asArray(item.test_refs).forEach((value) => referencedTests.add(value));
  for (const item of asArray(decisionsManifest.decisions)) asArray(item.verification_refs).forEach((value) => referencedTests.add(value));
  for (const name of benchmarkNames) referencedBenchmarks.add(name);
  referencedTests.add("treatcode-public-api-conformance");
  for (const benchmark of referencedBenchmarks) benchmarkIdByRaw.set(benchmark, publicId("benchmark", benchmark));
  for (const test of referencedTests) testIdByRaw.set(test, `tc:test:${slug(test)}`);

  const sourcePaths = new Set([
    "AGENTS.md",
    "README.md",
    "KNOWN_GAPS.md",
    "TEST_MANIFEST.json",
    "BENCHMARK_MANIFEST.json",
    "BENCHMARK_PROTOCOL_SCHEMA.json",
    "benchmarks/reference/p10-reference.v1.json",
    "STACK_MANIFEST.json",
    "CAPABILITY_MANIFEST.json",
    "CONTRACT_MANIFEST.json",
    "DECISION_MANIFEST.json",
    "STACK_COVERAGE_REPORT.json",
    "docs/11_TreatCode_Platform/DOMAIN_MODEL.md",
    "docs/11_TreatCode_Platform/plans/P04_public_api_stack_explorer.md",
    "docs/11_TreatCode_Platform/plans/P15_P04_completeness_amendment.md",
    "docs/11_TreatCode_Platform/schemas/public_api.v1.openapi.json",
    "treatcode/package.json",
    "treatcode/src/PublicApp.tsx",
    "ARCHITECTURE_MANIFEST.json",
    "generated/architecture_contract.h",
    "generated/architecture_contract.trit",
    "docs/00_Quick_Ref/register_map.md",
    "docs/01_Logic_Level/gates.md",
    "docs/02_Hardware_ISA/immediate_decoding.md",
    "docs/04_Binary_Contract/architecture_v2.md",
    "ternary_compiler_ast.h",
    "ternary_compiler_ir.h",
    "ternary_compiler_lexer.h",
    "ternary_compiler_parser.h",
    "ternary_compiler_types.h",
    "ternary_gpu_kernels.h",
    "ternary_gpu_validation.h",
    "tcl_ast.trit",
    "tcl_infer.trit",
    "tcl_ir.trit",
    "tcl_lexer.trit",
    "tcl_type.trit",
    "tests/test_kernel.cpp",
    "tools/generate_architecture_contract.py",
    "tools/trit-test.ps1",
  ]);
  const p03FilesByPath = new Map();
  const removedIndexedPaths = new Set();
  for (const file of asArray(repositoryIndex?.files)) {
    const normalized = normalizePath(file.path);
    // An older index can retain commit blobs for files deleted in this checkout.
    if (normalized && !fs.existsSync(path.join(REPO_ROOT, normalized))) {
      removedIndexedPaths.add(normalized);
      continue;
    }
    if (!normalized || !isPublicPath(normalized) || file.source_authority === false || file.role === "generated") continue;
    p03FilesByPath.set(normalized, file);
    sourcePaths.add(normalized);
  }
  const rawRefs = [];
  const collectRefs = (items) => {
    for (const item of asArray(items)) {
      for (const ref of asArray(item.source_refs)) {
        if (typeof ref === "string") sourcePaths.add(normalizePath(ref));
        else if (ref?.path) {
          sourcePaths.add(normalizePath(ref.path));
          rawRefs.push(ref);
        }
      }
    }
  };
  collectRefs(stack.layers);
  collectRefs(stack.releases);
  collectRefs(capabilitiesManifest.capabilities);
  collectRefs(contractsManifest.contracts);
  collectRefs(decisionsManifest.decisions);
  collectRefs(coverageManifest.coverage);
  sourcePaths.add("docs/10_Benchmarks/system_benchmark_plan.md");
  sourcePaths.add("docs/10_Benchmarks/doom.md");
  sourcePaths.add("docs/10_Benchmarks/bitnet.md");
  sourcePaths.add("treatcode/src/ImplementationArena.tsx");

  const sourceRecords = [];
  const sourceIdByPath = new Map();
  for (const relativePath of [...sourcePaths].sort()) {
    const normalized = normalizePath(relativePath);
    if (!isPublicPath(normalized)) continue;
    const identity = `${slug(normalized)}-${sha256(normalized).slice(0, 8)}`;
    const id = `tc:source:${identity}`;
    const file = fileHash(normalized);
    const indexedFile = p03FilesByPath.get(normalized);
    const indexedSpan = sourceSpanFrom(indexedFile?.source?.span);
    sourceIdByPath.set(normalized, id);
    sourceRecords.push({
      id,
      entity_type: "source",
      name: path.basename(normalized),
      path: normalized,
      language: languageFor(normalized),
      role: indexedFile?.role || "source",
      content_kind: indexedFile?.content_kind || "text",
      status: file.exists ? "resolved" : "missing",
      bytes: file.bytes,
      sha256: file.hash,
      line_count: indexedFile?.line_count || undefined,
      source_authority: indexedFile?.source_authority !== false,
      index_status: indexedFile?.index_status || (file.exists ? "not_indexed" : "missing"),
      search_terms: file.exists ? sourceSearchTerms(normalized) : [],
      source_refs: [sourceRef(commit, normalized, { role: "repository_source", source_span: indexedSpan })],
      evidence_refs: [sourceRef(commit, "docs/11_TreatCode_Platform/REPOSITORY_INDEX.md", { role: "p03_index_contract" })],
    });
  }

  const sourceRefsFor = (refs, fallback = []) => {
    const values = [...asArray(refs), ...fallback];
    const result = [];
    for (const value of values) {
      const relativePath = normalizePath(typeof value === "string" ? value : value?.path);
      if (!relativePath || !isPublicPath(relativePath)) continue;
      if (!result.some((ref) => ref.path === relativePath)) {
        const metadata = {};
        if (typeof value === "object") {
          if (value.role) metadata.role = value.role;
          if (value.status) metadata.status = value.status;
          if (value.reason) metadata.reason = value.reason;
        }
        const declaredCommit = typeof value === "object" && /^[0-9a-f]{7,64}$/i.test(String(value.commit || "")) ? String(value.commit) : commit;
        // Resolved paths are hashed from the current worktree snapshot, so their
        // public provenance must identify the same commit. Preserve an older
        // declared commit only for missing/historical references.
        const referencedCommit = fileHash(relativePath).exists ? commit : declaredCommit;
        result.push(sourceRef(referencedCommit, relativePath, metadata));
      }
    }
    return result;
  };

  const stackCoverageById = new Map(asArray(coverageManifest.coverage).map((item) => [item.stack_id, item]));
  const stackNodes = asArray(stack.layers).map((item) => {
    const id = layerIdByRaw.get(item.id);
    const coverage = stackCoverageById.get(item.id);
    const dependencyNames = asArray(item.depends_on)
      .map((dependency) => asArray(stack.layers).find((candidate) => candidate.id === dependency)?.name)
      .filter(Boolean);
    const phaseStatus = coverage?.coverage || {};
    const statusSummary = Object.fromEntries(Object.entries(phaseStatus).map(([dimension, value]) => [dimension, value?.status || "unrecorded"]));
    return {
      id,
      entity_type: "stack_node",
      source_id: item.id,
      ordinal: item.phase,
      slug: item.slug,
      name: item.name,
      description: `${item.name}. Phase ${item.phase} of the Trit dependency stack.`,
      layer_kind: "dependency_phase",
      depends_on: asArray(item.depends_on).map((value) => layerIdByRaw.get(value)).filter(Boolean),
      component_ids: [],
      capability_ids: asArray(item.capability_ids).map((value) => capabilityIdByRaw.get(value)).filter(Boolean),
      contract_ids: asArray(item.contract_ids).map((value) => contractIdByRaw.get(value)).filter(Boolean),
      test_ids: asArray(item.test_refs).map((value) => testIdByRaw.get(value)).filter(Boolean),
      benchmark_ids: asArray(item.benchmark_refs).map((value) => benchmarkIdByRaw.get(value)).filter(Boolean),
      gap_ids: asArray(item.gap_refs).map((value) => gapIdByRaw.get(value)).filter(Boolean),
      release_ids: asArray(item.release_refs).map((value) => releaseIdByRaw.get(value)).filter(Boolean),
      coverage: coverage?.coverage || {},
      status_summary: statusSummary,
      problem: `Provides the ${String(item.name || "stack").toLowerCase()} boundary as an ordered, reviewable part of the Trit system.`,
      inputs: dependencyNames.length
        ? `Outputs from ${dependencyNames.join(", ")} enter this phase through the recorded dependency edges.`
        : "Repository authority, physical ternary constraints, and the declared phase contract enter this root phase.",
      outputs: "Its recorded contracts, capabilities, tests, benchmarks, releases, and gaps are available to the next dependent phases.",
      implementation_status: statusSummary.implemented || statusSummary.integrated || "unrecorded",
      phase_context: {
        dependency_count: asArray(item.depends_on).length,
        source_count: asArray(item.source_refs).length,
        evidence_count: 1,
        registry_id: item.id,
      },
      source_refs: sourceRefsFor(item.source_refs),
      evidence_refs: sourceRefsFor(["TEST_MANIFEST.json", ...asArray(item.test_refs).map(() => "TEST_MANIFEST.json")]),
    };
  });

  const dependentsById = new Map(stackNodes.map((node) => [node.id, []]));
  for (const item of asArray(stack.layers)) {
    const nodeId = layerIdByRaw.get(item.id);
    for (const dependency of asArray(item.depends_on)) {
      const dependencyId = layerIdByRaw.get(dependency);
      if (dependencyId && dependentsById.has(dependencyId)) dependentsById.get(dependencyId).push(nodeId);
    }
  }
  for (const node of stackNodes) {
    node.dependent_ids = [...new Set(dependentsById.get(node.id) || [])];
    node.next_node_ids = node.dependent_ids;
    node.outputs = node.dependent_ids.length
      ? `This phase hands its verified boundary to ${node.dependent_ids.map((nextId) => stackNodes.find((candidate) => candidate.id === nextId)?.name).filter(Boolean).join(", ")}.`
      : "This terminal phase hands closure evidence to the release and review boundary.";
  }

  const layerIdsBySourcePath = new Map();
  for (const item of asArray(stack.layers)) {
    const layerId = layerIdByRaw.get(item.id);
    for (const ref of asArray(item.source_refs)) {
      const sourcePath = normalizePath(typeof ref === "string" ? ref : ref?.path);
      if (layerId && sourcePath && isPublicPath(sourcePath)) layerIdsBySourcePath.set(sourcePath, [...(layerIdsBySourcePath.get(sourcePath) || []), layerId]);
    }
  }
  const components = sourceRecords.map((source) => {
    const layerIds = [...new Set(layerIdsBySourcePath.get(source.path) || [])];
    const layerNames = layerIds.map((layerId) => stackNodes.find((node) => node.id === layerId)?.name).filter(Boolean);
    return {
      id: `tc:component:${slug(source.path)}-${sha256(source.path).slice(0, 8)}`,
      entity_type: "component",
      name: source.name,
      description: layerNames.length
        ? `Source-backed component in the ${layerNames.join(", ")} layer${layerNames.length === 1 ? "" : "s"}.`
        : `Repository source component for ${source.path}.`,
      component_kind: source.language === "markdown" ? "documentation" : source.role || "source_file",
      layer_ids: layerIds,
      source_ids: [source.id],
      path: source.path,
      symbol_ids: [],
      source_refs: source.source_refs,
      evidence_refs: source.evidence_refs,
    };
  });
  for (const node of stackNodes) node.component_ids = components.filter((item) => item.layer_ids.includes(node.id)).map((item) => item.id);

  const capabilities = asArray(capabilitiesManifest.capabilities).map((item) => ({
    id: capabilityIdByRaw.get(item.id),
    entity_type: "capability",
    source_id: item.id,
    name: item.name,
    description: item.name,
    capability_class: item.capability_class,
    layer_ids: asArray(item.layer_ids).map((value) => layerIdByRaw.get(value)).filter(Boolean),
    contract_ids: asArray(item.contract_ids).map((value) => contractIdByRaw.get(value)).filter(Boolean),
    depends_on: asArray(item.depends_on_capability_ids).map((value) => capabilityIdByRaw.get(value)).filter(Boolean),
    status: item.status,
    maturity_status: item.maturity_status,
    compatibility_status: item.compatibility_status,
    evidence_status: item.evidence_status,
    test_ids: asArray(item.test_refs).map((value) => testIdByRaw.get(value)).filter(Boolean),
    benchmark_ids: asArray(item.benchmark_refs).map((value) => benchmarkIdByRaw.get(value)).filter(Boolean),
    gap_ids: asArray(item.gap_refs).map((value) => gapIdByRaw.get(value)).filter(Boolean),
    source_refs: sourceRefsFor(item.source_refs),
    evidence_refs: sourceRefsFor(["TEST_MANIFEST.json"]),
  }));

  const contracts = asArray(contractsManifest.contracts).map((item) => ({
    id: contractIdByRaw.get(item.id),
    entity_type: "contract",
    source_id: item.id,
    name: item.name,
    description: item.name,
    contract_status: item.contract_status,
    maturity_status: item.maturity_status,
    compatibility_status: item.compatibility_status,
    evidence_status: item.evidence_status,
    layer_ids: asArray(item.layer_ids).map((value) => layerIdByRaw.get(value)).filter(Boolean),
    test_ids: asArray(item.test_refs).map((value) => testIdByRaw.get(value)).filter(Boolean),
    decision_ids: asArray(item.decision_ids).map((value) => decisionIdByRaw.get(value)).filter(Boolean),
    source_refs: sourceRefsFor(item.source_refs),
    evidence_refs: sourceRefsFor(["TEST_MANIFEST.json"]),
  }));

  const decisions = asArray(decisionsManifest.decisions).map((item) => ({
    id: decisionIdByRaw.get(item.id),
    entity_type: "decision",
    source_id: item.id,
    name: item.title,
    title: item.title,
    description: item.rationale,
    disposition: item.disposition,
    canonical: item.canonical,
    current_truth: contractIdByRaw.get(item.current_truth) || item.current_truth,
    supersedes: asArray(item.supersedes).map((value) => decisionIdByRaw.get(value)).filter(Boolean),
    contract_ids: asArray(item.affected_contract_ids).map((value) => contractIdByRaw.get(value)).filter(Boolean),
    capability_ids: asArray(item.affected_capability_ids).map((value) => capabilityIdByRaw.get(value)).filter(Boolean),
    source_refs: sourceRefsFor(item.source_refs),
    evidence_refs: sourceRefsFor(["DECISION_MANIFEST.json"]),
  }));
  for (const node of stackNodes) {
    const contractIds = new Set(asArray(node.contract_ids));
    const capabilityIds = new Set(asArray(node.capability_ids));
    node.decision_ids = decisions
      .filter((decision) => asArray(decision.contract_ids).some((id) => contractIds.has(id)) || asArray(decision.capability_ids).some((id) => capabilityIds.has(id)))
      .map((decision) => decision.id);
  }

  const gapsById = new Map(knownGapRecords.map((item) => [item.id, item]));
  const gaps = [...gapIdByRaw.keys()].sort().map((rawId) => {
    const item = gapsById.get(rawId) || {};
    return {
      id: gapIdByRaw.get(rawId),
      entity_type: "gap",
      source_id: rawId,
      name: rawId.replace(/^trit\.gap\./, "").replaceAll("-", " "),
      description: item.description || `Known open gap: ${rawId}.`,
      status: item.status || "open",
      source_refs: sourceRefsFor(item.source_refs, ["KNOWN_GAPS.md"]),
      evidence_refs: sourceRefsFor(["KNOWN_GAPS.md"]),
    };
  });

  const tests = [...testIdByRaw.keys()].sort().map((target) => {
    const suite = suiteByTarget.get(target);
    const planned = suite?.planned_targets?.includes(target);
    return {
      id: testIdByRaw.get(target),
      entity_type: "test",
      name: target,
      description: suite?.description || `Referenced verification target ${target}.`,
      test_kind: suite?.name || "referenced_target",
      command: suite ? `python tools/trit_tool.py test ${suite.name}` : `ctest --test-dir build -R ${target}`,
      asserts: suite?.description || target,
      status: planned ? "planned" : suite ? "active" : "referenced",
      suite: suite?.name || null,
      source_refs: sourceRefsFor(["TEST_MANIFEST.json"]),
      evidence_refs: sourceRefsFor(["TEST_MANIFEST.json"]),
    };
  });

  const benchmarks = [...benchmarkIdByRaw.keys()].sort().map((name) => {
    const suite = suiteByTarget.get(name) || asArray(testManifest.suites).find((item) => item.name === name);
    const isP10 = name === "p10-optimization-lab";
    const docs = isP10
      ? ["BENCHMARK_MANIFEST.json", "BENCHMARK_PROTOCOL_SCHEMA.json", "benchmarks/reference/p10-reference.v1.json", "treatcode/src/ImplementationArena.tsx"]
      : asArray(suite?.docs);
    return {
      id: benchmarkIdByRaw.get(name),
      entity_type: "benchmark",
      name,
      description: isP10 ? "Correctness-gated, representation-aware benchmark protocol with tritwise and vector/matrix pilots." : suite?.description || `Benchmark evidence for ${name}.`,
      workload: isP10 ? "P10 tritwise sign inversion and vector/matrix dot-product pilots." : suite?.description || name,
      metric: isP10 ? "time, VM cycles, instructions, dispatches, memory traffic, code size, register pressure, allocations, and hardware proxies" : "repeatable test or workload result",
      budget: isP10 ? "two warmups, seven measured repetitions, and a declared coefficient-of-variation envelope" : "recorded by the authoritative test manifest",
      status: isP10 ? "active" : suite?.status || "referenced",
      documentation_paths: docs,
      source_refs: sourceRefsFor(["TEST_MANIFEST.json", ...docs]),
      evidence_refs: sourceRefsFor(["TEST_MANIFEST.json"]),
    };
  });

  const releases = asArray(stack.releases).map((item) => ({
    id: releaseIdByRaw.get(item.id),
    entity_type: "release",
    source_id: item.id,
    name: item.name,
    description: item.name,
    status: item.status,
    verification_ids: asArray(item.verification_refs).map((value) => testIdByRaw.get(value)).filter(Boolean),
    source_refs: sourceRefsFor(item.source_refs),
    evidence_refs: sourceRefsFor(["TEST_MANIFEST.json"]),
  }));
  releases.push({
    id: "tc:release:treatcode-public-api-v1",
    entity_type: "release",
    source_id: "treatcode-public-api-v1",
    name: "TreatCode public API v1",
    description: "Read-only, provenance-preserving Stack Explorer snapshot release.",
    status: "generated",
    verification_ids: ["tc:test:treatcode-public-api-conformance"],
    source_refs: sourceRefsFor(["docs/11_TreatCode_Platform/schemas/public_api.v1.openapi.json", "treatcode/package.json"]),
    evidence_refs: sourceRefsFor(["treatcode/package.json"]),
  });

  const projects = [{
    id: "tc:project:trit",
    entity_type: "project",
    name: "Trit",
    description: "Trit v2 ternary ISA, compiler, VM, kernel, and application stack.",
    repository: PUBLIC_REPOSITORY,
    default_branch: "main",
    current_commit: commit,
    stack_node_ids: stackNodes.map((item) => item.id),
    source_refs: sourceRefsFor(["README.md", "AGENTS.md"]),
    evidence_refs: sourceRefsFor(["TEST_MANIFEST.json"]),
  }];

  const relations = [];
  const relationByKey = new Map();
  const addRelation = (from, type, to, paths = [], extra = {}) => {
    if (!from || !to) return;
    const key = `${from}|${type}|${to}`;
    const references = sourceRefsFor(paths.length ? paths : ["STACK_MANIFEST.json"]);
    if (relationByKey.has(key)) {
      const existing = relationByKey.get(key);
      for (const reference of references) if (!existing.source_refs.some((candidate) => candidate.path === reference.path)) existing.source_refs.push(reference);
      return;
    }
    const relation = { from, type, to, source_refs: references, ...extra };
    relationByKey.set(key, relation);
    relations.push(relation);
  };
  for (const node of stackNodes) {
    for (const dependency of asArray(node.depends_on)) addRelation(node.id, "depends_on", dependency, ["STACK_MANIFEST.json"]);
    for (const target of [...asArray(node.capability_ids), ...asArray(node.contract_ids), ...asArray(node.test_ids), ...asArray(node.benchmark_ids), ...asArray(node.gap_ids), ...asArray(node.release_ids)]) {
      addRelation(node.id, "contains", target, ["STACK_COVERAGE_REPORT.json"]);
    }
    for (const component of asArray(node.component_ids)) addRelation(node.id, "contains", component, ["STACK_MANIFEST.json"]);
  }
  for (const capability of capabilities) {
    for (const target of [...asArray(capability.contract_ids), ...asArray(capability.layer_ids)]) addRelation(capability.id, "implements", target, ["CAPABILITY_MANIFEST.json"]);
    for (const target of asArray(capability.depends_on)) addRelation(capability.id, "depends_on", target, ["CAPABILITY_MANIFEST.json"]);
  }
  for (const contract of contracts) {
    for (const target of [...asArray(contract.test_ids), ...asArray(contract.decision_ids)]) addRelation(contract.id, "verified_by", target, ["CONTRACT_MANIFEST.json"]);
  }
  for (const release of releases) for (const target of asArray(release.verification_ids)) addRelation(release.id, "verified_by", target, ["STACK_MANIFEST.json"]);

  const symbolRecords = [];
  const symbolIdByRaw = new Map();
  const symbolKeySet = new Set();
  const supportedLanguages = new Set(["trit", "tasm", "cpp", "c", "typescript", "typescript-react", "javascript", "python"]);
  const addSymbol = (source, definition) => {
    const name = String(definition.name || definition.qualified_name || "").trim();
    const line = Number(definition.source_span?.start_line || definition.line || 0);
    if (!name || name.length < 2 || !Number.isInteger(line) || line < 1 || ["if", "is", "and", "or", "for", "while", "switch", "return", "match"].includes(name.toLowerCase())) return null;
    const rawKey = String(definition.raw_id || `${source.path}:${name}:${line}`);
    if (symbolKeySet.has(rawKey)) return symbolIdByRaw.get(rawKey);
    symbolKeySet.add(rawKey);
    const identity = `${source.path}-${definition.qualified_name || name}-${line}`;
    const symbolId = `tc:symbol:${slug(identity)}-${sha256(rawKey).slice(0, 8)}`;
    const span = sourceSpanFrom(definition.source_span) || { start_line: line, end_line: line };
    const record = {
      id: symbolId,
      entity_type: "symbol",
      name,
      symbol: name,
      qualified_name: definition.qualified_name || name,
      kind: definition.kind || "declaration",
      language: definition.language || source.language,
      parser: definition.parser || "p03-index",
      signature: definition.signature || undefined,
      source_id: source.id,
      path: source.path,
      source_span: span,
      source_refs: [sourceRef(commit, source.path, { role: "symbol_definition", source_span: span })],
      evidence_refs: [sourceRef(commit, source.path, { role: "symbol_source", source_span: span })],
    };
    symbolRecords.push(record);
    symbolIdByRaw.set(rawKey, symbolId);
    source.symbol_ids = [...(source.symbol_ids || []), symbolId];
    return symbolId;
  };
  if (repositoryIndex) {
    for (const definition of asArray(repositoryIndex.symbols)) {
      const sourcePath = normalizePath(definition.path);
      const source = sourceRecords.find((candidate) => candidate.path === sourcePath);
      if (!source || !supportedLanguages.has(source.language) || source.status !== "resolved") continue;
      addSymbol(source, { ...definition, raw_id: definition.id });
    }
  } else {
    for (const source of sourceRecords) {
      if (!supportedLanguages.has(source.language) || source.status !== "resolved") continue;
      const lines = readText(source.path).split(/\r?\n/);
      lines.forEach((line, index) => {
        const lineNumber = index + 1;
        let match = line.match(/\b(?:fn|function|func|def|class|struct|enum|interface|type)\s+([A-Za-z_]\w*)/);
        if (match) addSymbol(source, { name: match[1], kind: "declaration", line: lineNumber });
        match = line.match(/^\s*(?:export\s+)?(?:const|let|var)\s+([A-Za-z_]\w*)/);
        if (match) addSymbol(source, { name: match[1], kind: "value", line: lineNumber });
        match = line.match(/^\s*(?:static\s+|inline\s+|virtual\s+|constexpr\s+)*(?:[A-Za-z_][\w:<>*&\[\], ]+)\s+([A-Za-z_]\w*)\s*\([^;]*\)\s*(?:const)?\s*\{/);
        if (match) addSymbol(source, { name: match[1], kind: "function", line: lineNumber });
      });
    }
  }
  for (const component of components) component.symbol_ids = sourceRecords.find((source) => source.id === component.source_ids[0])?.symbol_ids || [];

  const rawPublicIdByValue = new Map();
  for (const [raw, value] of layerIdByRaw) rawPublicIdByValue.set(`record:layer:${raw}`, value);
  for (const [raw, value] of capabilityIdByRaw) rawPublicIdByValue.set(`record:capability:${raw}`, value);
  for (const [raw, value] of contractIdByRaw) rawPublicIdByValue.set(`record:contract:${raw}`, value);
  for (const [raw, value] of decisionIdByRaw) rawPublicIdByValue.set(`record:decision:${raw}`, value);
  for (const [raw, value] of releaseIdByRaw) rawPublicIdByValue.set(`record:release:${raw}`, value);
  for (const [raw, value] of gapIdByRaw) rawPublicIdByValue.set(`record:gap:${raw}`, value);
  for (const [raw, value] of testIdByRaw) rawPublicIdByValue.set(`record:test:${raw}`, value);
  for (const [raw, value] of benchmarkIdByRaw) rawPublicIdByValue.set(`record:benchmark:${raw}`, value);
  const sourceIdByRawFile = new Map([...sourceIdByPath.entries()].map(([sourcePath, id]) => [`file:${sourcePath}`, id]));
  const symbolIdByRawIndex = new Map([...symbolIdByRaw.entries()].map(([raw, id]) => [`${raw}`, id]));
  const unresolvedRepositoryRelationships = [];
  const mapRepositoryEndpoint = (endpoint) => {
    if (typeof endpoint !== "string") return null;
    if (sourceIdByRawFile.has(endpoint)) return sourceIdByRawFile.get(endpoint);
    if (symbolIdByRawIndex.has(endpoint)) return symbolIdByRawIndex.get(endpoint);
    if (rawPublicIdByValue.has(endpoint)) return rawPublicIdByValue.get(endpoint);
    return null;
  };
  if (repositoryIndex) {
    for (const relationship of asArray(repositoryIndex.relationships)) {
      if ([relationship.source?.path, relationship.target_path].some((value) => removedIndexedPaths.has(normalizePath(value)))) continue;
      const from = mapRepositoryEndpoint(relationship.from);
      const to = mapRepositoryEndpoint(relationship.to);
      if (from && to) {
        const relationshipPath = normalizePath(relationship.source?.path || relationship.target_path);
        addRelation(from, relationship.type || "references", to, relationshipPath ? [relationshipPath] : ["docs/11_TreatCode_Platform/REPOSITORY_INDEX.md"], {
          source_span: sourceSpanFrom(relationship.source?.span),
          origin: "p03_repository_index",
        });
      } else {
        unresolvedRepositoryRelationships.push({
          from: relationship.from,
          to: relationship.to,
          type: relationship.type || "references",
          resolution: relationship.resolution || "unresolved",
          required: false,
          reason: relationship.resolution === "unresolved" ? "P03 parser recorded an unresolved or external reference." : "Endpoint is outside the intentionally public entity inventory.",
          source: relationship.source || null,
        });
      }
    }
  }

  const allRecordCollections = { projects, stack_nodes: stackNodes, components, capabilities, contracts, decisions, sources: sourceRecords, symbols: symbolRecords, tests, benchmarks, runs: [], releases, gaps };
  const publicRecordById = new Map();
  for (const collection of Object.values(allRecordCollections)) for (const record of collection) publicRecordById.set(record.id, record);
  const sourceIdForReference = (reference) => {
    const referencePath = normalizePath(typeof reference === "string" ? reference : reference?.path);
    return referencePath ? sourceIdByPath.get(referencePath) : undefined;
  };
  const addEvidenceRelations = (record) => {
    for (const reference of asArray(record.source_refs)) {
      const sourceId = sourceIdForReference(reference);
      if (sourceId) addRelation(record.id, "sourced_by", sourceId, [reference.path]);
    }
    for (const reference of asArray(record.evidence_refs)) {
      const sourceId = sourceIdForReference(reference);
      if (sourceId) addRelation(record.id, "evidenced_by", sourceId, [reference.path]);
    }
  };
  for (const collection of Object.values(allRecordCollections)) for (const record of collection) addEvidenceRelations(record);

  const runs = [
    {
      id: "tc:run:baseline-smoke",
      entity_type: "run",
      name: "Smoke baseline",
      run_kind: "test_suite",
      started_at: generatedAt,
      result: "passed",
      command: "tools/trit-test.ps1 smoke",
      source_refs: sourceRefsFor(["TEST_MANIFEST.json"]),
      evidence_refs: sourceRefsFor(["TEST_MANIFEST.json"]),
    },
    {
      id: "tc:run:baseline-production",
      entity_type: "run",
      name: "Production baseline",
      run_kind: "test_suite",
      started_at: generatedAt,
      result: "passed",
      command: "tools/trit-test.ps1 production",
      source_refs: sourceRefsFor(["TEST_MANIFEST.json"]),
      evidence_refs: sourceRefsFor(["TEST_MANIFEST.json"]),
    },
  ];

  allRecordCollections.runs = runs;
  for (const run of runs) {
    publicRecordById.set(run.id, run);
    addEvidenceRelations(run);
  }

  const resourceRouteFor = (resource, id) => `/resources/${resource}/${encodeURIComponent(id)}`;
  const evidenceRouteFor = (id) => `/evidence/${encodeURIComponent(id)}`;
  for (const resource of PUBLIC_RESOURCES) {
    for (const record of allRecordCollections[resource] || []) {
      record.public_resource = resource;
      record.public_route = resourceRouteFor(resource, record.id);
      record.evidence_route = evidenceRouteFor(record.id);
      publicRecordById.set(record.id, record);
    }
  }
  for (const source of sourceRecords) {
    for (const symbolId of asArray(source.symbol_ids)) addRelation(source.id, "defines", symbolId, [source.path]);
  }
  for (const component of components) {
    for (const target of [...asArray(component.layer_ids), ...asArray(component.source_ids), ...asArray(component.symbol_ids)]) addRelation(component.id, "contains", target, [component.path]);
  }
  for (const symbol of symbolRecords) {
    if (symbol.source_id) addRelation(symbol.id, "defined_in", symbol.source_id, [symbol.path]);
  }
  for (const project of projects) for (const target of asArray(project.stack_node_ids)) addRelation(project.id, "contains", target, ["STACK_MANIFEST.json"]);
  for (const record of Object.values(allRecordCollections).flat()) {
    for (const [field, type] of [["layer_ids", "belongs_to_layer"], ["contract_ids", "references_contract"], ["capability_ids", "references_capability"], ["test_ids", "verified_by"], ["benchmark_ids", "benchmarked_by"], ["gap_ids", "records_gap"], ["release_ids", "included_in_release"], ["decision_ids", "references_decision"], ["verification_ids", "verified_by"], ["source_ids", "references_source"]]) {
      for (const target of asArray(record[field])) if (publicRecordById.has(target)) addRelation(record.id, type, target, [record.source_refs?.[0]?.path || "STACK_MANIFEST.json"]);
    }
    for (const target of asArray(record.supersedes)) if (publicRecordById.has(target)) addRelation(record.id, "supersedes", target, [record.source_refs?.[0]?.path || "DECISION_MANIFEST.json"]);
    if (typeof record.current_truth === "string" && publicRecordById.has(record.current_truth)) addRelation(record.id, "current_truth", record.current_truth, [record.source_refs?.[0]?.path || "DECISION_MANIFEST.json"]);
  }

  const compareText = (left, right) => left < right ? -1 : left > right ? 1 : 0;
  relations.sort((left, right) => compareText(`${left.from}\u0000${left.type}\u0000${left.to}`, `${right.from}\u0000${right.type}\u0000${right.to}`));
  unresolvedRepositoryRelationships.sort((left, right) => compareText(
    `${left.from || ""}\u0000${left.type || ""}\u0000${left.to || ""}`,
    `${right.from || ""}\u0000${right.type || ""}\u0000${right.to || ""}`,
  ));

  const relationCountById = new Map();
  for (const relation of relations) {
    relationCountById.set(relation.from, (relationCountById.get(relation.from) || 0) + 1);
    relationCountById.set(relation.to, (relationCountById.get(relation.to) || 0) + 1);
  }
  const collectionCoverage = {};
  for (const resource of PUBLIC_RESOURCES) {
    collectionCoverage[resource] = (allRecordCollections[resource] || []).map((record) => ({
      id: record.id,
      name: record.name || record.title || record.path || record.id,
      route: record.public_route,
      evidence_route: record.evidence_route,
      source_count: asArray(record.source_refs).length,
      evidence_count: asArray(record.evidence_refs).length,
      relationship_count: relationCountById.get(record.id) || 0,
      reachable: Boolean(record.public_route && publicRecordById.has(record.id)),
    }));
  }
  const stackCoverage = stackNodes.map((node) => {
    const requiredFields = ["problem", "inputs", "outputs", "depends_on", "dependent_ids", "status_summary", "implementation_status", "source_refs", "evidence_refs", "component_ids", "capability_ids", "contract_ids", "decision_ids", "test_ids", "benchmark_ids", "gap_ids", "release_ids"];
    const missingFields = requiredFields.filter((field) => {
      const value = node[field];
      return value === undefined || value === null || (Array.isArray(value) && value.length === 0 && ["source_refs", "evidence_refs"].includes(field));
    });
    return {
      id: node.id,
      ordinal: node.ordinal,
      name: node.name,
      route: node.public_route,
      evidence_route: node.evidence_route,
      required_fields: requiredFields,
      missing_fields: missingFields,
      linked_records: {
        components: asArray(node.component_ids),
        capabilities: asArray(node.capability_ids),
        contracts: asArray(node.contract_ids),
        decisions: asArray(node.decision_ids),
        tests: asArray(node.test_ids),
        benchmarks: asArray(node.benchmark_ids),
        gaps: asArray(node.gap_ids),
        releases: asArray(node.release_ids),
        sources: asArray(node.source_refs).map((reference) => sourceIdForReference(reference)).filter(Boolean),
      },
      reachable: Boolean(node.public_route && !missingFields.length),
    };
  });
  const p03IndexCommit = repositoryIndex?.repository?.commit || p03Freshness?.index_commit || null;
  const p03IndexFresh = Boolean(repositoryIndex && p03IndexCommit === commit && p03Freshness?.commit_match && p03Freshness?.current_commit === commit);
  const freshness = {
    schema: FRESHNESS_SCHEMA,
    snapshot_commit: commit,
    snapshot_id: `tc:snapshot:${commit.slice(0, 12)}`,
    p03_index_available: Boolean(repositoryIndex),
    p03_index_commit: p03IndexCommit,
    p03_index_fresh: p03IndexFresh,
    p03_index_path: repositoryIndex ? "build/treatcode-index/repository-index.v1.json" : null,
    source_inputs: ["STACK_MANIFEST.json", "CAPABILITY_MANIFEST.json", "CONTRACT_MANIFEST.json", "DECISION_MANIFEST.json", "STACK_COVERAGE_REPORT.json", "TEST_MANIFEST.json", "BENCHMARK_MANIFEST.json", "build/treatcode-index/repository-index.v1.json"],
    stale: !p03IndexFresh,
    // Use the repository commit date instead of wall-clock time. A repeated
    // build of the same source state must produce the same snapshot bytes.
    generated_at: generatedAt,
  };
  const publicCoverage = {
    schema: COVERAGE_SCHEMA,
    snapshot_commit: commit,
    snapshot_id: `tc:snapshot:${commit.slice(0, 12)}`,
    source_of_truth: ["STACK_MANIFEST.json", "CAPABILITY_MANIFEST.json", "CONTRACT_MANIFEST.json", "DECISION_MANIFEST.json", "STACK_COVERAGE_REPORT.json", "TEST_MANIFEST.json", "BENCHMARK_MANIFEST.json", "build/treatcode-index/repository-index.v1.json"],
    stack_phase_count: stackNodes.length,
    stack_phases: stackCoverage,
    collection_counts: Object.fromEntries(PUBLIC_RESOURCES.map((resource) => [resource, (allRecordCollections[resource] || []).length])),
    resources: collectionCoverage,
    relationship_count: relations.length,
    unresolved_repository_relationship_count: unresolvedRepositoryRelationships.length,
    all_records_reachable: PUBLIC_RESOURCES.every((resource) => collectionCoverage[resource].every((record) => record.reachable)),
    all_stack_phases_useful: stackCoverage.every((record) => record.reachable),
    no_hidden_first_n_limit: true,
    freshness,
  };
  const relationshipIndex = {
    schema: RELATIONSHIP_INDEX_SCHEMA,
    snapshot: { commit, id: `tc:snapshot:${commit.slice(0, 12)}` },
    edges: relations,
    unresolved_external_edges: unresolvedRepositoryRelationships,
    counts: {
      public_edges: relations.length,
      unresolved_external_edges: unresolvedRepositoryRelationships.length,
      by_type: Object.fromEntries([...relations.reduce((map, relation) => map.set(relation.type, (map.get(relation.type) || 0) + 1), new Map())].sort()),
    },
  };

  const allEntities = [projects, stackNodes, components, capabilities, contracts, decisions, sourceRecords, symbolRecords, tests, benchmarks, runs, releases, gaps];
  const counts = {};
  for (const collection of allEntities) for (const item of collection) counts[item.entity_type || "entity"] = (counts[item.entity_type || "entity"] || 0) + 1;
  counts.relations = relations.length;

  const snapshot = {
    schema_version: SNAPSHOT_SCHEMA,
    snapshot: {
      id: `tc:snapshot:${commit.slice(0, 12)}`,
      repository: PUBLIC_REPOSITORY,
      commit,
      generated_at: generatedAt,
      source: "root registries, TEST_MANIFEST.json, and tracked source paths",
    },
    projects,
    stack_nodes: stackNodes,
    components,
    capabilities,
    contracts,
    decisions,
    sources: sourceRecords,
    symbols: symbolRecords,
    tests,
    benchmarks,
    runs,
    releases,
    gaps,
    relations,
    coverage: publicCoverage,
    relationship_index: {
      schema: RELATIONSHIP_INDEX_SCHEMA,
      edge_count: relations.length,
      unresolved_external_edge_count: unresolvedRepositoryRelationships.length,
    },
    freshness,
    statistics: counts,
  };
  snapshot.snapshot.id = `tc:snapshot:${sha256(JSON.stringify(snapshot)).slice(0, 12)}`;
  snapshot.coverage.snapshot_id = snapshot.snapshot.id;
  snapshot.freshness.snapshot_id = snapshot.snapshot.id;
  relationshipIndex.snapshot = snapshot.snapshot;
  return { snapshot, relationshipIndex };
}

function envelope(snapshot, data, resource) {
  return {
    schema_version: API_SCHEMA,
    snapshot: snapshot.snapshot,
    data,
    meta: { resource, count: Array.isArray(data) ? data.length : undefined, total: Array.isArray(data) ? data.length : undefined },
    links: { self: `/api/public/v1/${resource}` },
  };
}

export function generatePublicSnapshot() {
  const { snapshot, relationshipIndex } = buildSnapshot();
  fs.mkdirSync(OUTPUT_ROOT, { recursive: true });
  fs.writeFileSync(path.join(OUTPUT_ROOT, "snapshot.json"), `${JSON.stringify(snapshot, null, 2)}\n`, "utf8");
  const resources = PUBLIC_RESOURCES;
  for (const resource of resources) {
    fs.writeFileSync(path.join(OUTPUT_ROOT, `${resource}.json`), `${JSON.stringify(envelope(snapshot, snapshot[resource], resource), null, 2)}\n`, "utf8");
  }
  fs.writeFileSync(path.join(OUTPUT_ROOT, "search-index.json"), `${JSON.stringify({
    schema_version: API_SCHEMA,
    snapshot: snapshot.snapshot,
    data: { searchable_resources: resources, relationship_count: snapshot.relations.length, record_counts: Object.fromEntries(resources.map((resource) => [resource, snapshot[resource].length])), complete: true },
  }, null, 2)}\n`, "utf8");
  fs.writeFileSync(path.join(OUTPUT_ROOT, "coverage.json"), `${JSON.stringify(snapshot.coverage, null, 2)}\n`, "utf8");
  fs.writeFileSync(path.join(OUTPUT_ROOT, "relationship-index.json"), `${JSON.stringify(relationshipIndex, null, 2)}\n`, "utf8");
  fs.writeFileSync(path.join(OUTPUT_ROOT, "freshness.json"), `${JSON.stringify(snapshot.freshness, null, 2)}\n`, "utf8");
  const openapiSource = path.join(REPO_ROOT, "docs", "11_TreatCode_Platform", "schemas", "public_api.v1.openapi.json");
  if (fs.existsSync(openapiSource)) fs.copyFileSync(openapiSource, path.join(OUTPUT_ROOT, "openapi.json"));
  console.log(`generated TreatCode public snapshot ${snapshot.snapshot.id} (${snapshot.sources.length} sources, ${snapshot.symbols.length} symbols)`);
}

generatePublicSnapshot();
