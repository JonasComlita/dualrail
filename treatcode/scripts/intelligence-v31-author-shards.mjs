import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

export const AUTHOR_SHARD_PLAN_FILE = "author-shard-plan.v3.1.json";
export const AUTHOR_CONTRIBUTION_SCHEMA = "treatcode.intelligence.author-contribution.v3.1";
export const AUTHOR_RECEIPT_SCHEMA = "treatcode.intelligence.author-shard-receipt.v3.1";
export const FEATURE_KEYS = [
  "repository_exploration_required",
  "multi_module_change",
  "misleading_or_incomplete_tests",
  "conflicting_invariants",
  "initial_build_failure",
  "abi_or_integration_bug",
  "performance_constraint",
];
export const TRACK_IDS = [
  "fresh_coding",
  "repository_repair",
  "terminal_agent",
  "expert_reasoning",
  "frontier_math",
  "abstract_generalization",
  "multimodal_reasoning",
  "web_research",
];
const CANONICAL_MODEL_FAMILY = /^[a-z0-9]+(?:[.-][a-z0-9]+)*$/;

export function sha256(value) {
  return crypto.createHash("sha256").update(value).digest("hex");
}

export function canonicalize(value) {
  if (Array.isArray(value)) return value.map(canonicalize);
  if (value && typeof value === "object") {
    return Object.fromEntries(Object.entries(value)
      .sort(([left], [right]) => left.localeCompare(right))
      .map(([key, item]) => [key, canonicalize(item)]));
  }
  return value;
}

export function canonicalHash(value) {
  return sha256(JSON.stringify(canonicalize(value)));
}

export function authorShardPaths(repositoryRoot, checkpointRootOverride) {
  const corpusRoot = path.join(repositoryRoot, "benchmarks", "intelligence-v3.1");
  const localBase = process.env.LOCALAPPDATA || path.join(os.tmpdir(), "TreatCode-local");
  return {
    corpusRoot,
    planPath: path.join(corpusRoot, AUTHOR_SHARD_PLAN_FILE),
    stagedRoot: path.join(corpusRoot, "authoring", "contributions"),
    checkpointRoot: checkpointRootOverride || process.env.TREATCODE_V31_AUTHOR_CHECKPOINT_ROOT || path.join(localBase, "TreatCode", "intelligence-v31-private", "author-shard-checkpoints"),
    evidenceRoot: path.join(repositoryRoot, "build", "treatcode-plan-evidence", "P14", "author-shard-checkpoints"),
  };
}

function sameObject(left, right) {
  return JSON.stringify(left) === JSON.stringify(right);
}

function safeRelativePath(value) {
  return typeof value === "string" && value.length > 0 && !path.isAbsolute(value) && !value.includes("\\")
    && value.split("/").every((part) => part && part !== "." && part !== "..");
}

function withinOrSame(parent, child) {
  const relative = path.relative(parent, child);
  return relative === "" || (!relative.startsWith("..") && !path.isAbsolute(relative));
}

function exactKeys(value, keys) {
  return value && typeof value === "object" && !Array.isArray(value)
    && sameObject(Object.keys(value).sort(), [...keys].sort());
}

function countOccurrences(text, needle) {
  if (!needle) return 0;
  let total = 0;
  let offset = 0;
  while ((offset = text.indexOf(needle, offset)) >= 0) {
    total += 1;
    offset += needle.length;
  }
  return total;
}

function normalizeText(value) {
  return value.replaceAll("\r\n", "\n");
}

function commentsIdentifyDefect(value) {
  const comments = [
    ...(value.match(/\/\/[^\n\r]*/g) || []),
    ...(value.match(/\/\*[\s\S]*?\*\//g) || []),
    ...(value.match(/#[^\n\r]*/g) || []),
  ].join("\n");
  return /\b(?:bug|defect|broken|incorrect|wrong|fixme|todo\s*:\s*fix|repair\s+here|intentional(?:ly)?\s+bad)\b/i.test(comments);
}

export function isExcludedSubjectModel(model) {
  const normalized = `${model || ""}`.trim().toLowerCase();
  return /(?:^|[/:])gpt-5\.6-(?:luna|sol)(?:$|[-_\s/:])/.test(normalized);
}

export function loadAuthorShardPlan(repositoryRoot) {
  const { planPath } = authorShardPaths(repositoryRoot);
  const bytes = fs.readFileSync(planPath);
  const plan = JSON.parse(bytes.toString("utf8"));
  const errors = [];
  if (plan.schema !== "treatcode.intelligence.author-shard-plan.v3.1" || plan.version !== "3.1") errors.push("invalid author shard plan schema");
  if (plan.task_count !== 100 || plan.shard_count !== 20 || plan.tasks_per_shard !== 5 || !Array.isArray(plan.shards) || plan.shards.length !== 20) errors.push("author shard plan must contain twenty five-task shards");
  if (!plan.track_mix || typeof plan.track_mix !== "object" || Object.values(plan.track_mix).reduce((sum, count) => sum + count, 0) !== 100) errors.push("author shard track mix must total 100 tasks");
  const shardIds = plan.shards?.map((shard) => shard.shard_id) || [];
  if (new Set(shardIds).size !== shardIds.length) errors.push("author shard ids must be unique");
  const assignments = (plan.shards || []).map((shard, shardIndex) => {
    const taskIds = Array.from({ length: plan.tasks_per_shard || 0 }, (_, taskIndex) => `TC-V31-FINAL-${String(shard.first_task_number + taskIndex).padStart(3, "0")}`);
    const featureMinimums = Object.fromEntries(FEATURE_KEYS.map((feature) => [feature,
      (plan.default_feature_minimums_per_shard?.[feature] || 0) + (plan.feature_bonus_shards?.[feature]?.includes(shard.shard_id) ? 1 : 0),
    ]));
    const expectedShardId = `V31-SHARD-${String(shardIndex + 1).padStart(3, "0")}`;
    if (shard.shard_id !== expectedShardId || shard.first_task_number !== shardIndex * 5 + 1 || !Array.isArray(shard.categories) || shard.categories.length !== 5 || !Array.isArray(shard.tracks) || shard.tracks.length !== 5 || shard.tracks.some((track) => !TRACK_IDS.includes(track))) errors.push(`${shard.shard_id || shardIndex}: shard range, category, or track slot count is invalid`);
    return { ...shard, file: `${shard.shard_id}.json`, task_ids: taskIds, feature_minimums: featureMinimums };
  });
  const taskIds = assignments.flatMap((assignment) => assignment.task_ids);
  if (taskIds.length !== 100 || new Set(taskIds).size !== 100 || taskIds[0] !== "TC-V31-FINAL-001" || taskIds.at(-1) !== "TC-V31-FINAL-100") errors.push("author shard task ranges do not cover final tasks 001 through 100 exactly once");
  const categoryCounts = Object.fromEntries(Object.keys(plan.category_mix || {}).map((category) => [category, assignments.flatMap((assignment) => assignment.categories).filter((value) => value === category).length]));
  if (!sameObject(categoryCounts, plan.category_mix)) errors.push("author shard category slots do not match the final category mix");
  const trackCounts = Object.fromEntries(Object.keys(plan.track_mix || {}).map((track) => [track, assignments.flatMap((assignment) => assignment.tracks || []).filter((value) => value === track).length]));
  if (!sameObject(trackCounts, plan.track_mix)) errors.push("author shard track slots do not match the final track mix");
  for (const [feature, shardList] of Object.entries(plan.feature_bonus_shards || {})) {
    if (!FEATURE_KEYS.includes(feature) || !Array.isArray(shardList) || shardList.some((shardId) => !shardIds.includes(shardId)) || new Set(shardList).size !== shardList.length) errors.push(`invalid feature bonus assignment for ${feature}`);
  }
  if (errors.length) throw new Error(errors.join("; "));
  return { plan, planBytes: bytes, planSha256: sha256(bytes), planPath, assignments, categoryCounts, trackCounts };
}

export function createAuthorShardScaffold({ repositoryRoot, shardId, contributorId, model, reasoningEffort }) {
  const loaded = loadAuthorShardPlan(repositoryRoot);
  const assignment = loaded.assignments.find((item) => item.shard_id === shardId);
  if (!assignment) throw new Error(`unknown author shard: ${shardId}`);
  if (`${contributorId || ""}`.length < 3) throw new Error("a stable contributor id is required");
  if (!CANONICAL_MODEL_FAMILY.test(`${model || ""}`) || isExcludedSubjectModel(model)) throw new Error("a canonical non-Luna/non-Sol author model is required");
  if (`${reasoningEffort || ""}`.length < 2) throw new Error("the author reasoning effort is required");
  return {
    schema: AUTHOR_CONTRIBUTION_SCHEMA,
    version: "3.1",
    shard_id: shardId,
    authorship: { contributor_id: contributorId, contributor_kind: "model_agent", model, reasoning_effort: reasoningEffort, completed_at: "", attestation: "" },
    tasks: assignment.task_ids.map((taskId, index) => ({
      task_id: taskId,
      track: assignment.tracks[index],
      category: assignment.categories[index],
      title: "",
      initial_signal: "",
      participant_files: [],
      editable_files: [],
      mutations: [],
      distractor_files: [],
      public_signal: "",
      focused_suites: [],
      focused_targets: [],
      required_interaction: "",
      invariant: "",
      hidden_test_rationale: "",
      design_features: Object.fromEntries(FEATURE_KEYS.map((feature) => [feature, feature === "repository_exploration_required"])),
    })),
  };
}

function validateAuthorship(authorship, errors, label) {
  const keys = ["contributor_id", "contributor_kind", "model", "reasoning_effort", "completed_at", "attestation"];
  if (!exactKeys(authorship, keys)) errors.push(`${label}: authorship fields are incomplete or unsupported`);
  if (`${authorship?.contributor_id || ""}`.length < 3 || authorship?.contributor_kind !== "model_agent" || !CANONICAL_MODEL_FAMILY.test(`${authorship?.model || ""}`) || `${authorship?.reasoning_effort || ""}`.length < 2 || !Number.isFinite(Date.parse(authorship?.completed_at || "")) || `${authorship?.attestation || ""}`.length < 40) errors.push(`${label}: authorship metadata is incomplete or noncanonical`);
  if (isExcludedSubjectModel(authorship?.model)) errors.push(`${label}: a Luna or Sol subject-model family cannot author final candidates`);
}

function validateTask(task, assignment, index, context, errors) {
  const label = `${assignment.shard_id}/${task?.task_id || `task-${index + 1}`}`;
  const taskKeys = ["task_id", "track", "category", "title", "initial_signal", "participant_files", "editable_files", "mutations", "distractor_files", "public_signal", "focused_suites", "focused_targets", "required_interaction", "invariant", "hidden_test_rationale", "design_features"];
  if (!exactKeys(task, taskKeys)) errors.push(`${label}: task fields are incomplete or unsupported`);
  if (task?.task_id !== assignment.task_ids[index] || task?.category !== assignment.categories[index]) errors.push(`${label}: task id or category does not match its assigned slot`);
  if (!TRACK_IDS.includes(task?.track) || task?.track !== assignment.tracks[index]) errors.push(`${label}: track is missing or does not match its assigned slot`);
  for (const [field, minimum] of [["title", 12], ["initial_signal", 12], ["public_signal", 8], ["required_interaction", 12], ["invariant", 12], ["hidden_test_rationale", 12]]) {
    if (typeof task?.[field] !== "string" || task[field].length < minimum) errors.push(`${label}: ${field} is incomplete`);
  }
  const participantFiles = task?.participant_files;
  const editableFiles = task?.editable_files;
  const distractorFiles = task?.distractor_files;
  if (!Array.isArray(participantFiles) || participantFiles.length < 8 || new Set(participantFiles).size !== participantFiles.length || participantFiles.some((file) => !safeRelativePath(file))) errors.push(`${label}: participant_files must contain at least eight safe distinct paths`);
  if (!Array.isArray(editableFiles) || editableFiles.length < 3 || new Set(editableFiles).size !== editableFiles.length || editableFiles.some((file) => !participantFiles?.includes(file))) errors.push(`${label}: editable_files must contain at least three participant paths`);
  if (!editableFiles?.some((file) => file.endsWith(".trit"))) errors.push(`${label}: at least one editable Trit file is required`);
  if (!Array.isArray(distractorFiles) || distractorFiles.length < 3 || new Set(distractorFiles).size !== distractorFiles.length || distractorFiles.some((file) => !safeRelativePath(file))) errors.push(`${label}: distractor_files must contain at least three safe distinct paths`);
  for (const file of new Set([...(participantFiles || []), ...(distractorFiles || [])])) {
    const source = path.join(context.repositoryRoot, file);
    if (!fs.existsSync(source) || !fs.statSync(source).isFile()) {
      errors.push(`${label}: repository source file does not exist: ${file}`);
      continue;
    }
    const resolved = fs.realpathSync(source);
    if (!withinOrSame(fs.realpathSync(context.repositoryRoot), resolved)) errors.push(`${label}: repository source path escapes through a link: ${file}`);
  }
  if (!Array.isArray(task?.mutations) || task.mutations.length < 2 || new Set(task.mutations?.map((mutation) => mutation.path)).size < 2) errors.push(`${label}: at least two cross-file mutations are required`);
  for (const mutation of task?.mutations || []) {
    const mutationKeys = ["path", "before", "after", "invariant", "hidden_test_rationale"];
    if (!exactKeys(mutation, mutationKeys) || !editableFiles?.includes(mutation.path) || typeof mutation.before !== "string" || typeof mutation.after !== "string" || !mutation.before || !mutation.after || mutation.before === mutation.after || `${mutation.invariant || ""}`.length < 12 || `${mutation.hidden_test_rationale || ""}`.length < 12) {
      errors.push(`${label}: malformed mutation for ${mutation?.path || "unknown path"}`);
      continue;
    }
    if (commentsIdentifyDefect(mutation.after)) errors.push(`${label}: mutation for ${mutation.path} contains a defect-identifying comment`);
    const sourcePath = path.join(context.repositoryRoot, mutation.path);
    if (!fs.existsSync(sourcePath) || !fs.statSync(sourcePath).isFile()) continue;
    const occurrences = countOccurrences(normalizeText(fs.readFileSync(sourcePath, "utf8")), normalizeText(mutation.before));
    if (occurrences !== 1) errors.push(`${label}: mutation anchor occurs ${occurrences} times in ${mutation.path}, expected exactly once`);
  }
  if (!Array.isArray(task?.focused_suites) || task.focused_suites.length === 0 || new Set(task.focused_suites).size !== task.focused_suites.length || task.focused_suites.some((suite) => !context.suiteNames.has(suite))) errors.push(`${label}: focused_suites contains an unknown or duplicate suite`);
  if (!Array.isArray(task?.focused_targets) || task.focused_targets.length === 0 || new Set(task.focused_targets).size !== task.focused_targets.length || task.focused_targets.some((target) => !context.targetNames.has(target))) errors.push(`${label}: focused_targets contains an unknown or duplicate target`);
  const features = task?.design_features;
  if (!exactKeys(features, FEATURE_KEYS) || features?.repository_exploration_required !== true || FEATURE_KEYS.some((feature) => typeof features?.[feature] !== "boolean")) errors.push(`${label}: design_features are incomplete`);
}

export function validateAuthorContribution(contribution, assignment, context) {
  const errors = [];
  if (!exactKeys(contribution, ["schema", "version", "shard_id", "authorship", "tasks"])) errors.push(`${assignment.shard_id}: contribution fields are incomplete or unsupported`);
  if (contribution?.schema !== AUTHOR_CONTRIBUTION_SCHEMA || contribution?.version !== "3.1" || contribution?.shard_id !== assignment.shard_id) errors.push(`${assignment.shard_id}: contribution schema or shard id is invalid`);
  validateAuthorship(contribution?.authorship, errors, assignment.shard_id);
  if (!Array.isArray(contribution?.tasks) || contribution.tasks.length !== 5) errors.push(`${assignment.shard_id}: exactly five tasks are required`);
  for (let index = 0; index < (contribution?.tasks || []).length; index += 1) validateTask(contribution.tasks[index], assignment, index, context, errors);
  const taskIds = contribution?.tasks?.map((task) => task.task_id) || [];
  if (!sameObject(taskIds, assignment.task_ids)) errors.push(`${assignment.shard_id}: task ids or order do not match the shard plan`);
  if (new Set(contribution?.tasks?.map((task) => task.title)).size !== contribution?.tasks?.length || new Set(contribution?.tasks?.map((task) => task.initial_signal)).size !== contribution?.tasks?.length) errors.push(`${assignment.shard_id}: titles and initial signals must be unique within the shard`);
  const featureCounts = Object.fromEntries(FEATURE_KEYS.map((feature) => [feature, (contribution?.tasks || []).filter((task) => task.design_features?.[feature]).length]));
  for (const [feature, minimum] of Object.entries(assignment.feature_minimums)) if ((featureCounts[feature] || 0) < minimum) errors.push(`${assignment.shard_id}: ${feature} has ${featureCounts[feature] || 0}, requires ${minimum}`);
  return { valid: errors.length === 0, errors, featureCounts };
}

function validationContext(repositoryRoot) {
  const testManifest = JSON.parse(fs.readFileSync(path.join(repositoryRoot, "TEST_MANIFEST.json"), "utf8"));
  return {
    repositoryRoot,
    suiteNames: new Set(testManifest.suites.map((suite) => suite.name)),
    targetNames: new Set(testManifest.suites.flatMap((suite) => [...(suite.targets || []), ...(suite.ctest_tests || [])])),
  };
}

export function readAuthorCheckpoint({ repositoryRoot, assignment, checkpointRoot }) {
  const root = checkpointRoot || authorShardPaths(repositoryRoot).checkpointRoot;
  const directory = path.join(root, assignment.shard_id);
  if (!fs.existsSync(directory)) return { exists: false, directory, errors: [] };
  const contributionPath = path.join(directory, "contribution.json");
  const receiptPath = path.join(directory, "receipt.json");
  const errors = [];
  if (!fs.existsSync(contributionPath) || !fs.existsSync(receiptPath)) return { exists: true, directory, contributionPath, receiptPath, errors: [`${assignment.shard_id}: incomplete immutable checkpoint`] };
  if (fs.lstatSync(directory).isSymbolicLink() || fs.lstatSync(contributionPath).isSymbolicLink() || fs.lstatSync(receiptPath).isSymbolicLink()) return { exists: true, directory, contributionPath, receiptPath, errors: [`${assignment.shard_id}: checkpoint paths must not be symbolic links`] };
  let text = "";
  let contribution;
  let receipt;
  try { text = fs.readFileSync(contributionPath, "utf8"); contribution = JSON.parse(text); } catch (error) { errors.push(`${assignment.shard_id}: checkpoint contribution is unreadable (${error.message})`); }
  try { receipt = JSON.parse(fs.readFileSync(receiptPath, "utf8")); } catch (error) { errors.push(`${assignment.shard_id}: checkpoint receipt is unreadable (${error.message})`); }
  const { planSha256 } = loadAuthorShardPlan(repositoryRoot);
  if (receipt) {
    if (receipt.schema !== AUTHOR_RECEIPT_SCHEMA || receipt.version !== "3.1" || receipt.official !== false || receipt.shard_id !== assignment.shard_id || receipt.source_file !== assignment.file) errors.push(`${assignment.shard_id}: checkpoint receipt identity is invalid`);
    if (receipt.shard_plan_sha256 !== planSha256 || receipt.contribution_sha256 !== sha256(text) || receipt.canonical_tasks_sha256 !== canonicalHash(contribution?.tasks || [])) errors.push(`${assignment.shard_id}: checkpoint receipt hash mismatch`);
    const expectedTaskHashes = Object.fromEntries((contribution?.tasks || []).map((task) => [task.task_id, canonicalHash(task)]));
    if (!sameObject(receipt.task_ids, assignment.task_ids) || !sameObject(receipt.task_sha256, expectedTaskHashes) || receipt.contributor_id !== contribution?.authorship?.contributor_id || receipt.model !== contribution?.authorship?.model) errors.push(`${assignment.shard_id}: checkpoint receipt payload mismatch`);
  }
  return { exists: true, directory, contributionPath, receiptPath, text, contribution, receipt, errors };
}

function durableWriteNew(filePath, content) {
  const descriptor = fs.openSync(filePath, "wx", 0o444);
  try {
    fs.writeFileSync(descriptor, content);
    fs.fsyncSync(descriptor);
  } finally {
    fs.closeSync(descriptor);
  }
  try { fs.chmodSync(filePath, 0o444); } catch {}
}

function ensureReceiptEvidence(receipt, evidenceRoot) {
  fs.mkdirSync(evidenceRoot, { recursive: true });
  const evidencePath = path.join(evidenceRoot, `${receipt.shard_id}.receipt.json`);
  if (fs.existsSync(evidencePath)) {
    const evidence = JSON.parse(fs.readFileSync(evidencePath, "utf8"));
    if (evidence.contribution_sha256 !== receipt.contribution_sha256 || evidence.shard_plan_sha256 !== receipt.shard_plan_sha256 || evidence.canonical_tasks_sha256 !== receipt.canonical_tasks_sha256) throw new Error(`${receipt.shard_id}: evidence receipt already binds different bytes`);
  } else {
    durableWriteNew(evidencePath, `${JSON.stringify(receipt, null, 2)}\n`);
  }
  return evidencePath;
}

export function checkpointAuthorShard({ repositoryRoot, shardId, stagedRoot, checkpointRoot, evidenceRoot, now = new Date() }) {
  const loaded = loadAuthorShardPlan(repositoryRoot);
  const assignment = loaded.assignments.find((item) => item.shard_id === shardId);
  if (!assignment) throw new Error(`unknown author shard: ${shardId}`);
  const roots = authorShardPaths(repositoryRoot, checkpointRoot);
  const sourceRoot = stagedRoot || roots.stagedRoot;
  const sourcePath = path.join(sourceRoot, assignment.file);
  const receiptEvidenceRoot = evidenceRoot || roots.evidenceRoot;
  const existing = readAuthorCheckpoint({ repositoryRoot, assignment, checkpointRoot: roots.checkpointRoot });
  if (existing.exists && existing.errors.length) throw new Error(existing.errors.join("\n"));
  if (!fs.existsSync(sourcePath) || !fs.statSync(sourcePath).isFile()) {
    if (!existing.exists) throw new Error(`${assignment.file}: staged contribution is missing`);
    ensureReceiptEvidence(existing.receipt, receiptEvidenceRoot);
    return { status: "already_checkpointed", shard_id: shardId, receipt: existing.receipt, checkpoint_directory: existing.directory };
  }
  if (fs.lstatSync(sourcePath).isSymbolicLink()) throw new Error(`${assignment.file}: staged contribution must not be a symbolic link`);
  const text = fs.readFileSync(sourcePath, "utf8");
  let contribution;
  try { contribution = JSON.parse(text); } catch (error) { throw new Error(`${assignment.file}: invalid JSON (${error.message})`); }
  const checked = validateAuthorContribution(contribution, assignment, validationContext(repositoryRoot));
  if (!checked.valid) throw new Error(checked.errors.join("\n"));
  if (existing.exists) {
    if (existing.receipt.contribution_sha256 !== sha256(text)) throw new Error(`${shardId}: immutable checkpoint already binds different contribution bytes`);
    ensureReceiptEvidence(existing.receipt, receiptEvidenceRoot);
    return { status: "already_checkpointed", shard_id: shardId, receipt: existing.receipt, checkpoint_directory: existing.directory };
  }
  const receipt = {
    schema: AUTHOR_RECEIPT_SCHEMA,
    version: "3.1",
    official: false,
    shard_id: shardId,
    checkpointed_at: now.toISOString(),
    source_file: assignment.file,
    contributor_id: contribution.authorship.contributor_id,
    model: contribution.authorship.model,
    task_ids: assignment.task_ids,
    shard_plan_sha256: loaded.planSha256,
    contribution_sha256: sha256(text),
    canonical_tasks_sha256: canonicalHash(contribution.tasks),
    task_sha256: Object.fromEntries(contribution.tasks.map((task) => [task.task_id, canonicalHash(task)])),
  };
  fs.mkdirSync(roots.checkpointRoot, { recursive: true });
  const temporary = path.join(roots.checkpointRoot, `.${shardId}.tmp-${process.pid}-${crypto.randomUUID()}`);
  const destination = path.join(roots.checkpointRoot, shardId);
  try {
    fs.mkdirSync(temporary, { recursive: false });
    durableWriteNew(path.join(temporary, "contribution.json"), text);
    durableWriteNew(path.join(temporary, "receipt.json"), `${JSON.stringify(receipt, null, 2)}\n`);
    fs.renameSync(temporary, destination);
  } catch (error) {
    if (fs.existsSync(temporary)) fs.rmSync(temporary, { recursive: true, force: true });
    if (error?.code === "EEXIST" || error?.code === "ENOTEMPTY") {
      const raced = readAuthorCheckpoint({ repositoryRoot, assignment, checkpointRoot: roots.checkpointRoot });
      if (!raced.errors.length && raced.receipt?.contribution_sha256 === receipt.contribution_sha256) return { status: "already_checkpointed", shard_id: shardId, receipt: raced.receipt, checkpoint_directory: raced.directory };
    }
    throw error;
  }
  ensureReceiptEvidence(receipt, receiptEvidenceRoot);
  return { status: "checkpointed", shard_id: shardId, receipt, checkpoint_directory: destination };
}

export function validateAuthorShardSet({ repositoryRoot, selectedShardId, requireCheckpoints = false, stagedRoot, checkpointRoot }) {
  const loaded = loadAuthorShardPlan(repositoryRoot);
  const roots = authorShardPaths(repositoryRoot, checkpointRoot);
  const sourceRoot = stagedRoot || roots.stagedRoot;
  const context = validationContext(repositoryRoot);
  const selectedAssignments = selectedShardId ? loaded.assignments.filter((assignment) => assignment.shard_id === selectedShardId) : loaded.assignments;
  if (selectedShardId && selectedAssignments.length !== 1) throw new Error(`unknown author shard: ${selectedShardId}`);
  const records = [];
  const errors = [];
  for (const assignment of selectedAssignments) {
    const stagedPath = path.join(sourceRoot, assignment.file);
    const stagedExists = fs.existsSync(stagedPath) && fs.statSync(stagedPath).isFile();
    const checkpoint = readAuthorCheckpoint({ repositoryRoot, assignment, checkpointRoot: roots.checkpointRoot });
    if (checkpoint.exists && checkpoint.errors.length) {
      errors.push(...checkpoint.errors);
      records.push({ shard_id: assignment.shard_id, status: "invalid_checkpoint", task_count: 0, errors: checkpoint.errors });
      continue;
    }
    if (checkpoint.exists && stagedExists && sha256(fs.readFileSync(stagedPath)) !== checkpoint.receipt.contribution_sha256) {
      const mismatch = `${assignment.shard_id}: staged bytes differ from the immutable checkpoint`;
      errors.push(mismatch);
      records.push({ shard_id: assignment.shard_id, status: "checkpoint_mismatch", task_count: 0, errors: [mismatch] });
      continue;
    }
    const sourcePath = checkpoint.exists ? checkpoint.contributionPath : stagedExists ? stagedPath : undefined;
    if (!sourcePath) {
      const missing = `${assignment.shard_id}: contribution is missing`;
      errors.push(missing);
      records.push({ shard_id: assignment.shard_id, status: "missing", task_count: 0, errors: [missing] });
      continue;
    }
    const text = fs.readFileSync(sourcePath, "utf8");
    let contribution;
    try { contribution = JSON.parse(text); } catch (error) {
      const invalidJson = `${assignment.shard_id}: invalid JSON (${error.message})`;
      errors.push(invalidJson);
      records.push({ shard_id: assignment.shard_id, status: "invalid", task_count: 0, errors: [invalidJson] });
      continue;
    }
    const checked = validateAuthorContribution(contribution, assignment, context);
    if (!checked.valid) errors.push(...checked.errors);
    if (requireCheckpoints && !checkpoint.exists) errors.push(`${assignment.shard_id}: valid contribution has not been checkpointed`);
    records.push({
      shard_id: assignment.shard_id,
      status: !checked.valid ? "invalid" : checkpoint.exists ? "checkpointed" : "valid_uncheckpointed",
      task_count: contribution.tasks?.length || 0,
      contributor_id: contribution.authorship?.contributor_id,
      model: contribution.authorship?.model,
      contribution_sha256: sha256(text),
      canonical_tasks_sha256: canonicalHash(contribution.tasks || []),
      checkpointed: checkpoint.exists,
      source_path: sourcePath,
      contribution,
      feature_counts: checked.featureCounts,
      errors: checked.errors,
    });
  }
  if (!selectedShardId) {
    const tasks = records.flatMap((record) => record.contribution?.tasks || []);
    const categoryCounts = Object.fromEntries(Object.keys(loaded.plan.category_mix).map((category) => [category, tasks.filter((task) => task.category === category).length]));
    const trackCounts = Object.fromEntries(Object.keys(loaded.plan.track_mix).map((track) => [track, tasks.filter((task) => task.track === track).length]));
    const featureCounts = Object.fromEntries(FEATURE_KEYS.map((feature) => [feature, tasks.filter((task) => task.design_features?.[feature]).length]));
    if (tasks.length === 100) {
      if (!sameObject(categoryCounts, loaded.plan.category_mix)) errors.push("aggregate category counts do not match the shard plan");
      if (!sameObject(trackCounts, loaded.plan.track_mix)) errors.push("aggregate track counts do not match the shard plan");
      if (new Set(tasks.map((task) => task.task_id)).size !== 100 || new Set(tasks.map((task) => task.title)).size !== 100 || new Set(tasks.map((task) => task.initial_signal)).size !== 100) errors.push("aggregate task ids, titles, and initial signals must each contain 100 unique values");
    }
    return { loaded, records, errors, taskCount: tasks.length, categoryCounts, trackCounts, featureCounts, stagedRoot: sourceRoot, checkpointRoot: roots.checkpointRoot };
  }
  return { loaded, records, errors, taskCount: records.reduce((total, record) => total + record.task_count, 0), stagedRoot: sourceRoot, checkpointRoot: roots.checkpointRoot };
}
