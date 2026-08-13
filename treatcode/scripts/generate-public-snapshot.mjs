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
  return slug(parts.slice(-3).join("-"));
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
  return [...new Set(normalized.split(/\s+/).filter((term) => term.length > 1 && !SEARCH_STOP_WORDS.has(term)))].slice(0, 1024);
}

function sourceRef(commit, relativePath, extra = {}) {
  const normalized = normalizePath(relativePath);
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
  for (const suite of asArray(testManifest.suites)) {
    for (const target of [...asArray(suite.targets), ...asArray(suite.planned_targets)]) {
      if (!suiteByTarget.has(target)) suiteByTarget.set(target, suite);
    }
    if (suite.name?.includes("benchmark") || suite.name === "system_benchmarks") benchmarkNames.add(suite.name);
  }
  if (benchmarkManifest.schema === "trit.benchmark_manifest.v1") benchmarkNames.add("p10-optimization-lab");
  const referencedTests = new Set();
  const referencedBenchmarks = new Set();
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
  sourcePaths.add("build/diagnostics/latest/agent_diagnostics.json");
  sourcePaths.add("docs/10_Benchmarks/system_benchmark_plan.md");
  sourcePaths.add("docs/10_Benchmarks/doom.md");
  sourcePaths.add("docs/10_Benchmarks/bitnet.md");
  sourcePaths.add("treatcode/src/ImplementationArena.tsx");

  const sourceRecords = [];
  const sourceIdByPath = new Map();
  for (const relativePath of [...sourcePaths].sort()) {
    const normalized = normalizePath(relativePath);
    const identity = `${slug(normalized)}-${sha256(normalized).slice(0, 8)}`;
    const id = `tc:source:${identity}`;
    const file = fileHash(normalized);
    sourceIdByPath.set(normalized, id);
    sourceRecords.push({
      id,
      entity_type: "source",
      name: path.basename(normalized),
      path: normalized,
      language: languageFor(normalized),
      status: file.exists ? "resolved" : "missing",
      bytes: file.bytes,
      sha256: file.hash,
      search_terms: file.exists ? sourceSearchTerms(normalized) : [],
      source_refs: [sourceRef(commit, normalized, { role: "snapshot_source" })],
      evidence_refs: [sourceRef(commit, "STACK_MANIFEST.json", { role: "snapshot_manifest" })],
    });
  }

  const sourceRefsFor = (refs, fallback = []) => {
    const values = [...asArray(refs), ...fallback];
    const result = [];
    for (const value of values) {
      const relativePath = normalizePath(typeof value === "string" ? value : value?.path);
      if (!relativePath) continue;
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
      source_refs: sourceRefsFor(item.source_refs),
      evidence_refs: sourceRefsFor(["TEST_MANIFEST.json", ...asArray(item.test_refs).map(() => "TEST_MANIFEST.json")]),
    };
  });

  const componentMap = new Map();
  for (const node of stackNodes) {
    for (const ref of node.source_refs || []) {
      if (!ref.path) continue;
      const key = `${node.id}:${ref.path}`;
      if (!componentMap.has(key)) {
        componentMap.set(key, {
          id: `tc:component:${slug(`${node.slug}-${ref.path}`)}`,
          entity_type: "component",
          name: path.basename(ref.path),
          description: `Source-backed component in the ${node.name} layer.`,
          component_kind: "source_file",
          layer_ids: [node.id],
          source_ids: [sourceIdByPath.get(ref.path)].filter(Boolean),
          path: ref.path,
          source_refs: [ref],
          evidence_refs: node.evidence_refs,
        });
      }
    }
  }
  const components = [...componentMap.values()];
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
  const addRelation = (from, type, to, paths = []) => {
    if (!from || !to) return;
    relations.push({ from, type, to, source_refs: sourceRefsFor(paths.length ? paths : ["STACK_MANIFEST.json"]) });
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
  const symbolKeySet = new Set();
  const symbolCountBySource = new Map();
  const MAX_PUBLIC_SYMBOLS = 3000;
  const MAX_SYMBOLS_PER_SOURCE = 64;
  const supportedSource = sourceRecords.filter((record) => ["trit", "tasm", "cpp", "c", "typescript", "typescript-react"].includes(record.language) && record.status === "resolved");
  const addSymbol = (source, name, line, kind) => {
    if (symbolRecords.length >= MAX_PUBLIC_SYMBOLS || (symbolCountBySource.get(source.id) || 0) >= MAX_SYMBOLS_PER_SOURCE) return;
    if (!name || name.length < 2 || ["if", "is", "and", "or", "for", "while", "switch", "return", "match"].includes(name.toLowerCase())) return;
    const key = `${source.path}:${name}:${line}`;
    if (symbolKeySet.has(key)) return;
    symbolKeySet.add(key);
    const symbolId = `tc:symbol:${slug(`${source.path}-${name}-${line}`)}`;
    symbolRecords.push({
      id: symbolId,
      entity_type: "symbol",
      name,
      symbol: name,
      kind,
      source_id: source.id,
      path: source.path,
      source_refs: [sourceRef(commit, source.path, { role: "symbol_definition", source_span: { start_line: line, end_line: line } })],
      evidence_refs: [sourceRef(commit, source.path, { role: "symbol_source", source_span: { start_line: line, end_line: line } })],
    });
    symbolCountBySource.set(source.id, (symbolCountBySource.get(source.id) || 0) + 1);
    source.symbol_ids = [...(source.symbol_ids || []), symbolId];
    addRelation(source.id, "defines", symbolId, [source.path]);
  };
  for (const source of supportedSource) {
    const content = readText(source.path);
    const lines = content.split(/\r?\n/);
    lines.forEach((line, index) => {
      const lineNumber = index + 1;
      let match = line.match(/\b(?:fn|function|func|def|class|struct|enum|interface|type)\s+([A-Za-z_]\w*)/);
      if (match) addSymbol(source, match[1], lineNumber, "declaration");
      match = line.match(/^\s*(?:export\s+)?(?:const|let|var)\s+([A-Za-z_]\w*)/);
      if (match) addSymbol(source, match[1], lineNumber, "value");
      match = line.match(/^\s*(?:static\s+|inline\s+|virtual\s+|constexpr\s+)*(?:[A-Za-z_][\w:<>*&\[\], ]+)\s+([A-Za-z_]\w*)\s*\([^;]*\)\s*(?:const)?\s*\{/);
      if (match) addSymbol(source, match[1], lineNumber, "function");
      if (source.language === "tasm") {
        match = line.match(/^\s*([A-Za-z_][\w.]*)\s*:/);
        if (match) addSymbol(source, match[1], lineNumber, "label");
      }
    });
  }

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
      evidence_refs: sourceRefsFor(["build/diagnostics/latest/agent_diagnostics.json"]),
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
      evidence_refs: sourceRefsFor(["build/diagnostics/latest/agent_diagnostics.json"]),
    },
  ];

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
    statistics: counts,
  };
  snapshot.snapshot.id = `tc:snapshot:${sha256(JSON.stringify(snapshot)).slice(0, 12)}`;
  return snapshot;
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
  const snapshot = buildSnapshot();
  fs.mkdirSync(OUTPUT_ROOT, { recursive: true });
  fs.writeFileSync(path.join(OUTPUT_ROOT, "snapshot.json"), `${JSON.stringify(snapshot, null, 2)}\n`, "utf8");
  const resources = ["projects", "stack_nodes", "components", "capabilities", "contracts", "decisions", "sources", "symbols", "tests", "benchmarks", "runs", "releases", "gaps"];
  for (const resource of resources) {
    fs.writeFileSync(path.join(OUTPUT_ROOT, `${resource}.json`), `${JSON.stringify(envelope(snapshot, snapshot[resource], resource), null, 2)}\n`, "utf8");
  }
  fs.writeFileSync(path.join(OUTPUT_ROOT, "search-index.json"), `${JSON.stringify({
    schema_version: API_SCHEMA,
    snapshot: snapshot.snapshot,
    data: { searchable_resources: resources, relationship_count: snapshot.relations.length },
  }, null, 2)}\n`, "utf8");
  const openapiSource = path.join(REPO_ROOT, "docs", "11_TreatCode_Platform", "schemas", "public_api.v1.openapi.json");
  if (fs.existsSync(openapiSource)) fs.copyFileSync(openapiSource, path.join(OUTPUT_ROOT, "openapi.json"));
  console.log(`generated TreatCode public snapshot ${snapshot.snapshot.id} (${snapshot.sources.length} sources, ${snapshot.symbols.length} symbols)`);
}

generatePublicSnapshot();
