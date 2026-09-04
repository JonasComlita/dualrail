import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";

const repoRoot = path.resolve(import.meta.dirname, "..", "..");
const outputPath = path.join(repoRoot, "benchmarks", "intelligence-v3", "corpus.candidates.v3.json");

const categories = {
  algorithm_and_data_structure: [
    ["Stable three-way partition", "Repair a partitioner that loses stable order when sentinel values are present."],
    ["Overflow-safe interval merge", "Diagnose interval merging that wraps endpoint arithmetic at the representable boundary."],
    ["Dependency cycle witness", "Repair topological sorting so it returns a deterministic cycle witness instead of a partial order."],
    ["Bounded streaming median", "Fix two-heap balancing and duplicate handling under a fixed memory budget."],
    ["Weighted path tie-breaking", "Repair shortest-path selection with blocked cells and a deterministic lexicographic tie contract."],
    ["Rollback union-find", "Restore parent, rank, and component-count invariants across nested rollback checkpoints."],
    ["Wrapping ring buffer", "Fix full-versus-empty ambiguity and multi-step wraparound without reallocating storage."],
    ["Sparse vector intersection", "Repair ordered sparse dot product with duplicate coordinates and bounded accumulation."],
    ["Signed ternary radix ordering", "Fix stable digit passes for negative, zero, and positive balanced-ternary keys."],
    ["Capacity-safe LRU", "Repair list/map coherence during update, eviction, and zero-capacity operation."],
    ["Rotated search with duplicates", "Fix boundary elimination when a rotated sorted sequence contains repeated pivots."],
    ["Stable interval scheduler", "Repair endpoint semantics and original-index tie-breaking in activity selection."],
    ["DAG longest-path sentinel", "Distinguish unreachable nodes from valid negative path totals in a DAG solver."],
    ["Prefix trie deletion", "Repair reference counts so deleting one key never removes a shared prefix."],
    ["K-way cursor merge", "Fix exhausted-cursor handling and stable source ordering without materializing all inputs."],
  ],
  balanced_ternary_and_numeric: [
    ["Canonical token decoder", "Reject noncanonical balanced-ternary encodings before numeric conversion."],
    ["Carry-chain addition", "Repair multiword balanced-ternary carry propagation across positive and negative lanes."],
    ["Checked ternary multiply", "Detect representational overflow without rejecting valid negative products."],
    ["Fixed-point rounding", "Implement ties-to-even balanced-ternary rounding for signed fixed-point values."],
    ["Syndrome checksum", "Repair a ternary checksum that confuses digit value with encoded digit position."],
    ["Packed lane mask", "Fix tritwise masking without scalar unpack/repack or cross-lane contamination."],
    ["Euclidean division", "Repair quotient and remainder signs for negative balanced-ternary operands."],
    ["Bounded base conversion", "Reject out-of-range decimal conversion before partial output becomes observable."],
    ["Ternary parity recovery", "Locate and repair one corrupted trit while rejecting ambiguous two-error syndromes."],
    ["Normalized rational", "Repair signed gcd normalization and checked numerator/denominator arithmetic."],
  ],
  compiler_and_codegen: [
    ["Expression precedence parser", "Repair associativity and recovery after a malformed nested ternary expression."],
    ["Comment-aware lexer", "Fix token locations across nested comments, newlines, and unterminated input."],
    ["Shadowed scope resolver", "Prevent symbol leakage across sibling blocks while preserving legal shadowing."],
    ["SSA phi construction", "Insert minimal phi nodes for a loop with multiple backedges and unreachable predecessors."],
    ["Overflow-safe folding", "Stop constant folding from changing checked-arithmetic failure behavior."],
    ["Side-effect-aware DCE", "Preserve volatile loads and trapping expressions whose result is otherwise unused."],
    ["Spill slot allocator", "Repair overlapping spill slots across nested calls and exceptional control flow."],
    ["Nested-call ABI", "Restore caller/callee saved registers and stack alignment across a nested call chain."],
    ["Exhaustive match checker", "Distinguish guarded arms from total coverage and report the missing sign case."],
    ["Relocation range repair", "Choose or synthesize a long relocation when a short branch no longer reaches."],
    ["Weak symbol resolution", "Repair deterministic strong/weak symbol selection across archive orderings."],
    ["Debug line mapping", "Preserve source locations through desugaring and instruction scheduling."],
    ["Alias-safe load folding", "Prevent an optimization from reusing a load across a potentially aliasing store."],
    ["Tail-call eligibility", "Reject tail-call conversion when ABI cleanup or result adaptation remains."],
    ["Narrow-value extension", "Repair signed versus unsigned extension at a ternary machine-code boundary."],
  ],
  memory_and_pointer_safety: [
    ["Overflowing span validator", "Validate half-open spans without trusting address-plus-length arithmetic."],
    ["Arena ownership boundary", "Reject pointers from a sibling arena even when numeric offsets overlap."],
    ["Exact alignment contract", "Distinguish unsupported alignment from supported-but-misaligned addresses."],
    ["Generation-tagged handles", "Prevent a recycled allocation slot from validating a stale handle."],
    ["Reference-count handoff", "Repair a retain/release transfer that frees an object during callback reentrancy."],
    ["Negative-length copy", "Reject malformed lengths before pointer normalization or destination writes."],
    ["Overlapping move", "Repair copy direction for partially overlapping source and destination spans."],
    ["Allocator coalescing", "Merge only physically adjacent free blocks while preserving ordered free lists."],
    ["Stack red-zone check", "Detect a frame-size rounding bug that writes across the protected boundary."],
    ["Capability permissions", "Require bounds and permissions before deriving a writable child capability."],
  ],
  concurrency_and_state: [
    ["Linearizable bounded queue", "Repair a lost wakeup and full-slot publication race under two producers."],
    ["Mutex ownership transfer", "Reject unlock by a non-owner without stranding an eligible waiter."],
    ["Saturating semaphore", "Prevent overflow and preserve FIFO wake ordering across release bursts."],
    ["Ordered event reducer", "Apply reset, toggle, and saturation events in observable input order."],
    ["ABA-resistant stack", "Repair a compare-and-swap stack with generation-tagged head updates."],
    ["Cancellation handoff", "Ensure exactly one completion wins when cancellation races successful delivery."],
    ["Channel close protocol", "Drain buffered items before closed status and wake all blocked endpoints once."],
    ["Priority donation", "Propagate and revoke inherited priority through a nested lock dependency."],
    ["Once initialization", "Retry after initialization failure without exposing partially initialized state."],
    ["Transactional rollback", "Restore all state fields when the second ordered transition fails."],
  ],
  syscall_and_abi: [
    ["Kernel status normalization", "Preserve negative kernel errors while validating successful result policies."],
    ["User pointer syscall", "Validate range, ownership, and write permission before dispatching a syscall."],
    ["Partial I/O accounting", "Return transferred length with correct restart behavior after an interrupt."],
    ["Errno precedence", "Repair wrapper ordering so kernel failure outranks caller-side result adaptation."],
    ["Descriptor lifecycle", "Prevent double-close and stale descriptor reuse across process handoff."],
    ["Capability-gated dispatch", "Check caller capability before argument-dependent kernel side effects."],
    ["Restartable signal boundary", "Restore arguments and program counter only for restartable interruptions."],
    ["Monotonic time conversion", "Detect unit-conversion overflow and reject invalid clock identifiers first."],
    ["Tagged ioctl payload", "Validate variant tag, payload size, and direction before copying user memory."],
    ["Wait status adapter", "Distinguish exited, signaled, stopped, and no-child outcomes without value collisions."],
  ],
  multi_module_repository_repair: [
    ["Atomic configuration reload", "Repair parse, validate, publish, and rollback behavior across configuration modules."],
    ["Dependency-aware cache", "Invalidate transitive dependents without evicting unrelated cached artifacts."],
    ["Session rotation", "Repair authentication token rotation, revocation, persistence, and audit ordering."],
    ["Crash-safe file journal", "Recover committed entries and discard torn writes across storage modules."],
    ["Package constraint resolver", "Repair backtracking, prerelease rules, and deterministic conflict explanations."],
    ["Predicate pushdown planner", "Preserve null semantics while moving filters across an outer join."],
    ["Message retry router", "Prevent duplicate delivery while retaining dead-letter provenance."],
    ["Image manifest upgrader", "Migrate versioned image entries without changing content hashes or boot order."],
    ["Plugin dependency loader", "Reject cycles and incompatible APIs before running plugin initialization."],
    ["Snapshot restore", "Restore schema, data, indexes, and sequence state as one validated operation."],
    ["Idempotent migration", "Make a partially applied multi-step migration safe to resume."],
    ["Tamper-evident audit log", "Repair hash chaining and concurrent append ordering across log segments."],
    ["Offline sync conflict", "Resolve delete/update conflicts consistently across client and server replicas."],
    ["Feature flag snapshot", "Keep request-scoped flag decisions stable during concurrent configuration updates."],
    ["Generated API pipeline", "Repair schema generation, client emission, validation, and stale-artifact detection."],
  ],
  build_test_and_tooling: [
    ["Incremental dependency invalidation", "Rebuild transitive consumers when a generated interface changes."],
    ["Deterministic flaky-test replay", "Capture and replay randomness, ordering, and timing inputs for a failing test."],
    ["Cross-platform command quoting", "Repair argument preservation for spaces, quotes, and literal metacharacters."],
    ["Environment precedence", "Resolve command, project, user, and default configuration without leaking host values."],
    ["Artifact hash manifest", "Detect missing, extra, stale, and content-modified release artifacts."],
    ["Cache poisoning guard", "Bind build-cache entries to compiler, flags, inputs, and target architecture."],
    ["Cross-compile probe", "Separate runnable host probes from compile-only target capability checks."],
    ["Test discovery isolation", "Prevent generated fixtures and quarantined tests from entering the production suite."],
    ["Schema code generation", "Reject incompatible schema evolution and remove obsolete generated members."],
    ["Release package closure", "Include runtime dependencies while excluding secrets, caches, and developer evidence."],
  ],
  performance_and_resource_constraints: [
    ["Linear-time tokenization", "Remove quadratic rescanning while preserving exact error locations."],
    ["Allocation-bounded traversal", "Eliminate per-node allocation in a hot graph walk under a fixed memory cap."],
    ["Vectorized trit packing", "Use lane-parallel operations without changing scalar reference results."],
    ["Batched syscall writer", "Reduce kernel crossings while preserving partial-write and error semantics."],
    ["Content-addressed image dedupe", "Avoid duplicate payload storage without corrupting offsets or integrity hashes."],
  ],
};

const expectedCounts = {
  algorithm_and_data_structure: 15,
  balanced_ternary_and_numeric: 10,
  compiler_and_codegen: 15,
  memory_and_pointer_safety: 10,
  concurrency_and_state: 10,
  syscall_and_abi: 10,
  multi_module_repository_repair: 15,
  build_test_and_tooling: 10,
  performance_and_resource_constraints: 5,
};

assert.deepEqual(Object.fromEntries(Object.entries(categories).map(([category, tasks]) => [category, tasks.length])), expectedCounts);

let ordinal = 0;
const tasks = Object.entries(categories).flatMap(([category, briefs]) => briefs.map(([title, symptom], localIndex) => {
  ordinal += 1;
  const designFeatures = ["repository_exploration_required", "multi_module_change"];
  if (ordinal % 5 === 0) designFeatures.push("initial_build_failure");
  if (ordinal % 4 === 0) designFeatures.push("misleading_or_incomplete_tests");
  if (category === "performance_and_resource_constraints" || ordinal % 10 === 1) designFeatures.push("performance_constraint");
  if (category === "syscall_and_abi" || (category === "compiler_and_codegen" && localIndex < 10)) designFeatures.push("abi_or_integration_bug");
  if (category === "concurrency_and_state" || category === "multi_module_repository_repair" || (category === "memory_and_pointer_safety" && localIndex < 5)) designFeatures.push("conflicting_invariants");
  return {
    id: `TC-V3-${String(ordinal).padStart(3, "0")}`,
    category,
    title,
    symptom,
    design_features: designFeatures,
    state: "needs_independent_author",
    authoring_contract: {
      minimum_modules: category === "multi_module_repository_repair" ? 4 : 2,
      require_behavioral_hidden_grader: true,
      require_adversarial_suite: true,
      require_misleading_or_incomplete_initial_signal: true,
      require_reference_solution_outside_participant_bundle: true,
    },
    provenance: {
      author_id: null,
      reviewer_ids: [],
      subject_model_independence_verified: false,
    },
    packaging: { participant_bundle_hash: null, grader_bundle_hash: null, isolation_audit_hash: null },
    calibration: { status: "not_started", cohort_pass_rates: {} },
    holdout: { frozen: false, manifest_hash: null },
  };
}));

assert.equal(tasks.length, 100);
assert.equal(new Set(tasks.map((task) => task.id)).size, 100);
assert.equal(new Set(tasks.map((task) => task.title)).size, 100);
const featureCounts = Object.fromEntries(Object.keys({
  repository_exploration_required: 0,
  multi_module_change: 0,
  initial_build_failure: 0,
  misleading_or_incomplete_tests: 0,
  performance_constraint: 0,
  abi_or_integration_bug: 0,
  conflicting_invariants: 0,
}).map((feature) => [feature, tasks.filter((task) => task.design_features.includes(feature)).length]));
assert.deepEqual(featureCounts, {
  repository_exploration_required: 100,
  multi_module_change: 100,
  initial_build_failure: 20,
  misleading_or_incomplete_tests: 25,
  performance_constraint: 15,
  abi_or_integration_bug: 20,
  conflicting_invariants: 30,
});

const corpus = {
  schema: "treatcode.intelligence.corpus-candidates.v3",
  version: 3,
  status: "authoring",
  official: false,
  task_count: tasks.length,
  warning: "These are independently scoped authoring briefs, not executable or calibrated benchmark tasks. Never publish a model score from this file.",
  category_counts: expectedCounts,
  design_feature_counts: featureCounts,
  tasks,
};

await mkdir(path.dirname(outputPath), { recursive: true });
await writeFile(outputPath, `${JSON.stringify(corpus, null, 2)}\n`, "utf8");
console.log(JSON.stringify({ status: "passed", output: outputPath, task_count: tasks.length, official: false }, null, 2));
