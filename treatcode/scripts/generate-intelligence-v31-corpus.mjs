import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const outputRoot = path.resolve(scriptDir, "..", "..", "benchmarks", "intelligence-v3.1");

const groups = [
  ["algorithm_and_data_structure", [
    ["Deterministic cycle witness", "A dependency planner returns a partial order instead of the canonical cycle witness after an incremental edge update.", ["misleading_or_incomplete_tests", "conflicting_invariants"], ["Trit", "TypeScript"]],
    ["Bounded streaming median", "A fixed-capacity two-heap median drifts after duplicate-heavy eviction and signed boundary inputs.", ["performance_constraint", "conflicting_invariants"], ["Trit"]],
    ["Nested rollback union-find", "Nested checkpoints restore parents but corrupt rank and component counts after path compression.", ["misleading_or_incomplete_tests", "conflicting_invariants"], ["Trit", "C++"]],
    ["Weighted path tie contract", "A blocked-grid path solver finds the right distance but violates lexicographic predecessor selection across equal-cost routes.", ["initial_build_failure", "performance_constraint"], ["Trit"]],
  ]],
  ["balanced_ternary_and_numeric", [
    ["Multiword carry-chain addition", "Signed balanced-ternary carry propagation corrupts the adjacent word when positive and negative lanes cancel.", ["misleading_or_incomplete_tests", "conflicting_invariants"], ["Trit", "C++"]],
    ["Checked ternary multiply", "Overflow detection rejects valid negative products and accepts one boundary product whose magnitude is unrepresentable.", ["conflicting_invariants", "abi_or_integration_bug"], ["Trit"]],
    ["Canonical token decoder", "A token decoder accepts redundant leading trits and exposes partial output before checksum validation completes.", ["misleading_or_incomplete_tests", "abi_or_integration_bug"], ["Trit", "TypeScript"]],
  ]],
  ["compiler_and_codegen", [
    ["Recovering precedence parser", "Malformed nested ternary expressions shift associativity after recovery and poison the following declaration.", ["initial_build_failure", "abi_or_integration_bug", "conflicting_invariants"], ["Trit", "C++"]],
    ["Minimal SSA phi placement", "A loop with two backedges receives redundant phi nodes while one reachable predecessor remains unrepresented.", ["misleading_or_incomplete_tests", "conflicting_invariants"], ["Trit", "C++"]],
    ["Checked constant folding", "Compile-time folding changes the observable failure mode of checked arithmetic near the t40 boundary.", ["initial_build_failure", "abi_or_integration_bug"], ["Trit", "C++"]],
    ["Spill-slot lifetime reuse", "Register-pressure lowering aliases two spill slots across a branch join and only fails after a nested call.", ["performance_constraint", "conflicting_invariants", "abi_or_integration_bug"], ["Trit", "C++"]],
    ["Deterministic module linker", "Module initialization order depends on filesystem enumeration and misresolves one weak symbol cycle.", ["initial_build_failure", "abi_or_integration_bug", "misleading_or_incomplete_tests"], ["Trit", "C++", "CMake"]],
  ]],
  ["memory_and_pointer_safety", [
    ["Alias-range validator", "Half-open pointer spans pass independently but their overflow-safe alias check misses a wrapped overlap.", ["misleading_or_incomplete_tests", "conflicting_invariants"], ["Trit", "C++"]],
    ["Arena checkpoint rollback", "A failed nested allocation rewinds capacity without restoring generation metadata and exposes a stale handle.", ["initial_build_failure", "conflicting_invariants"], ["Trit"]],
    ["Move-state propagation", "Ownership transfer updates the value state but leaves one sibling control-flow edge marked readable.", ["abi_or_integration_bug", "conflicting_invariants"], ["Trit", "C++"]],
  ]],
  ["concurrency_and_state", [
    ["ABA-safe queue handoff", "A bounded queue passes sequential tests but loses a slot when two claims wrap through the neutral generation.", ["misleading_or_incomplete_tests", "conflicting_invariants"], ["Trit", "C++"]],
    ["Scheduler wakeup ordering", "A wakeup racing with timeout expiry duplicates one runnable entry and violates deterministic priority order.", ["performance_constraint", "conflicting_invariants"], ["Trit", "C++"]],
    ["Redo-WAL recovery fence", "Recovery replays committed values but advances the durable watermark across an incomplete ternary record.", ["initial_build_failure", "abi_or_integration_bug", "conflicting_invariants"], ["Trit", "C++"]],
  ]],
  ["syscall_and_abi", [
    ["Generated syscall wrapper drift", "Kernel dispatch and the generated application wrapper disagree on one argument register after an ABI extension.", ["initial_build_failure", "abi_or_integration_bug"], ["Trit", "C++", "JSON"]],
    ["Partial I/O result policy", "A short write followed by a negative status discards committed progress under one caller policy.", ["misleading_or_incomplete_tests", "abi_or_integration_bug", "conflicting_invariants"], ["Trit", "C++"]],
    ["Capability handle narrowing", "A valid high-generation handle is truncated by the SDK wrapper and collides with a revoked entry.", ["initial_build_failure", "abi_or_integration_bug"], ["Trit", "C++"]],
  ]],
  ["multi_module_repository_repair", [
    ["Configuration precedence repair", "Environment, manifest, and runtime overrides produce different effective limits in the CLI and service paths.", ["misleading_or_incomplete_tests", "conflicting_invariants", "abi_or_integration_bug"], ["Trit", "TypeScript", "JSON"]],
    ["Dependency-aware cache invalidation", "Changing an imported Trit type invalidates direct consumers but leaves a transitive codegen artifact stale.", ["initial_build_failure", "performance_constraint", "conflicting_invariants"], ["Trit", "C++", "CMake"]],
    ["Image-format migration contract", "A v2-to-v3 image migration preserves payload bytes but breaks alignment and feature flags on the second segment.", ["abi_or_integration_bug", "conflicting_invariants", "misleading_or_incomplete_tests"], ["Trit", "C++", "JSON"]],
    ["Service lifecycle handoff", "Restart recovery restores process state but duplicates one device lease and loses the original failure cause.", ["initial_build_failure", "abi_or_integration_bug", "conflicting_invariants"], ["Trit", "TypeScript"]],
  ]],
  ["build_test_and_tooling", [
    ["Incremental dependency rebuild", "A header-only Trit ABI change skips the native wrapper target unless a clean build is performed.", ["initial_build_failure", "misleading_or_incomplete_tests"], ["Trit", "CMake", "PowerShell"]],
    ["Generated manifest consistency", "The generator and validator canonicalize equivalent task data differently and produce unstable evidence hashes.", ["abi_or_integration_bug", "conflicting_invariants"], ["Trit", "TypeScript", "JSON"]],
    ["Shard discovery isolation", "Focused tests pass, but production discovery silently omits a nested adversarial shard on Windows paths.", ["initial_build_failure", "misleading_or_incomplete_tests", "abi_or_integration_bug"], ["Trit", "TypeScript", "PowerShell"]],
  ]],
  ["performance_and_resource_constraints", [
    ["Bounded symbol lookup", "A correct scope resolver becomes quadratic on shadow-heavy modules and exceeds the fixed repository budget.", ["performance_constraint", "misleading_or_incomplete_tests"], ["Trit", "C++"]],
    ["Packed-lane transform fusion", "A correct ternary lane transform repeatedly unpacks values and violates the cycle and allocation ceilings.", ["performance_constraint", "conflicting_invariants"], ["Trit", "C++"]],
  ]],
];

const tasks = [];
for (const [category, items] of groups) {
  for (const [title, symptom, extraFeatures, languages] of items) {
    const index = tasks.length + 1;
    tasks.push({
      task_id: `TC-V31-PILOT-${String(index).padStart(3, "0")}`,
      phase: "pilot",
      disposable: true,
      holdout_eligible_for: { "gpt-5.6-luna": false, "gpt-5.6-sol": false },
      category,
      title,
      initial_signal: symptom,
      languages,
      required_trit_change: true,
      design_features: ["repository_exploration_required", "multi_module_change", ...extraFeatures],
      repository_contract: {
        minimum_files: 8,
        minimum_editable_files: 3,
        minimum_changed_modules: 2,
        sparse_public_coverage: true,
        defect_identifying_comments_forbidden: true,
        raw_commands_forbidden: true,
      },
      package_status: "needs_executable_participant_and_private_grader",
      model_attempt_status: { "gpt-5.6-luna:max": "not_started", "gpt-5.6-sol:high": "not_started" },
      disposition: "pending_package",
    });
  }
}

if (tasks.length !== 30) throw new Error(`expected 30 pilot tasks, found ${tasks.length}`);
const categoryCounts = Object.fromEntries(groups.map(([category]) => [category, tasks.filter((task) => task.category === category).length]));
const featureCounts = {};
for (const task of tasks) for (const feature of task.design_features) featureCounts[feature] = (featureCounts[feature] || 0) + 1;
const manifest = {
  schema: "treatcode.intelligence.pilot-corpus.v3.1",
  version: "3.1",
  status: "design_complete_packages_pending",
  official: false,
  disposable: true,
  task_count: tasks.length,
  warning: "Pilot tasks are exposed development instruments. They can inform task-family design but can never enter a Luna/Sol holdout.",
  category_counts: categoryCounts,
  design_feature_counts: featureCounts,
  tasks,
};
await mkdir(outputRoot, { recursive: true });
await writeFile(path.join(outputRoot, "pilot-corpus.v3.1.json"), `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
console.log(JSON.stringify({ output: path.join(outputRoot, "pilot-corpus.v3.1.json"), task_count: tasks.length, category_counts: categoryCounts }, null, 2));
