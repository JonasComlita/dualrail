import { FormEvent, useEffect, useMemo, useState } from "react";
import "./intelligence.css";

type ApiRecord = Record<string, unknown>;

type IntelligenceFile = {
  path: string;
  purpose?: string;
  content?: string;
};

type IntelligenceTrial = {
  id: string;
  label?: string;
  status?: string;
  score?: number | null;
  passed?: boolean;
  tests_passed?: number;
  tests_total?: number;
  sealed?: boolean;
  started_at?: string;
  completed_at?: string;
};

type IntelligenceTask = {
  id: string;
  title: string;
  summary: string;
  description: string;
  category?: string;
  availability?: "ready" | "catalog";
  repository?: string;
  files: IntelligenceFile[];
  public_tests: string[];
  hidden_score_label: string;
  trials: IntelligenceTrial[];
};

type LeaderboardEntry = {
  rank?: number;
  handle: string;
  score?: number | null;
  passed?: number;
  total?: number;
  trials?: number;
  date?: string;
  status?: string;
};

type Discussion = {
  id: string;
  handle?: string;
  body: string;
  created_at?: string;
  solution_id?: string;
};

type Solution = {
  id?: string;
  task_id?: string;
  code: string;
  version?: number;
  updated_at?: string;
  status?: string;
};

type IntelligencePayload = {
  task?: Partial<IntelligenceTask> & ApiRecord;
  benchmark?: Partial<IntelligenceTask> & ApiRecord;
  data?: ApiRecord;
  public_leaderboard?: LeaderboardEntry[];
  self_reported_leaderboard?: LeaderboardEntry[];
  leaderboard?: LeaderboardEntry[];
  official_leaderboard?: LeaderboardEntry[];
  tasks?: unknown[];
  catalog?: unknown[] | ApiRecord;
  task_catalog?: unknown[];
  discussions?: Discussion[];
  solution?: Solution | null;
  latest_solution?: Solution | null;
  participant?: ApiRecord | null;
  trials?: IntelligenceTrial[];
  [key: string]: unknown;
};

type AuthMode = "login" | "register";

const TASK_ID = "TC-SWE-001";
const INTELLIGENCE_API = "/api/intelligence/v1";
const TOKEN_KEY = "treatcode.intelligence.token";
const IDENTITY_KEY = "treatcode.intelligence.identity";
const SELECTED_TASK_KEY = "treatcode.intelligence.selected-task";
const RUN_KEY_PREFIX = "treatcode.intelligence.run.";

const FALLBACK_TASK: IntelligenceTask = {
  id: TASK_ID,
  title: "Trit repository repair",
  summary: "Make a focused, test-backed change in a sealed mini-repository.",
  description:
    "Implement the requested repository change while preserving the published contract. The public tests explain the task boundary; hidden verification measures behavior and repository hygiene in isolated trials.",
  repository: "tc-swe-001-mini-repository",
  files: [
    { path: "src/compare.trit", purpose: "allowlisted implementation", content: "fn compare(a: t40, b: t40) -> t40 {\n    match a - b {\n        neg => { return 1; }\n        zero => { return 0; }\n        pos => { return -1; }\n    }\n}\n" },
    { path: "src/median.trit", purpose: "allowlisted implementation", content: "fn median(a: t40, b: t40, c: t40) -> t40 {\n    var ab: t40 = compare(a, b);\n    match ab {\n        neg => { return a; }\n        zero => { return a; }\n        pos => { return b; }\n    }\n}\n" },
  ],
  category: "Repository repair",
  availability: "ready",
  public_tests: ["public correctness fixtures", "compile and format check"],
  hidden_score_label: "Hidden verification score",
  trials: [
    { id: "trial-1", label: "Trial 1", status: "ready", sealed: true },
    { id: "trial-2", label: "Trial 2", status: "ready", sealed: true },
    { id: "trial-3", label: "Trial 3", status: "ready", sealed: true },
    { id: "trial-4", label: "Trial 4", status: "ready", sealed: true },
  ],
};

function trialSet(): IntelligenceTrial[] {
  return [1, 2, 3, 4].map((index) => ({ id: `trial-${index}`, label: `Trial ${index}`, status: "ready", sealed: true }));
}

function catalogTask(config: Pick<IntelligenceTask, "id" | "title" | "summary" | "description" | "category" | "repository" | "files" | "public_tests">): IntelligenceTask {
  return {
    ...config,
    availability: "catalog",
    hidden_score_label: "Hidden verification score",
    trials: trialSet(),
  };
}

// The service currently publishes TC-SWE-001 as the executable benchmark. The
// catalog keeps the surrounding suite discoverable without pretending that a
// task has a runner before its contract and sealed verifier are published.
const FALLBACK_SUITE: IntelligenceTask[] = [
  FALLBACK_TASK,
  catalogTask({
    id: "TC-SWE-002",
    title: "Balanced median",
    category: "Algorithm repair",
    summary: "Restore a median-of-three implementation without widening its API.",
    description: "A compact algorithm task focused on invariants, edge cases, and a narrowly allowlisted source file.",
    repository: "tc-swe-002-median-repair",
    files: [{ path: "src/median.trit", purpose: "allowlisted implementation", content: "fn median(a: t40, b: t40, c: t40) -> t40 {\n    // implement the published contract\n}\n" }],
    public_tests: ["ordered values", "duplicate values", "negative values"],
  }),
  catalogTask({
    id: "TC-SWE-003",
    title: "Compiler diagnostics",
    category: "Compiler behavior",
    summary: "Make diagnostics precise while preserving stable compiler exit behavior.",
    description: "A compiler-facing task where public fixtures expose the diagnostic shape and hidden trials probe malformed programs.",
    repository: "tc-swe-003-diagnostics",
    files: [{ path: "compiler/diagnostics.trit", purpose: "allowlisted compiler surface", content: "// implement the published diagnostic contract\n" }],
    public_tests: ["diagnostic code", "source span", "stable exit status"],
  }),
  catalogTask({
    id: "TC-SWE-004",
    title: "Pointer safety guard",
    category: "Runtime safety",
    summary: "Close a boundary bug while keeping safe programs on the fast path.",
    description: "A runtime task for reasoning about ownership boundaries, rejection behavior, and compatibility with existing callers.",
    repository: "tc-swe-004-pointer-safety",
    files: [{ path: "runtime/guard.trit", purpose: "allowlisted runtime surface", content: "// implement the published safety contract\n" }],
    public_tests: ["safe access", "null rejection", "boundary message"],
  }),
  catalogTask({
    id: "TC-SWE-005",
    title: "Test isolation cleanup",
    category: "Test infrastructure",
    summary: "Keep isolated test state deterministic across repeated runs.",
    description: "A harness task focused on reset semantics, repeatability, and preventing one trial from leaking into the next.",
    repository: "tc-swe-005-test-isolation",
    files: [{ path: "tests/isolation.trit", purpose: "allowlisted test harness", content: "// implement the published isolation contract\n" }],
    public_tests: ["fresh fixture", "repeatable run", "cleanup on failure"],
  }),
];

function asRecord(value: unknown): ApiRecord {
  return value && typeof value === "object" ? value as ApiRecord : {};
}

function asArray<T>(value: unknown): T[] {
  return Array.isArray(value) ? value as T[] : [];
}

function stringValue(value: unknown, fallback = "") {
  return typeof value === "string" && value.trim() ? value : fallback;
}

function numberValue(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

function errorReason(value: unknown, fallback: string) {
  const record = asRecord(value);
  const error = asRecord(record.error);
  return stringValue(error.reason || record.reason || record.message, fallback);
}

function unwrapPayload(value: unknown): IntelligencePayload {
  const root = asRecord(value);
  const data = asRecord(root.data);
  const nested = asRecord(data.data);
  return { ...root, ...data, ...nested } as IntelligencePayload;
}

function normalizeTrial(value: unknown, index: number): IntelligenceTrial {
  const trial = asRecord(value);
  return {
    id: stringValue(trial.id || trial.trial_id, `trial-${index + 1}`),
    label: stringValue(trial.label || trial.name, `Trial ${index + 1}`),
    status: stringValue(trial.status, "ready"),
    score: numberValue(trial.score ?? trial.hidden_score),
    passed: typeof trial.passed === "boolean" ? trial.passed : undefined,
    tests_passed: numberValue(trial.tests_passed) ?? undefined,
    tests_total: numberValue(trial.tests_total) ?? undefined,
    sealed: trial.sealed !== false,
    started_at: stringValue(trial.started_at),
    completed_at: stringValue(trial.completed_at),
  };
}

function normalizeTask(payload: IntelligencePayload, fallbackTask: IntelligenceTask = FALLBACK_TASK): IntelligenceTask {
  const data = asRecord(payload.data);
  const raw = asRecord(payload.task || payload.benchmark || data.task || data.benchmark);
  const files = asArray<unknown>(raw.files || raw.allowlisted_files || raw.allowed_files)
    .map((item) => typeof item === "string" ? { path: item } : asRecord(item))
    .map((item) => ({ path: stringValue(item.path), purpose: stringValue(item.purpose), content: stringValue(item.content || item.starter) || undefined }))
    .filter((item) => item.path);
  const publicTestsSource = raw.public_tests || raw.publicTests || raw.tests || payload.public_tests || data.public_tests;
  const publicTestsRecord = asRecord(publicTestsSource);
  const publicTests = asArray<unknown>(publicTestsSource).concat(asArray<unknown>(publicTestsRecord.cases))
    .map((item) => typeof item === "string" ? item : stringValue(asRecord(item).name || asRecord(item).id))
    .filter(Boolean);
  const trials = asArray<unknown>(raw.trials || payload.trials).map(normalizeTrial);
  const fallback = fallbackTask;
  const runnerState = stringValue(raw.availability || raw.runner_status || raw.status).toLowerCase();
  const runnerReady = raw.runner_ready === true || raw.executable === true || ["ready", "published", "executable", "runner_ready"].includes(runnerState);
  return {
    id: stringValue(raw.id || raw.task_id, fallback.id),
    title: stringValue(raw.title || raw.name, fallback.title),
    summary: stringValue(raw.summary || raw.subtitle, fallback.summary),
    description: stringValue(raw.description || raw.prompt, fallback.description),
    category: stringValue(raw.category || raw.track, fallback.category || "Repository repair"),
    availability: runnerReady ? "ready" : fallback.availability || "catalog",
    repository: stringValue(raw.repository || raw.repository_id, fallback.repository),
    files: files.length ? files : fallback.files,
    public_tests: publicTests.length ? publicTests : fallback.public_tests,
    hidden_score_label: stringValue(raw.hidden_score_label || raw.hidden_score, fallback.hidden_score_label),
    trials: trials.length === 4 ? trials : fallback.trials,
  };
}

function normalizeCatalog(payload: IntelligencePayload): IntelligenceTask[] {
  const data = asRecord(payload.data);
  const catalogObject = asRecord(payload.catalog || data.catalog);
  const source = payload.tasks || payload.task_catalog || payload.catalog || data.tasks || data.task_catalog || catalogObject.tasks || catalogObject.items;
  const candidates = asArray<unknown>(source);
  const merged = new Map(FALLBACK_SUITE.map((item) => [item.id, item]));
  for (const candidate of candidates) {
    const raw = asRecord(candidate);
    const id = stringValue(raw.id || raw.task_id);
    if (!id) continue;
    const fallback = merged.get(id) || catalogTask({
      id,
      title: stringValue(raw.title || raw.name, id),
      category: stringValue(raw.category || raw.track, "Benchmark suite"),
      summary: stringValue(raw.summary || raw.subtitle, "Published task contract"),
      description: stringValue(raw.description || raw.prompt, "Task contract available from the benchmark service."),
      repository: stringValue(raw.repository || raw.repository_id, `${id.toLowerCase()}-repository`),
      files: [],
      public_tests: [],
    });
    merged.set(id, normalizeTask({ ...payload, task: raw }, fallback));
  }
  const directTask = asRecord(payload.task || payload.benchmark || data.task || data.benchmark);
  const directId = stringValue(directTask.id || directTask.task_id);
  if (directId) {
    const fallback = merged.get(directId) || FALLBACK_TASK;
    merged.set(directId, normalizeTask(payload, fallback));
  }
  return Array.from(merged.values());
}

function payloadTaskId(payload: IntelligencePayload) {
  const data = asRecord(payload.data);
  const raw = asRecord(payload.task || payload.benchmark || data.task || data.benchmark);
  return stringValue(raw.id || raw.task_id || payload.task_id || data.task_id);
}

function parseSerializedSolution(raw: string, allowedFiles: IntelligenceFile[]): Record<string, string> {
  const allowed = new Set(allowedFiles.map((file) => file.path));
  const marker = /^\s*\/\/\s*FILE:\s*([^\r\n]+)\s*$/gm;
  const matches: Array<{ path: string; start: number; end: number }> = [];
  let match: RegExpExecArray | null;
  while ((match = marker.exec(raw))) {
    matches.push({ path: match[1].trim(), start: match.index, end: marker.lastIndex });
  }
  if (!matches.length) return {};
  const parsed: Record<string, string> = {};
  matches.forEach((entry, index) => {
    if (!allowed.has(entry.path)) return;
    const nextStart = matches[index + 1]?.start ?? raw.length;
    parsed[entry.path] = raw.slice(entry.end, nextStart).replace(/^\r?\n/, "").trimEnd();
  });
  return parsed;
}

function runDetails(value: unknown): { runId: string; trials: IntelligenceTrial[]; aggregate: ApiRecord } {
  const root = asRecord(value);
  const data = asRecord(root.data);
  const run = asRecord(root.run || data.run || (root.run_id ? root : data.run_id ? data : {}));
  const aggregate = asRecord(root.aggregate || data.aggregate || run.aggregate);
  return {
    runId: stringValue(run.run_id || run.id || root.run_id || data.run_id),
    trials: asArray<unknown>(run.trials || root.trials || data.trials).map(normalizeTrial),
    aggregate,
  };
}

function trialIsComplete(trial: IntelligenceTrial) {
  return ["passed", "complete", "completed", "submitted", "hidden_submitted", "public_passed", "failed", "rejected", "timed_out"].includes(String(trial.status || "").toLowerCase()) || trial.passed === true || trial.completed_at !== "" && Boolean(trial.completed_at);
}

function trialStatusLabel(trial?: IntelligenceTrial) {
  if (!trial) return "ready · sealed";
  const status = String(trial.status || "ready").replace(/_/g, " ");
  return `${status} · ${trial.sealed === false ? "open" : "sealed"}`;
}

function normalizeLeaderboard(value: unknown): LeaderboardEntry[] {
  return asArray<unknown>(value).map((item, index) => {
    const entry = asRecord(item);
    return {
      rank: numberValue(entry.rank) ?? index + 1,
      handle: stringValue(entry.handle || entry.owner_handle || entry.username || entry.display_name || entry.participant_id, "anonymous"),
      score: numberValue(entry.score ?? entry.hidden_score ?? entry.public_score),
      passed: numberValue(entry.passed ?? entry.tests_passed ?? entry.passed_trials) ?? undefined,
      total: numberValue(entry.total ?? entry.tests_total ?? entry.trial_count) ?? undefined,
      trials: numberValue(entry.trials ?? entry.completed_trials) ?? undefined,
      date: stringValue(entry.date || entry.created_at),
      status: stringValue(entry.status),
    };
  });
}

function normalizeSolution(value: unknown): Solution | null {
  if (!value || typeof value !== "object") return null;
  const solution = asRecord(value);
  return {
    id: stringValue(solution.id || solution.solution_id),
    task_id: stringValue(solution.task_id),
    code: stringValue(solution.code || solution.content),
    version: numberValue(solution.version) ?? undefined,
    updated_at: stringValue(solution.updated_at || solution.created_at),
    status: stringValue(solution.status),
  };
}

async function requestJson(path: string, init: RequestInit = {}, token = "") {
  const headers = new Headers(init.headers);
  headers.set("Accept", "application/json");
  if (init.body && !headers.has("Content-Type")) headers.set("Content-Type", "application/json");
  if (token) headers.set("Authorization", `Bearer ${token}`);
  if (init.method && init.method !== "GET" && init.method !== "HEAD" && !headers.has("X-Action-Nonce") && !headers.has("Idempotency-Key")) {
    headers.set("X-Action-Nonce", `intelligence-${Date.now()}-${Math.random().toString(36).slice(2, 12)}`);
  }
  const response = await fetch(path, { ...init, headers });
  const text = await response.text();
  let body: unknown = {};
  try { body = text ? JSON.parse(text) : {}; } catch { body = { message: text }; }
  if (!response.ok) {
    const error = new Error(errorReason(body, `Intelligence API returned ${response.status}`));
    (error as Error & { status?: number }).status = response.status;
    throw error;
  }
  return body;
}

async function requestWithFallback(paths: string[], init: RequestInit = {}, token = "") {
  let lastError: unknown = new Error("Intelligence API unavailable");
  for (const path of paths) {
    try {
      return await requestJson(path, init, token);
    } catch (error) {
      lastError = error;
      const status = (error as Error & { status?: number }).status;
      if (status !== 404 && status !== 405) throw error;
    }
  }
  throw lastError;
}

function extractToken(value: unknown) {
  const root = asRecord(value);
  const data = asRecord(root.data);
  const credential = asRecord(root.credential || data.credential);
  return stringValue(root.token || data.token || credential.token || root.access_token);
}

function formatDate(value?: string) {
  if (!value) return "";
  const date = new Date(value);
  return Number.isNaN(date.getTime()) ? value : date.toLocaleDateString();
}

export default function IntelligenceApp() {
  const [suiteTasks, setSuiteTasks] = useState<IntelligenceTask[]>(FALLBACK_SUITE);
  const [selectedTaskId, setSelectedTaskId] = useState(() => window.localStorage.getItem(SELECTED_TASK_KEY) || TASK_ID);
  const [task, setTask] = useState<IntelligenceTask>(FALLBACK_TASK);
  const [publicLeaderboard, setPublicLeaderboard] = useState<LeaderboardEntry[]>([]);
  const [officialLeaderboard, setOfficialLeaderboard] = useState<LeaderboardEntry[]>([]);
  const [discussions, setDiscussions] = useState<Discussion[]>([]);
  const [solution, setSolution] = useState<Solution | null>(null);
  const [code, setCode] = useState("// Start your TC-SWE-001 solution here.\n");
  const [fileContents, setFileContents] = useState<Record<string, string>>({});
  const [selectedFilePath, setSelectedFilePath] = useState("src/median.trit");
  const [selectedTrialId, setSelectedTrialId] = useState("trial-1");
  const [runId, setRunId] = useState(() => window.localStorage.getItem(`${RUN_KEY_PREFIX}${window.localStorage.getItem(SELECTED_TASK_KEY) || TASK_ID}`) || "");
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [actionMessage, setActionMessage] = useState("");
  const [actionError, setActionError] = useState("");
  const [busyAction, setBusyAction] = useState("");
  const [token, setToken] = useState(() => window.localStorage.getItem(TOKEN_KEY) || window.localStorage.getItem("treatcode.auth.token") || "");
  const [identity, setIdentity] = useState(() => window.localStorage.getItem(IDENTITY_KEY) || "");
  const [authMode, setAuthMode] = useState<AuthMode>("login");
  const [handle, setHandle] = useState("");
  const [password, setPassword] = useState("");
  const [authMessage, setAuthMessage] = useState("");
  const [authError, setAuthError] = useState("");
  const [discussionBody, setDiscussionBody] = useState("");

  const selectedTrial = useMemo(
    () => task.trials.find((trial) => trial.id === selectedTrialId) || task.trials[0],
    [selectedTrialId, task.trials],
  );

  useEffect(() => {
    let cancelled = false;
    const activeTaskId = selectedTaskId;
    const savedRunId = window.localStorage.getItem(`${RUN_KEY_PREFIX}${activeTaskId}`) || "";
    setLoading(true);
    setError("");
    Promise.all([
      requestWithFallback([`${INTELLIGENCE_API}/catalog`, `${INTELLIGENCE_API}/tasks`, "/api/intelligence/catalog", "/api/intelligence/tasks"], {}, token).catch(() => ({})),
      requestWithFallback([`${INTELLIGENCE_API}/tasks/${encodeURIComponent(activeTaskId)}`, `${INTELLIGENCE_API}/benchmark?task_id=${encodeURIComponent(activeTaskId)}`, `/api/intelligence/tasks/${encodeURIComponent(activeTaskId)}`, `/api/intelligence/benchmark?task_id=${encodeURIComponent(activeTaskId)}`, `/api/intelligence?task_id=${encodeURIComponent(activeTaskId)}`]),
      requestWithFallback([`${INTELLIGENCE_API}/leaderboard?task_id=${encodeURIComponent(activeTaskId)}`, `/api/intelligence/leaderboard?task_id=${encodeURIComponent(activeTaskId)}`], {}, token).catch(() => ({})),
      requestWithFallback([`${INTELLIGENCE_API}/discussions?task_id=${encodeURIComponent(activeTaskId)}`, `/api/intelligence/discussions?task_id=${encodeURIComponent(activeTaskId)}`, `/api/community/v1/discussions?challenge_id=${encodeURIComponent(activeTaskId)}`], {}, token).catch(() => ({})),
      token ? requestWithFallback([`${INTELLIGENCE_API}/solutions?task_id=${encodeURIComponent(activeTaskId)}`, `/api/intelligence/solutions?task_id=${encodeURIComponent(activeTaskId)}`, `/api/community/v1/solutions?challenge_id=${encodeURIComponent(activeTaskId)}`], {}, token).catch(() => ({})) : Promise.resolve({}),
      token && savedRunId ? requestJson(`${INTELLIGENCE_API}/runs/${encodeURIComponent(savedRunId)}`, {}, token).catch(() => ({})) : Promise.resolve({}),
    ]).then(([catalogValue, taskValue, leaderboardValue, discussionValue, solutionValue, runValue]) => {
      if (cancelled) return;
      const catalogPayload = unwrapPayload(catalogValue);
      const taskPayload = unwrapPayload(taskValue);
      const benchmark = payloadTaskId(taskPayload) === activeTaskId ? taskPayload : {} as IntelligencePayload;
      const leaderboard = unwrapPayload(leaderboardValue);
      const discussionPayload = unwrapPayload(discussionValue);
      const solutionPayload = unwrapPayload(solutionValue);
      const catalogFromService = normalizeCatalog(catalogPayload);
      const taskFromService = payloadTaskId(taskPayload) === activeTaskId ? normalizeTask(taskPayload, catalogFromService.find((item) => item.id === activeTaskId) || FALLBACK_TASK) : null;
      const catalog = catalogFromService.map((item) => item.id === activeTaskId && taskFromService ? taskFromService : item);
      setSuiteTasks(catalog);
      const normalizedTask = catalog.find((item) => item.id === activeTaskId) || FALLBACK_TASK;
      setTask(normalizedTask);
      const starterFiles = Object.fromEntries(normalizedTask.files.map((file) => [file.path, file.content || ""]));
      const initialFile = normalizedTask.files.some((file) => file.path === selectedFilePath) ? selectedFilePath : normalizedTask.files[0]?.path || selectedFilePath;
      setSelectedFilePath(initialFile);
      const publicRows = normalizeLeaderboard(leaderboard.self_reported_leaderboard || leaderboard.self_reported || benchmark.self_reported_leaderboard || benchmark.public_leaderboard || benchmark.publicLeaderboard);
      const officialRows = normalizeLeaderboard(benchmark.official_leaderboard || benchmark.officialLeaderboard || leaderboard.official_leaderboard || leaderboard.official || leaderboard.leaderboard || leaderboard);
      setPublicLeaderboard(publicRows);
      setOfficialLeaderboard(officialRows);
      setDiscussions(asArray<unknown>(benchmark.discussions || discussionPayload.discussions).map((item, index) => {
        const value = asRecord(item);
        return { id: stringValue(value.id, `discussion-${index}`), handle: stringValue(value.handle || value.username), body: stringValue(value.body || value.content), created_at: stringValue(value.created_at), solution_id: stringValue(value.solution_id) };
      }).filter((item) => item.body));
      const loadedSolution = normalizeSolution(benchmark.solution || benchmark.latest_solution || solutionPayload.solution || solutionPayload.latest_solution);
      const parsedSolution = loadedSolution ? parseSerializedSolution(loadedSolution.code, normalizedTask.files) : {};
      const nextFiles = { ...starterFiles, ...parsedSolution };
      setFileContents(nextFiles);
      if (loadedSolution) {
        setSolution(loadedSolution);
        setCode(parsedSolution[initialFile] || (!Object.keys(parsedSolution).length ? loadedSolution.code : nextFiles[initialFile] || ""));
      } else {
        setSolution(null);
        setCode(nextFiles[initialFile] || "");
      }
      const participant = asRecord(benchmark.participant || benchmark.session);
      const participantLabel = stringValue(participant.handle || participant.display_name || participant.username);
      if (participantLabel) {
        setIdentity(participantLabel);
        window.localStorage.setItem(IDENTITY_KEY, participantLabel);
      }
      const restoredRun = runDetails(runValue);
      if (restoredRun.runId) {
        setRunId(restoredRun.runId);
        window.localStorage.setItem(`${RUN_KEY_PREFIX}${activeTaskId}`, restoredRun.runId);
      } else if (savedRunId && token) {
        window.localStorage.removeItem(`${RUN_KEY_PREFIX}${activeTaskId}`);
        setRunId("");
      }
      if (restoredRun.trials.length === 4) {
        const restoredTask = { ...normalizedTask, trials: restoredRun.trials };
        setTask(restoredTask);
        setSuiteTasks((current) => current.map((item) => item.id === activeTaskId ? restoredTask : item));
      }
    }).catch((reason: unknown) => {
      if (!cancelled) setError(reason instanceof Error ? reason.message || "Unable to load the intelligence benchmark." : "Unable to load the intelligence benchmark.");
    }).finally(() => {
      if (!cancelled) setLoading(false);
    });
    return () => { cancelled = true; };
  }, [token, selectedTaskId]);

  function selectTask(nextTaskId: string) {
    const nextTask = suiteTasks.find((item) => item.id === nextTaskId);
    if (!nextTask || nextTask.id === selectedTaskId) return;
    window.localStorage.setItem(SELECTED_TASK_KEY, nextTask.id);
    setSelectedTaskId(nextTask.id);
    setTask(nextTask);
    setSelectedTrialId(nextTask.trials[0]?.id || "trial-1");
    setSelectedFilePath(nextTask.files[0]?.path || "");
    setFileContents(Object.fromEntries(nextTask.files.map((file) => [file.path, file.content || ""])));
    setCode(nextTask.files[0]?.content || "");
    setSolution(null);
    setDiscussions([]);
    setPublicLeaderboard([]);
    setOfficialLeaderboard([]);
    setRunId(window.localStorage.getItem(`${RUN_KEY_PREFIX}${nextTask.id}`) || "");
    setActionMessage("");
    setActionError("");
  }

  async function authenticate(event: FormEvent) {
    event.preventDefault();
    setAuthMessage("");
    setAuthError("");
    if (!handle.trim() || !password) {
      setAuthError("Handle and password are required.");
      return;
    }
    setBusyAction("auth");
    try {
      const paths = authMode === "register"
        ? [`${INTELLIGENCE_API}/accounts/register`, "/api/intelligence/accounts/register", "/api/auth/v1/participants/register", "/api/auth/v1/register"]
        : [`${INTELLIGENCE_API}/accounts/login`, "/api/intelligence/accounts/login", "/api/auth/v1/participants/login", "/api/auth/v1/login"];
      const value = await requestWithFallback(paths, { method: "POST", body: JSON.stringify({ handle: handle.trim(), password }) });
      const nextToken = extractToken(value);
      if (!nextToken) throw new Error("Account response did not include a session token.");
      window.localStorage.setItem(TOKEN_KEY, nextToken);
      window.localStorage.setItem("treatcode.auth.token", nextToken);
      setToken(nextToken);
      const payload = unwrapPayload(value);
      const account = asRecord(payload.participant || payload.identity || payload.account || payload.user);
      const participantLabel = stringValue(account.handle || account.display_name || account.username, handle.trim());
      setIdentity(participantLabel);
      window.localStorage.setItem(IDENTITY_KEY, participantLabel);
      setPassword("");
      setAuthMessage(authMode === "register" ? "Account created. Your participant session is ready." : "Signed in. Participant actions are now enabled.");
    } catch (reason) {
      setAuthError(reason instanceof Error ? reason.message || "Unable to authenticate." : "Unable to authenticate.");
    } finally {
      setBusyAction("");
    }
  }

  function logout() {
    window.localStorage.removeItem(TOKEN_KEY);
    window.localStorage.removeItem("treatcode.auth.token");
    window.localStorage.removeItem(IDENTITY_KEY);
    setToken("");
    setIdentity("");
    setAuthMessage("Signed out. Your public results remain visible; editing requires a participant session.");
  }

  function serializedSolution() {
    const entries = Object.entries(fileContents).filter(([, value]) => value.trim());
    if (!entries.length) return code;
    return entries.map(([file, value]) => `// FILE: ${file}\n${value.trimEnd()}`).join("\n\n");
  }

  async function saveSolution() {
    setActionError("");
    setActionMessage("");
    if (!token) {
      setActionError("Sign in or create an account before saving a solution.");
      return;
    }
    if (!code.trim()) {
      setActionError("Add a solution before saving.");
      return;
    }
    setBusyAction("save");
    try {
      const value = await requestWithFallback([`${INTELLIGENCE_API}/solutions`, "/api/intelligence/solutions", "/api/community/v1/solutions"], { method: "POST", body: JSON.stringify({ task_id: task.id, challenge_id: task.id, title: `${task.id} solution`, code: serializedSolution(), language: "trit" }) }, token);
      const payload = unwrapPayload(value);
      const saved = normalizeSolution(payload.solution || payload.data || value);
      setSolution(saved || { task_id: task.id, code, status: "saved" });
      setActionMessage("Solution saved as a private, versioned draft.");
    } catch (reason) {
      setActionError(reason instanceof Error ? reason.message || "Unable to save the solution." : "Unable to save the solution.");
    } finally {
      setBusyAction("");
    }
  }

  async function startTrial() {
    setActionError("");
    setActionMessage("");
    if (!token) {
      setActionError("Sign in or create an account before starting a sealed trial.");
      return;
    }
    if (task.availability !== "ready") {
      setActionError(`${task.id} is cataloged, but its sealed runner is not published yet. Select a ready task to submit evidence.`);
      return;
    }
    if (!selectedTrial) {
      setActionError("Select one of the four sealed trials before submitting.");
      return;
    }
    setBusyAction("trial");
    try {
      const value = await requestWithFallback([`${INTELLIGENCE_API}/trials`, "/api/intelligence/trials"], { method: "POST", body: JSON.stringify({ task_id: task.id, challenge_id: task.id, benchmark_id: task.id, run_id: runId || undefined, trial_id: selectedTrial.id, solution_id: solution?.id, code, files: fileContents }) }, token);
      const payload = unwrapPayload(value);
      const returnedTrial = normalizeTrial(payload.trial || payload.data || value, task.trials.findIndex((trial) => trial.id === selectedTrial.id));
      const returnedRun = runDetails(value);
      const selectedIndex = Math.max(0, task.trials.findIndex((trial) => trial.id === selectedTrial.id));
      const aggregate = Object.keys(returnedRun.aggregate).length ? returnedRun.aggregate : asRecord(payload.aggregate || asRecord(payload.hidden).aggregate || asRecord(payload.run).aggregate);
      const aggregateScore = numberValue(aggregate.score);
      const aggregateComplete = numberValue(aggregate.completed_trials) === 4 || numberValue(aggregate.remaining_trials) === 0 || aggregate.status === "complete";
      const nextTrials = returnedRun.trials.length === 4
        ? returnedRun.trials
        : task.trials.map((trial, index) => index === selectedIndex ? { ...trial, ...returnedTrial, id: trial.id } : trial);
      const completedTrials = aggregateComplete
        ? nextTrials.map((trial) => ({ ...trial, status: aggregateScore === 100 ? "passed" : "complete", ...(aggregateScore === null ? {} : { score: aggregateScore }) }))
        : nextTrials;
      setTask((current) => ({ ...current, trials: completedTrials }));
      setSuiteTasks((current) => current.map((item) => item.id === task.id ? { ...item, trials: completedTrials } : item));
      const nextRunId = returnedRun.runId || runId;
      if (nextRunId) {
        setRunId(nextRunId);
        window.localStorage.setItem(`${RUN_KEY_PREFIX}${task.id}`, nextRunId);
      }
      if (aggregateComplete) {
        const leaderboardValue = await requestWithFallback([`${INTELLIGENCE_API}/leaderboard?task_id=${encodeURIComponent(task.id)}`, `/api/intelligence/leaderboard?task_id=${encodeURIComponent(task.id)}`], {}, token).catch(() => ({}));
        const leaderboardPayload = unwrapPayload(leaderboardValue);
        setPublicLeaderboard(normalizeLeaderboard(leaderboardPayload.self_reported_leaderboard || leaderboardPayload.self_reported || leaderboardPayload.public_leaderboard));
        setOfficialLeaderboard(normalizeLeaderboard(leaderboardPayload.official_leaderboard || leaderboardPayload.official || leaderboardPayload.leaderboard));
      }
      const publicationMessage = aggregateComplete
        ? aggregate.published === true
          ? " Official publication is attested."
          : " The sealed score is ready; official publication requires privileged attestation."
        : " Hidden score remains sealed until all four trials complete.";
      setActionMessage(`${task.id} trial submitted to the isolated verifier.${publicationMessage}`);
    } catch (reason) {
      setActionError(reason instanceof Error ? reason.message || "Unable to start the sealed trial." : "Unable to start the sealed trial.");
    } finally {
      setBusyAction("");
    }
  }

  async function publishDiscussion(event: FormEvent) {
    event.preventDefault();
    setActionError("");
    setActionMessage("");
    if (!token) {
      setActionError("Sign in or create an account before publishing a discussion.");
      return;
    }
    if (!discussionBody.trim()) {
      setActionError("Write an explanation or pseudocode before publishing.");
      return;
    }
    setBusyAction("discussion");
    try {
      const value = await requestWithFallback([`${INTELLIGENCE_API}/discussions`, "/api/intelligence/discussions", "/api/community/v1/discussions"], { method: "POST", body: JSON.stringify({ task_id: task.id, challenge_id: task.id, solution_id: solution?.id, body: discussionBody.trim() }) }, token);
      const payload = unwrapPayload(value);
      const raw = asRecord(payload.discussion || payload.data || value);
      const published: Discussion = { id: stringValue(raw.id, `discussion-${Date.now()}`), handle: identity || "participant", body: stringValue(raw.body || raw.content, discussionBody.trim()), created_at: stringValue(raw.created_at, new Date().toISOString()), solution_id: stringValue(raw.solution_id || solution?.id) };
      setDiscussions((current) => [published, ...current]);
      setDiscussionBody("");
      setActionMessage("Discussion published and linked to this task.");
    } catch (reason) {
      setActionError(reason instanceof Error ? reason.message || "Unable to publish the discussion." : "Unable to publish the discussion.");
    } finally {
      setBusyAction("");
    }
  }

  return (
    <div className="intelligence-app" data-testid="intelligence-app">
      <header className="intelligence-header">
        <a className="intelligence-brand" href="/">Treat<span>Code</span></a>
        <nav aria-label="Intelligence navigation">
          <a href="/stack">Stack Explorer</a>
          <a href="/learn">Learn</a>
          <a href="/practice">Practice</a>
          <a href="/arena">Implementation Arena</a>
        </nav>
        <div className="intelligence-account-summary">
          {identity ? <><span data-testid="intelligence-identity">{identity}</span><button type="button" onClick={logout}>Sign out</button></> : <a href="#account">Sign in / sign up</a>}
        </div>
      </header>

      <main className="intelligence-main">
        <section className="intelligence-hero">
          <div>
            <span className="intelligence-eyebrow">P14 · intelligence benchmark</span>
            <h1>Measure the work, keep the evidence sealed.</h1>
            <p>The TreatCode suite is DeepSWE-inspired: each repository task publishes a focused contract, then scores behavior across four clean, one-shot trials. Public tests stay useful while hidden verification remains sealed.</p>
          </div>
          <aside className="intelligence-contract" data-testid="benchmark-contract">
            <span className="intelligence-eyebrow">Selected benchmark contract</span>
            <strong>{task.id}</strong>
            <span>{task.trials.length} sealed trials · public tests visible · hidden score withheld</span>
          </aside>
        </section>

        {loading ? <div className="intelligence-status" role="status" aria-live="polite" data-testid="intelligence-loading">Loading benchmark evidence…</div> : null}
        {error ? <div className="intelligence-error" role="alert" data-testid="intelligence-error">Unable to load the intelligence benchmark: {error}</div> : null}

        <section className="intelligence-panel intelligence-suite-panel" data-testid="suite-catalog" aria-labelledby="intelligence-suite-title">
          <div className="intelligence-section-heading"><div><span className="intelligence-eyebrow">Benchmark suite</span><h2 id="intelligence-suite-title">Choose a task contract</h2></div><span className="intelligence-badge">{suiteTasks.length} task tracks</span></div>
          <p className="intelligence-muted">Select a task to inspect its own allowlisted files, public contract, trial receipt, and leaderboards. TC-SWE-001 is executable today; catalog cards remain visibly scoped until their sealed runner is published.</p>
          <div className="intelligence-suite-grid" role="list" aria-label="Intelligence benchmark task catalog">
            {suiteTasks.map((candidate) => <button type="button" role="listitem" key={candidate.id} className={`intelligence-suite-card ${candidate.id === selectedTaskId ? "selected" : ""}`} aria-pressed={candidate.id === selectedTaskId} data-testid={`suite-task-card-${candidate.id}`} data-task-id={candidate.id} onClick={() => selectTask(candidate.id)}>
              <span className="intelligence-suite-card-top"><span className="intelligence-eyebrow">{candidate.category || "Benchmark suite"}</span><span className={`intelligence-availability ${candidate.availability === "ready" ? "ready" : "catalog"}`}>{candidate.availability === "ready" ? "ready" : "catalog"}</span></span>
              <strong>{candidate.id}</strong><span className="intelligence-suite-card-title">{candidate.title}</span><small>{candidate.summary}</small>
            </button>)}
          </div>
          <p className="intelligence-suite-selection" role="status" aria-live="polite" data-testid="selected-task-status">Selected <code>{task.id}</code> · {task.category || "Benchmark suite"}</p>
        </section>

        <section className="intelligence-task-grid">
          <article className="intelligence-panel intelligence-task-panel" data-testid="intelligence-task">
            <div className="intelligence-section-heading"><div><span className="intelligence-eyebrow">Selected task · {task.category || "Benchmark"}</span><h2>{task.id} · {task.title}</h2></div><span className={`intelligence-badge ${task.availability === "ready" ? "saved" : "pending"}`}>{task.availability === "ready" ? "runner ready" : "catalog only"}</span></div>
            <p className="intelligence-lede">{task.summary}</p>
            <p className="intelligence-muted">{task.description}</p>
            <div className="intelligence-task-facts"><span><b>Category</b>{task.category || "Benchmark"}</span><span><b>Repository</b><code>{task.repository || "mini-repository"}</code></span><span><b>Public tests</b>{task.public_tests.length} checks exposed</span><span><b>Trials</b>{task.trials.length} independent runs</span></div>
            <div className="intelligence-subsection"><h3>Allowlisted files</h3><ul className="intelligence-file-list">{task.files.map((file) => <li key={file.path}><code>{file.path}</code><span>{file.purpose || "task file"}</span></li>)}</ul></div>
            <div className="intelligence-subsection"><h3>Public contract</h3><ul className="intelligence-public-tests">{task.public_tests.map((test) => <li key={test}><span aria-hidden="true">✓</span>{test}</li>)}</ul></div>
          </article>

          <aside className="intelligence-panel intelligence-trials-panel" data-testid="sealed-trials">
            <div className="intelligence-section-heading"><div><span className="intelligence-eyebrow">Trial workspace · {task.id}</span><h2>Four sealed runs</h2></div><span className="intelligence-badge">{task.trials.filter(trialIsComplete).length}/{task.trials.length} logged</span></div>
            <p className="intelligence-muted">Each trial receives an isolated workspace. A timeout, incomplete run, or tamper signal cannot enter the official leaderboard.</p>
            <div className="intelligence-trial-list">{task.trials.map((trial, index) => <button type="button" key={trial.id} className={`intelligence-trial ${selectedTrial?.id === trial.id ? "selected" : ""}`} onClick={() => setSelectedTrialId(trial.id)} data-testid={`trial-${index + 1}`} aria-label={`${trial.label || `Trial ${index + 1}`}: ${trialStatusLabel(trial)}`}><span><strong>{trial.label || `Trial ${index + 1}`}</strong><small>{trialStatusLabel(trial)}</small></span><b>{trial.score === null || trial.score === undefined ? "—" : trial.score}</b></button>)}</div>
            <div className="intelligence-trial-detail"><span className="intelligence-eyebrow">Selected trial</span><strong>{selectedTrial?.label || "Trial 1"}</strong><p>{task.availability !== "ready" ? "This task is cataloged; sealed submissions open when its runner is published." : selectedTrial && trialIsComplete(selectedTrial) ? "This sealed receipt is already recorded." : "Ready for an authenticated one-shot submission."}</p><button className="intelligence-primary" type="button" onClick={() => void startTrial()} disabled={busyAction === "trial" || task.availability !== "ready"} data-testid="submit-sealed-trial">{busyAction === "trial" ? "Submitting…" : task.availability !== "ready" ? "Runner pending" : "Submit sealed trial"}</button></div>
          </aside>
        </section>

        <section className="intelligence-panel intelligence-solution-panel" data-testid="solution-workspace">
          <div className="intelligence-section-heading"><div><span className="intelligence-eyebrow">Participant workspace · {task.id}</span><h2>Save a versioned solution</h2></div><span className={`intelligence-badge ${solution ? "saved" : "pending"}`}>{solution ? `saved${solution.version ? ` · v${solution.version}` : ""}` : "unsaved"}</span></div>
          <div className="intelligence-file-tabs" role="tablist" aria-label={`${task.id} allowlisted files`}>{task.files.map((file) => <button key={file.path} type="button" role="tab" aria-selected={selectedFilePath === file.path} aria-controls="intelligence-solution-code" className={selectedFilePath === file.path ? "selected" : ""} onClick={() => { setSelectedFilePath(file.path); setCode(fileContents[file.path] ?? file.content ?? ""); }}>{file.path}</button>)}</div>
          <label className="intelligence-field-label" htmlFor="intelligence-solution-code">{selectedFilePath || task.id} source</label>
          <textarea id="intelligence-solution-code" data-testid="solution-editor" aria-label={`${selectedFilePath || "TC-SWE-001"} source`} value={code} onChange={(event) => { const next = event.target.value; setCode(next); setFileContents((current) => ({ ...current, [selectedFilePath]: next })); }} spellCheck={false} />
          <div className="intelligence-action-row"><button className="intelligence-primary" type="button" data-testid="save-solution" onClick={() => void saveSolution()} disabled={busyAction === "save"}>{busyAction === "save" ? "Saving…" : "Save solution"}</button>{solution?.updated_at ? <span className="intelligence-muted">Last saved {formatDate(solution.updated_at)}</span> : null}</div>
          <p className="intelligence-help">The participant handle comes from the authenticated session. It is never entered beside the editor or trusted from the submission payload.</p>
        </section>

        <div className="intelligence-feedback" role="status" aria-live="polite" data-testid="intelligence-action-status">{actionMessage || actionError ? <span className={actionError ? "is-error" : ""}>{actionError || actionMessage}</span> : null}</div>

        <section className="intelligence-community-grid">
          <article className="intelligence-panel" data-testid="discussion-panel">
            <div className="intelligence-section-heading"><div><span className="intelligence-eyebrow">Community artifact</span><h2>Solution discussion</h2></div><span className="intelligence-badge">plain text</span></div>
            <p className="intelligence-muted">Publish an explanation, tradeoff, or pseudocode linked to your saved solution. Discussions are attributable to the session handle.</p>
            <form onSubmit={publishDiscussion}>
              <label className="intelligence-field-label" htmlFor="intelligence-discussion-body">Explanation or pseudocode</label>
              <textarea id="intelligence-discussion-body" data-testid="discussion-editor" aria-label="Solution explanation or pseudocode" value={discussionBody} onChange={(event) => setDiscussionBody(event.target.value)} placeholder="Explain the invariant, approach, or pseudocode…" />
              <button className="intelligence-secondary" type="submit" data-testid="publish-discussion" disabled={busyAction === "discussion"}>{busyAction === "discussion" ? "Publishing…" : "Publish discussion"}</button>
            </form>
            <div className="intelligence-discussion-list" aria-label="Published solution discussions">{discussions.length ? discussions.map((discussion) => <article className="intelligence-discussion" key={discussion.id}><div><strong>{discussion.handle || "participant"}</strong><small>{formatDate(discussion.created_at)}</small></div><p>{discussion.body}</p></article>) : <p className="intelligence-empty" data-testid="discussion-empty">No discussions yet. Be the first to explain a solution.</p>}</div>
          </article>

          <aside className="intelligence-panel" data-testid="account">
            <div className="intelligence-section-heading"><div><span className="intelligence-eyebrow">Participant account</span><h2>{token ? "Session active" : "Join the benchmark"}</h2></div><span className="intelligence-badge">least privilege</span></div>
            {token ? <div className="intelligence-account-active"><strong>{identity || "Authenticated participant"}</strong><p>Your saved solutions and trial submissions are scoped to this account. Public benchmark evidence remains readable without a session.</p><button className="intelligence-secondary" type="button" onClick={logout}>Sign out</button></div> : <form className="intelligence-auth-form" onSubmit={authenticate}><div className="intelligence-auth-tabs" role="tablist" aria-label="Participant account actions"><button type="button" role="tab" aria-selected={authMode === "login"} className={authMode === "login" ? "active" : ""} onClick={() => setAuthMode("login")}>Log in</button><button type="button" role="tab" aria-selected={authMode === "register"} className={authMode === "register" ? "active" : ""} onClick={() => setAuthMode("register")}>Sign up</button></div><label className="intelligence-field-label" htmlFor="intelligence-handle">Handle<input id="intelligence-handle" data-testid="account-handle" value={handle} onChange={(event) => setHandle(event.target.value)} autoComplete="username" /></label><label className="intelligence-field-label" htmlFor="intelligence-password">Password<input id="intelligence-password" data-testid="account-password" type="password" value={password} onChange={(event) => setPassword(event.target.value)} autoComplete={authMode === "register" ? "new-password" : "current-password"} /></label><button className="intelligence-primary" data-testid={authMode === "register" ? "signup" : "login"} type="submit" disabled={busyAction === "auth"}>{busyAction === "auth" ? "Working…" : authMode === "register" ? "Create participant account" : "Log in"}</button>{authError ? <p className="intelligence-form-error" role="alert">{authError}</p> : null}{authMessage ? <p className="intelligence-form-message" role="status">{authMessage}</p> : null}</form>}
          </aside>
        </section>

        <section className="intelligence-leaderboards" aria-label="Intelligence leaderboards">
          <article className="intelligence-panel" data-testid="public-leaderboard" data-task-id={task.id}><div className="intelligence-section-heading"><div><span className="intelligence-eyebrow">{task.id} · public tests</span><h2>Self-reported leaderboard</h2></div><span className="intelligence-badge">visible score</span></div><p className="intelligence-muted">A task-scoped view of public-test progress. It is not the official hidden score.</p><LeaderboardTable entries={publicLeaderboard} empty={`No public-test submissions for ${task.id} yet.`} kind="public" /></article>
          <article className="intelligence-panel" data-testid="official-leaderboard" data-task-id={task.id}><div className="intelligence-section-heading"><div><span className="intelligence-eyebrow">{task.id} · hidden verification</span><h2>Official leaderboard</h2></div><span className="intelligence-badge">sealed evidence</span></div><p className="intelligence-muted">Only complete, untampered, attested four-trial scores for this task appear here. Hidden details remain server-only.</p><LeaderboardTable entries={officialLeaderboard} empty={`No attested four-trial scores for ${task.id} yet.`} kind="official" /></article>
        </section>
      </main>
      <footer className="intelligence-footer">P14 Intelligence Benchmark · <a href="/api/intelligence/v1/benchmark">Benchmark API</a> · <a href="/arena">P10 Implementation Arena</a> · <a href="/">Return to TreatCode</a></footer>
    </div>
  );
}

function LeaderboardTable({ entries, empty, kind }: { entries: LeaderboardEntry[]; empty: string; kind: "public" | "official" }) {
  return entries.length ? <div className="intelligence-table-wrap"><table className="intelligence-table"><thead><tr><th>Rank</th><th>Handle</th><th>{kind === "official" ? "Score" : "Tests"}</th><th>Status</th></tr></thead><tbody>{entries.map((entry, index) => <tr key={`${entry.handle}-${index}`}><td>#{entry.rank || index + 1}</td><td><code>{entry.handle}</code></td><td>{kind === "official" ? (entry.score === null || entry.score === undefined ? "—" : entry.score) : `${entry.passed ?? 0}/${entry.total ?? "?"}`}</td><td>{entry.status || (kind === "official" ? `${entry.trials ?? 0}/4 trials` : "public")}</td></tr>)}</tbody></table></div> : <p className="intelligence-empty" data-testid={`${kind}-leaderboard-empty`}>{empty}</p>;
}
