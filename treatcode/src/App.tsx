import { useState, useMemo, useEffect } from "react";
import {
  LEARNING_CATALOG,
  LEARNING_PAGES,
  parseMarkdown,
  Block,
  type LearningInteractive,
  type LearningPage,
} from "./learningContent";
import challengeData from "./generated/challenges.client.json";

type ChallengeFacetKey =
  | "domain"
  | "technique"
  | "data_model"
  | "stack_layer"
  | "target"
  | "optimization_objective";

type ChallengeFacets = Record<ChallengeFacetKey, string[]>;

interface Problem {
  id: string;
  title: string;
  lifecycle: "published" | "draft";
  difficulty: "easy" | "medium" | "hard";
  category: string;
  tags: string[];
  facets: ChallengeFacets;
  solved: boolean;
  submissions: number;
  acceptance: number;
  points: number;
  description?: string;
  signature?: string;
  template?: string;
  execution: {
    mode: "verified" | "compile-only";
    runner: string;
    limits: {
      time_ms: number;
      memory_kib: number;
      max_cycles: number;
      max_output_bytes: number;
    };
  };
}

interface ChallengeData {
  schema: string;
  manifest_schema: string;
  manifest_version: number;
  source_of_truth: string;
  facet_dimensions: Record<ChallengeFacetKey, string[]>;
  challenges: Problem[];
}

const CHALLENGE_DATA = challengeData as unknown as ChallengeData;
const CHALLENGES: Problem[] = CHALLENGE_DATA.challenges;
const FACET_KEYS: ChallengeFacetKey[] = ["domain", "technique", "data_model", "stack_layer", "target", "optimization_objective"];

const CATEGORY_LABELS: Record<string, string> = {
  "three-valued": "Three-Valued Logic",
  "pointer-safety": "Pointer Safety",
  arithmetic: "Balanced Arithmetic",
  concurrent: "Concurrent Primitives",
  dsa: "DSA Classics",
  crypto: "Cryptographic",
  "isa-optimization": "ISA Optimization",
  "width-polymorphism": "Width Polymorphism",
  trinfuck: "Trinfuck",
};

const CATEGORIES = [
  { id: "all", label: "All" },
  ...Array.from(new Set(CHALLENGES.map((challenge) => challenge.category))).map((id) => ({
    id,
    label: CATEGORY_LABELS[id] || id,
  })),
];

interface LeaderboardData {
  id: number;
  problemId: string;
  name: string;
  engine: "native" | "bootstrap";
  cycles: number;
  date: string;
}

interface AuthUiState {
  identityLabel: string | null;
  actions: string[];
  error: string;
  onLogin: (accessKey: string) => void;
  onLogout: () => void;
}

const PROPOSALS = [
  { id: "UP-001", title: "ulib/ternary_map.trit", author: "trit_wizard", status: "review", votes: 14, desc: "Bidirectional probing hash map with TCMP-driven collision resolution." },
  { id: "UP-002", title: "ulib/trie_compressed.trit", author: "balanced_0xff", status: "draft", votes: 7, desc: "Compressed trie optimized for 9-trit sub-word key fragments." },
  { id: "UP-003", title: "ulib/fixed_point.trit", author: "sign_splitter", status: "review", votes: 5, desc: "Fixed-point arithmetic exploiting balanced ternary sign symmetry." },
  { id: "UP-004", title: "ulib/bloom_filter.trit", author: "ternary_ghost", status: "draft", votes: 3, desc: "Ternary bloom filter with three-valued membership states." },
];

const DEFAULT_STARTER_CODE = `// Solution
import ulib;

fn main() -> t40 {
    // your solution here
    return 0;
}`;

function toBalancedTernary(val: number): string {
  if (val === 0) return "0";
  let trits = "";
  let n = val;
  while (n !== 0) {
    let rem = n % 3;
    if (rem === 2) {
      trits = "-" + trits;
      n = Math.floor(n / 3) + 1;
    } else if (rem === -2) {
      trits = "+" + trits;
      n = Math.ceil(n / 3) - 1;
    } else if (rem === 1) {
      trits = "+" + trits;
      n = Math.floor(n / 3);
    } else if (rem === -1) {
      trits = "-" + trits;
      n = Math.ceil(n / 3);
    } else {
      trits = "0" + trits;
      n = n / 3;
    }
  }
  return trits;
}

const diff = (d: string) => {
  const s = {
    easy: { bg: "#EAF3DE", c: "#3B6D11" },
    medium: { bg: "#FAEEDA", c: "#854F0B" },
    hard: { bg: "#FCEBEB", c: "#A32D2D" },
  }[d as "easy" | "medium" | "hard"] || { bg: "#F1EFE8", c: "#5F5E5A" };
  return (
    <span
      style={{
        background: s.bg,
        color: s.c,
        fontSize: 11,
        padding: "2px 7px",
        borderRadius: 4,
        fontFamily: "var(--font-mono)",
        fontWeight: 500,
      }}
    >
      {d}
    </span>
  );
};

const cat = (c: string) => {
  const map: Record<string, [string, string]> = {
    "three-valued": ["#E1F5EE", "#0F6E56"],
    "pointer-safety": ["#EEEDFE", "#534AB7"],
    "arithmetic": ["#E6F1FB", "#185FA5"],
    "concurrent": ["#FAEEDA", "#854F0B"],
    "dsa": ["#F1EFE8", "#5F5E5A"],
    "crypto": ["#FAECE7", "#993C1D"],
    "isa-optimization": ["#E8F6DD", "#4D7C2C"],
    "width-polymorphism": ["#FDF1DC", "#A06B18"],
    "trinfuck": ["#FBEBEB", "#A52F2F"],
  };
  const [bg, color] = map[c] || ["#F1EFE8", "#5F5E5A"];
  return (
    <span
      style={{
        background: bg,
        color,
        fontSize: 11,
        padding: "2px 7px",
        borderRadius: 4,
        fontFamily: "var(--font-mono)",
      }}
    >
      {c}
    </span>
  );
};

function mono(text: string) {
  return (
    <code
      style={{
        fontFamily: "var(--font-mono)",
        background: "var(--color-background-tertiary)",
        padding: "1px 5px",
        borderRadius: 3,
        fontSize: "0.9em",
      }}
    >
      {text}
    </code>
  );
}

function Nav({ view, setView, auth }: { view: string; setView: (v: string) => void; auth?: AuthUiState }) {
  const [accessKey, setAccessKey] = useState("");
  return (
    <div
      style={{
        borderBottom: "0.5px solid var(--color-border-tertiary)",
        display: "flex",
        alignItems: "center",
        gap: 24,
        height: 52,
      }}
    >
      <span
        onClick={() => setView("home")}
        style={{
          fontFamily: "var(--font-mono)",
          fontSize: 15,
          fontWeight: 500,
          cursor: "pointer",
          color: "var(--color-text-primary)",
          letterSpacing: "0.06em",
          userSelect: "none",
        }}
      >
        TREATCODE
      </span>
      <div style={{ display: "flex", gap: 2 }}>
        {[
          ["problems", "Problems"],
          ["leaderboard", "Leaderboard"],
          ["contribute", "Contribute"],
          ["guide", "Guide"],
        ].map(([v, label]) => (
          <button
            key={v}
            onClick={() => setView(v)}
            style={{
              fontSize: 13,
              padding: "5px 12px",
              borderRadius: 5,
              border: "none",
              background:
                view === v
                  ? "var(--color-background-tertiary)"
                  : "transparent",
              cursor: "pointer",
              color:
                view === v
                  ? "var(--color-text-primary)"
                  : "var(--color-text-secondary)",
              fontWeight: view === v ? 500 : 400,
            }}
          >
            {label}
          </button>
        ))}
      </div>
      <div style={{ marginLeft: "auto", display: "flex", gap: 8 }}>
        {auth?.identityLabel ? (
          <>
            <span style={{ fontSize: 11, color: "var(--color-text-secondary)", alignSelf: "center" }}>
              {auth.identityLabel} · {auth.actions.length} grants
            </span>
            <button
              type="button"
              onClick={auth.onLogout}
              style={{ fontSize: 12, padding: "5px 14px", borderRadius: 5, border: "0.5px solid var(--color-border-tertiary)", background: "transparent", cursor: "pointer", color: "var(--color-text-secondary)" }}
            >
              log out
            </button>
          </>
        ) : auth ? (
          <>
            <label style={{ display: "flex", alignItems: "center", gap: 4, fontSize: 11, color: "var(--color-text-secondary)" }}>
              <span className="sr-only">TreatCode access key</span>
              <input
                aria-label="TreatCode access key"
                type="password"
                value={accessKey}
                onChange={(event) => setAccessKey(event.target.value)}
                placeholder="access key"
                style={{ width: 112, fontSize: 11, padding: "4px 6px" }}
              />
            </label>
            <button
              type="button"
              onClick={() => { auth.onLogin(accessKey); setAccessKey(""); }}
              style={{ fontSize: 12, padding: "5px 14px", borderRadius: 5, border: "0.5px solid var(--color-border-tertiary)", background: "transparent", cursor: "pointer", color: "var(--color-text-secondary)" }}
            >
              log in
            </button>
            {auth.error ? <span role="status" style={{ fontSize: 10, color: "var(--color-accent-red, #b33)" }}>{auth.error}</span> : null}
          </>
        ) : (
          <>
            <button type="button" style={{ fontSize: 12, padding: "5px 14px", borderRadius: 5, border: "0.5px solid var(--color-border-tertiary)", background: "transparent", cursor: "pointer", color: "var(--color-text-secondary)" }}>log in</button>
            <button type="button" style={{ fontSize: 12, padding: "5px 14px", borderRadius: 5, border: "0.5px solid var(--color-border-secondary)", background: "var(--color-background-primary)", cursor: "pointer", color: "var(--color-text-primary)", fontWeight: 500 }}>sign up</button>
          </>
        )}
      </div>
    </div>
  );
}

export default function App() {
  const initialView = window.location.pathname.startsWith("/practice") ? "problems" : "home";
  const [view, setView] = useState(initialView);
  const [activeTopicId, setActiveTopicId] = useState("representation-boundaries");
  const [activePathId, setActivePathId] = useState("beginner");
  const [activeProblem, setActiveProblem] = useState<Problem | null>(null);
  const [catFilter, setCatFilter] = useState("all");
  const [diffFilter, setDiffFilter] = useState("all");
  const [query, setQuery] = useState("");
  const [facetFilters, setFacetFilters] = useState<Record<ChallengeFacetKey, string>>({
    domain: "all",
    technique: "all",
    data_model: "all",
    stack_layer: "all",
    target: "all",
    optimization_objective: "all",
  });
  const [code, setCode] = useState("");
  const [running, setRunning] = useState(false);
  const [outputTab, setOutputTab] = useState<"tasm" | "vm" | "registers" | "telemetry" | "leaderboard">("vm");

  // Selection states next to editor controls
  const [username, setUsername] = useState("CoderTrit");
  const [compilerEngine, setCompilerEngine] = useState<"native" | "bootstrap">("native");
  const [optLevel, setOptLevel] = useState("-O2");

  // VM response states
  const [tasmOutput, setTasmOutput] = useState("");
  const [vmConsoleOutput, setVmConsoleOutput] = useState("");
  const [registers, setRegisters] = useState<Record<string, number>>({});
  const [prevRegisters, setPrevRegisters] = useState<Record<string, number>>({});
  
  // Telemetry metrics
  const [telemetry, setTelemetry] = useState({
    compilerCycles: 0,
    programCycles: 0,
    instructionCount: 0,
    haltStatus: "N/A",
    compilerLog: "No compilation run yet."
  });

  // Dynamic leaderboard loaded from Express API
  const [dynamicLeaderboard, setDynamicLeaderboard] = useState<LeaderboardData[]>([]);
  const [authToken, setAuthToken] = useState(() => window.localStorage.getItem("treatcode.auth.token") || "");
  const [authIdentity, setAuthIdentity] = useState<string | null>(null);
  const [authActions, setAuthActions] = useState<string[]>(["read"]);
  const [authError, setAuthError] = useState("");

  const loadAuthCapabilities = async (token: string) => {
    try {
      const response = await fetch("/api/auth/v1/capabilities", {
        headers: token ? { Authorization: `Bearer ${token}` } : {},
      });
      const payload = await response.json();
      if (!response.ok) {
        window.localStorage.removeItem("treatcode.auth.token");
        setAuthToken("");
        setAuthIdentity(null);
        setAuthActions(["read"]);
        setAuthError(payload?.error?.reason || "Session expired");
        return;
      }
      setAuthIdentity(payload.data?.principal?.display_name || null);
      setAuthActions(Array.isArray(payload.data?.actions) ? payload.data.actions : ["read"]);
      setAuthError("");
    } catch {
      setAuthError("Auth service unavailable");
    }
  };

  const login = async (accessKey: string) => {
    if (!accessKey.trim()) {
      setAuthError("Access key required");
      return;
    }
    try {
      const response = await fetch("/api/auth/v1/login", {
        method: "POST",
        headers: actionHeaders(),
        body: JSON.stringify({ identity_id: "tc:identity:demo-human", access_key: accessKey }),
      });
      const payload = await response.json();
      if (!response.ok) {
        setAuthError(payload?.error?.reason || "Login denied");
        return;
      }
      const token = payload.data.credential.token;
      window.localStorage.setItem("treatcode.auth.token", token);
      setAuthToken(token);
      setAuthIdentity(payload.data.identity?.display_name || null);
      setAuthActions(payload.data.credential.credential?.actions || []);
      setAuthError("");
    } catch {
      setAuthError("Auth service unavailable");
    }
  };

  const logout = () => {
    window.localStorage.removeItem("treatcode.auth.token");
    setAuthToken("");
    setAuthIdentity(null);
    setAuthActions(["read"]);
    setAuthError("");
  };

  const actionHeaders = (nonce = true): Record<string, string> => ({
    "Content-Type": "application/json",
    ...(authToken ? { Authorization: `Bearer ${authToken}` } : {}),
    "X-TreatCode-Project": "tc:project:trit",
    ...(nonce ? { "X-Action-Nonce": `ui-${Date.now()}-${Math.random().toString(36).slice(2, 12)}` } : {}),
  });

  const authUi: AuthUiState = {
    identityLabel: authIdentity,
    actions: authActions,
    error: authError,
    onLogin: (accessKey) => { void login(accessKey); },
    onLogout: logout,
  };

  // Fetch API leaderboard
  const loadLeaderboard = async () => {
    try {
      const res = await fetch("/api/leaderboard");
      if (res.ok) {
        const data = await res.json();
        setDynamicLeaderboard(data);
      }
    } catch (err) {
      console.error("Failed to load leaderboard", err);
    }
  };

  useEffect(() => {
    loadLeaderboard();
  }, []);

  useEffect(() => {
    if (authToken) void loadAuthCapabilities(authToken);
    else void loadAuthCapabilities("");
  }, [authToken]);

  const canTest = authActions.includes("test");
  const canSubmit = authActions.includes("edit") && authActions.includes("test");

  const filteredProblems = useMemo(() => {
    const normalizedQuery = query.trim().toLowerCase();
    return CHALLENGES.filter((p) => {
      const searchable = [p.id, p.title, p.category, p.description || "", ...p.tags, ...FACET_KEYS.flatMap((key) => p.facets[key])]
        .join(" ")
        .toLowerCase();
      return (
        (catFilter === "all" || p.category === catFilter) &&
        (diffFilter === "all" || p.difficulty === diffFilter) &&
        (!normalizedQuery || searchable.includes(normalizedQuery)) &&
        FACET_KEYS.every((key) => facetFilters[key] === "all" || p.facets[key].includes(facetFilters[key]))
      );
    });
  }, [catFilter, diffFilter, facetFilters, query]);

  const verifiedChallenges = CHALLENGES.filter((p) => p.lifecycle === "published");
  const stats = {
    total: CHALLENGES.length,
    verified: verifiedChallenges.length,
    solved: verifiedChallenges.filter((p) => p.solved).length,
  };

  function openProblem(p: Problem) {
    setActiveProblem(p);
    setCode(p.template || DEFAULT_STARTER_CODE);
    setTasmOutput("");
    setVmConsoleOutput("TreatCode Terminal initialized. Select a problem and click Run Code to begin.");
    setRegisters({});
    setPrevRegisters({});
    setTelemetry({
      compilerCycles: 0,
      programCycles: 0,
      instructionCount: 0,
      haltStatus: "N/A",
      compilerLog: "No compilation run yet."
    });
    setOutputTab("vm");
    setView("problem");
  }

  // Count non-empty TASM instructions
  function countInstructions(assemblyText: string) {
    if (!assemblyText) return 0;
    return assemblyText
      .split("\n")
      .map((line) => line.trim())
      .filter((line) => line.length > 0 && !line.startsWith("//") && !line.startsWith(";") && !line.startsWith("."))
      .length;
  }

  // Handle local Run execution
  const handleRun = async () => {
    if (!activeProblem) return;
    setRunning(true);
    setTasmOutput("");
    setVmConsoleOutput("Compiling...\n");
    setOutputTab("vm");

    try {
      const res = await fetch("/api/run", {
        method: "POST",
        headers: actionHeaders(),
        body: JSON.stringify({
          problemId: activeProblem.id,
          code,
          engine: compilerEngine,
          optLevel,
        }),
      });

      const result = await res.json();
      if (!result.success) {
        setVmConsoleOutput(`Run Failed!\n\n${result.error || "Compilation Error"}`);
        setTelemetry({
          compilerCycles: 0,
          programCycles: 0,
          instructionCount: 0,
          haltStatus: "COMPILE_ERROR",
          compilerLog: result.compilerOutput || result.error || "Error during compilation."
        });
        return;
      }

      setTasmOutput(result.assembly || "");
      
      let consoleLogText = `Compilation Successful!\n`;
      if (result.compileTimeCycles > 0) {
        consoleLogText += `Compiler VM execution: ${result.compileTimeCycles.toLocaleString()} cycles\n`;
      }
      consoleLogText += `\nVM Program Output:\n${result.consoleOutput || "(No console output)"}\n`;
      consoleLogText += `\nVM execution completed. Status: ${result.status}\n`;
      consoleLogText += `Total Program CPU Cycles: ${(result.cycles || 0).toLocaleString()}\n`;
      consoleLogText += `Return Value (R13): ${result.r13}`;
      
      setVmConsoleOutput(consoleLogText);

      // Save previous registers for highlighting delta updates
      setPrevRegisters(registers);
      setRegisters(result.registers || {});

      setTelemetry({
        compilerCycles: result.compileTimeCycles || 0,
        programCycles: result.cycles || 0,
        instructionCount: countInstructions(result.assembly),
        haltStatus: result.status || "HALT",
        compilerLog: result.compilerOutput || "No warnings."
      });

    } catch (err: any) {
      setVmConsoleOutput(`Error running code: ${err.message}`);
    } finally {
      setRunning(false);
    }
  };

  // Handle Submit optimization run
  const handleSubmit = async () => {
    if (!activeProblem) return;
    setRunning(true);
    setTasmOutput("");
    setVmConsoleOutput(`Submitting solution for verification...\nUser: ${username}\nRunning all test cases...\n`);
    setOutputTab("vm");

    try {
      const res = await fetch("/api/submit", {
        method: "POST",
        headers: actionHeaders(),
        body: JSON.stringify({
          problemId: activeProblem.id,
          code,
          engine: compilerEngine,
          username,
          optLevel,
        }),
      });

      const result = await res.json();
      if (!result.success) {
        let errorText = `Solution Rejected!\n\n`;
        if (result.results && result.results.length > 0) {
          result.results.forEach((tc: any) => {
            if (tc.passed) {
              errorText += `Test Case ${tc.testCase}: PASSED (${(tc.cycles || 0).toLocaleString()} cycles)\n`;
            } else {
              errorText += `Test Case ${tc.testCase}: FAILED\n`;
              if (tc.error) {
                errorText += `  Error: ${tc.error}\n`;
              } else {
                errorText += `  Console Output: ${tc.consoleOutput || "(none)"}\n`;
                errorText += `  Return Value (R13): ${tc.r13}\n`;
              }
            }
          });
        } else {
          errorText += result.error || "Unknown compilation error.";
        }
        setVmConsoleOutput(errorText);
        setTelemetry({
          compilerCycles: 0,
          programCycles: 0,
          instructionCount: 0,
          haltStatus: "SUBMISSION_REJECTED",
          compilerLog: result.error || "Submission rejected."
        });
        return;
      }

      let successText = `ALL TESTS PASSED!\nCongratulations! Your optimization has been recorded.\n\n`;
      result.results.forEach((tc: any) => {
        successText += `Test Case ${tc.testCase}: PASSED (${(tc.cycles || 0).toLocaleString()} cycles)\n`;
      });
      setVmConsoleOutput(successText);

      // Save previous registers for highlighting delta updates
      setPrevRegisters(registers);
      const lastTC = result.results[result.results.length - 1];
      setRegisters(lastTC.registers || {});

      setTelemetry({
        compilerCycles: 0,
        programCycles: lastTC.cycles || 0,
        instructionCount: countInstructions(tasmOutput),
        haltStatus: "HALT",
        compilerLog: "All tests compiled and passed verification successfully."
      });

      // Reload leaderboard to show the updated rank
      await loadLeaderboard();

      // Automatically switch to leaderboard tab
      setTimeout(() => {
        setOutputTab("leaderboard");
      }, 1200);

    } catch (err: any) {
      setVmConsoleOutput(`Error submitting code: ${err.message}`);
    } finally {
      setRunning(false);
    }
  };

  // Filter dynamic leaderboard for the active problem
  const filteredLeaderboard = useMemo(() => {
    if (!activeProblem) return [];
    return dynamicLeaderboard
      .filter((entry) => entry.problemId === activeProblem.id)
      .sort((a, b) => a.cycles - b.cycles);
  }, [dynamicLeaderboard, activeProblem]);

  // ── PROBLEM VIEW ────────────────────────────────────────────────────────────
  if (view === "problem" && activeProblem) {
    return (
      <div
        style={{
          fontFamily: "var(--font-sans)",
          display: "flex",
          flexDirection: "column",
          height: "100vh",
        }}
      >
        <div
          style={{
            borderBottom: "0.5px solid var(--color-border-tertiary)",
            padding: "0 20px",
            display: "flex",
            alignItems: "center",
            gap: 16,
            height: 48,
            flexShrink: 0,
          }}
        >
          <span
            onClick={() => setView("home")}
            style={{
              fontFamily: "var(--font-mono)",
              fontSize: 14,
              fontWeight: 500,
              cursor: "pointer",
              color: "var(--color-text-primary)",
            }}
          >
            TREATCODE
          </span>
          <span style={{ color: "var(--color-border-secondary)" }}>|</span>
          <span
            onClick={() => setView("problems")}
            style={{
              fontSize: 12,
              color: "var(--color-text-secondary)",
              cursor: "pointer",
            }}
          >
            ← problems
          </span>
          <span
            style={{
              fontFamily: "var(--font-mono)",
              fontSize: 12,
              color: "var(--color-text-secondary)",
            }}
          >
            {activeProblem.id}
          </span>
          {diff(activeProblem.difficulty)}
          <span style={{ fontSize: 13, fontWeight: 500 }}>
            {activeProblem.title}
          </span>
        </div>

        <div
          style={{
            display: "grid",
            gridTemplateColumns: "350px 1fr",
            flex: 1,
            overflow: "hidden",
          }}
        >
          {/* Description panel */}
          <div
            style={{
              borderRight: "0.5px solid var(--color-border-tertiary)",
              overflowY: "auto",
              padding: 20,
            }}
          >
            <div style={{ display: "flex", gap: 6, flexWrap: "wrap", marginBottom: 16 }}>
              {diff(activeProblem.difficulty)}
              {cat(activeProblem.category)}
              <span style={{ fontSize: 11, padding: "2px 7px", borderRadius: 4, fontFamily: "var(--font-mono)", color: "var(--color-text-secondary)", border: "0.5px solid var(--color-border-tertiary)" }}>
                {activeProblem.lifecycle === "published" ? "verified" : "draft / compile-only"}
              </span>
              {activeProblem.tags.map((t) => (
                <span
                  key={t}
                  style={{
                    fontSize: 11,
                    padding: "2px 6px",
                    border: "0.5px solid var(--color-border-tertiary)",
                    borderRadius: 4,
                    fontFamily: "var(--font-mono)",
                    color: "var(--color-text-secondary)",
                  }}
                >
                  {t}
                </span>
              ))}
            </div>

            <div
              style={{
                fontSize: 13,
                lineHeight: 1.7,
                color: "var(--color-text-secondary)",
                marginTop: 0,
              }}
            >
              {activeProblem.description ? (
                <div style={{ whiteSpace: "pre-line" }}>
                  {activeProblem.description}
                </div>
              ) : (
                <p>
                  Solve data structure and algorithm problems in native TCL 1.0. (Compile Check Only Mode)
                </p>
              )}
            </div>

            <div
              style={{
                borderTop: "0.5px solid var(--color-border-tertiary)",
                paddingTop: 14,
                marginTop: 16,
              }}
            >
              <div
                style={{
                  fontSize: 10,
                  fontWeight: 500,
                  color: "var(--color-text-secondary)",
                  textTransform: "uppercase",
                  letterSpacing: "0.08em",
                  marginBottom: 10,
                }}
              >
                Signature
              </div>
              <div
                style={{
                  background: "var(--color-background-secondary)",
                  borderRadius: 6,
                  padding: "8px 12px",
                  fontFamily: "var(--font-mono)",
                  fontSize: 12,
                  color: "var(--color-text-primary)",
                  border: "0.5px solid var(--color-border-tertiary)",
                }}
              >
                {activeProblem.signature}
              </div>
            </div>

            <div
              style={{
                borderTop: "0.5px solid var(--color-border-tertiary)",
                paddingTop: 14,
                marginTop: 16,
              }}
            >
              <div
                style={{
                  fontSize: 10,
                  fontWeight: 500,
                  color: "var(--color-text-secondary)",
                  textTransform: "uppercase",
                  letterSpacing: "0.08em",
                  marginBottom: 10,
                }}
              >
                Stats
              </div>
              <div
                style={{
                  display: "grid",
                  gridTemplateColumns: "1fr 1fr",
                  gap: 8,
                }}
              >
                {[
                  ["Submissions", activeProblem.lifecycle === "published" ? activeProblem.submissions.toLocaleString() : "-"],
                  ["Acceptance", activeProblem.lifecycle === "published" ? activeProblem.acceptance + "%" : "-"],
                  ["Points", activeProblem.lifecycle === "published" ? activeProblem.points : "-"],
                  ["Streak bonus", "×1.5"],
                ].map(([k, v]) => (
                  <div
                    key={k}
                    style={{
                      background: "var(--color-background-secondary)",
                      borderRadius: 6,
                      padding: "8px 10px",
                      border: "0.5px solid var(--color-border-tertiary)",
                    }}
                  >
                    <div style={{ fontSize: 10, color: "var(--color-text-secondary)", marginBottom: 3 }}>
                      {k}
                    </div>
                    <div style={{ fontSize: 13, fontFamily: "var(--font-mono)", fontWeight: 500 }}>
                      {v}
                    </div>
                  </div>
                ))}
              </div>
            </div>
          </div>

          {/* Editor + output tabs split */}
          <div
            style={{
              display: "grid",
              gridTemplateRows: "1fr 280px",
              overflow: "hidden",
            }}
          >
            {/* Editor Panel */}
            <div
              style={{
                display: "flex",
                flexDirection: "column",
                borderBottom: "0.5px solid var(--color-border-tertiary)",
              }}
            >
              {/* Editor Header Controls */}
              <div
                style={{
                  padding: "8px 16px",
                  borderBottom: "0.5px solid var(--color-border-tertiary)",
                  display: "flex",
                  justifyContent: "space-between",
                  alignItems: "center",
                  background: "var(--color-background-secondary)",
                  flexWrap: "wrap",
                  gap: 12,
                }}
              >
                <div style={{ display: "flex", alignItems: "center", gap: 14 }}>
                  <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
                    <label style={{ fontSize: 11, color: "var(--color-text-secondary)", fontWeight: 500 }}>User:</label>
                    <input
                      type="text"
                      value={username}
                      onChange={(e) => setUsername(e.target.value)}
                      placeholder="Username"
                      maxLength={20}
                      style={{
                        padding: "3px 6px",
                        fontSize: "11px",
                        width: 90,
                      }}
                    />
                  </div>
                  
                  <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
                    <label style={{ fontSize: 11, color: "var(--color-text-secondary)", fontWeight: 500 }}>Engine:</label>
                    <select
                      value={compilerEngine}
                      onChange={(e) => setCompilerEngine(e.target.value as "native" | "bootstrap")}
                      style={{
                        padding: "2px 4px",
                        fontSize: "11px",
                      }}
                    >
                      <option value="native">Native VM Compiler</option>
                      <option value="bootstrap">C++ Bootstrap</option>
                    </select>
                  </div>

                  <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
                    <label style={{ fontSize: 11, color: "var(--color-text-secondary)", fontWeight: 500 }}>Optim:</label>
                    <select
                      value={optLevel}
                      onChange={(e) => setOptLevel(e.target.value)}
                      style={{
                        padding: "2px 4px",
                        fontSize: "11px",
                      }}
                    >
                      <option value="-O2">-O2</option>
                      <option value="-O1">-O1</option>
                      <option value="-O0">-O0</option>
                    </select>
                  </div>
                </div>

                <div style={{ display: "flex", gap: 8 }}>
                  <button
                    onClick={() => setCode(activeProblem.template || DEFAULT_STARTER_CODE)}
                    style={{
                      fontSize: 12,
                      padding: "4px 12px",
                      borderRadius: 4,
                      border: "0.5px solid var(--color-border-secondary)",
                      background: "transparent",
                      cursor: "pointer",
                      color: "var(--color-text-secondary)",
                    }}
                  >
                    reset
                  </button>
                  <button
                    onClick={handleRun}
                    disabled={running || !canTest}
                    title={canTest ? "Run the bounded task" : "Log in with a credential that grants test"}
                    style={{
                      fontSize: 12,
                      padding: "4px 12px",
                      borderRadius: 4,
                      border: "0.5px solid var(--color-border-primary)",
                      background: "var(--color-background-primary)",
                      cursor: running || !canTest ? "default" : "pointer",
                      fontFamily: "var(--font-mono)",
                      color: "var(--color-text-primary)",
                      fontWeight: 500,
                      opacity: running || !canTest ? 0.6 : 1,
                    }}
                  >
                    {running ? "Compiling..." : "▶ run"}
                  </button>
                  <button
                    onClick={handleSubmit}
                    disabled={running || !canSubmit}
                    title={canSubmit ? "Submit the solution" : "Log in with edit and test grants"}
                    style={{
                      fontSize: 12,
                      padding: "4px 14px",
                      borderRadius: 4,
                      border: "none",
                      background: "var(--color-border-primary)",
                      color: "var(--color-background-primary)",
                      cursor: running || !canSubmit ? "default" : "pointer",
                      fontFamily: "var(--font-mono)",
                      fontWeight: 600,
                      opacity: running || !canSubmit ? 0.6 : 1,
                    }}
                  >
                    🚀 submit
                  </button>
                </div>
              </div>

              {/* Textarea Code Input */}
              <textarea
                value={code}
                onChange={(e) => setCode(e.target.value)}
                spellCheck={false}
                style={{
                  flex: 1,
                  resize: "none",
                  border: "none",
                  padding: "16px 20px",
                  fontFamily: "var(--font-mono)",
                  fontSize: 12.5,
                  lineHeight: 1.65,
                  background: "var(--color-background-primary)",
                  color: "var(--color-text-primary)",
                  outline: "none",
                }}
              />
            </div>

            {/* Bottom Output Tabs Panel */}
            <div
              style={{
                display: "flex",
                flexDirection: "column",
                overflow: "hidden",
                background: "var(--color-background-secondary)",
              }}
            >
              <div
                style={{
                  borderBottom: "0.5px solid var(--color-border-tertiary)",
                  display: "flex",
                  gap: 0,
                  flexShrink: 0,
                }}
              >
                {[
                  ["vm", "vm output"],
                  ["tasm", "tasm output"],
                  ["registers", "registers"],
                  ["telemetry", "telemetry"],
                  ["leaderboard", "leaderboard"],
                ].map(([t, label]) => (
                  <button
                    key={t}
                    onClick={() => setOutputTab(t as any)}
                    style={{
                      fontSize: 11,
                      padding: "8px 16px",
                      border: "none",
                      borderRight: "0.5px solid var(--color-border-tertiary)",
                      background:
                        outputTab === t
                          ? "var(--color-background-primary)"
                          : "var(--color-background-secondary)",
                      cursor: "pointer",
                      fontFamily: "var(--font-mono)",
                      color:
                        outputTab === t
                          ? "var(--color-text-primary)"
                          : "var(--color-text-secondary)",
                      fontWeight: outputTab === t ? 600 : 400,
                      borderBottom:
                        outputTab === t
                          ? "2px solid var(--color-text-primary)"
                          : "none",
                    }}
                  >
                    {label}
                  </button>
                ))}
              </div>

              <div style={{ flex: 1, overflowY: "auto", padding: "12px 20px" }}>
                {outputTab === "vm" && (
                  <pre
                    style={{
                      margin: 0,
                      fontFamily: "var(--font-mono)",
                      fontSize: 11.5,
                      lineHeight: 1.65,
                      color: vmConsoleOutput.includes("FAILED") || vmConsoleOutput.includes("Failed")
                        ? "#b91c1c"
                        : "var(--color-text-secondary)",
                    }}
                  >
                    {vmConsoleOutput}
                  </pre>
                )}

                {outputTab === "tasm" && (
                  <pre
                    style={{
                      margin: 0,
                      fontFamily: "var(--font-mono)",
                      fontSize: 11.5,
                      lineHeight: 1.65,
                      color: "var(--color-text-secondary)",
                    }}
                  >
                    {tasmOutput || "; Run code to generate TASM Assembly output"}
                  </pre>
                )}

                {outputTab === "registers" && (
                  <div>
                    <div
                      style={{
                        fontSize: 11,
                        color: "var(--color-text-secondary)",
                        marginBottom: 10,
                        fontFamily: "var(--font-sans)",
                      }}
                    >
                      Ternary VM Register File (r0 - r26). Changed registers from last execution run are highlighted.
                    </div>
                    <div
                      style={{
                        display: "grid",
                        gridTemplateColumns: "repeat(auto-fill, minmax(130px, 1fr))",
                        gap: 6,
                      }}
                    >
                      {Array.from({ length: 27 }, (_, i) => {
                        const regName = `r${i}`;
                        const val = registers[regName] !== undefined ? registers[regName] : 0;
                        const hasChanged = prevRegisters[regName] !== undefined && registers[regName] !== prevRegisters[regName];
                        
                        let labelSuffix = "";
                        if (i === 13) labelSuffix = " (RET)";
                        if (i === 26) labelSuffix = " (SP)";

                        return (
                          <div
                            key={regName}
                            style={{
                              background: hasChanged ? "#eff6ff" : "var(--color-background-primary)",
                              border: hasChanged
                                ? "0.5px solid #3b82f6"
                                : "0.5px solid var(--color-border-tertiary)",
                              padding: "4px 8px",
                              borderRadius: 4,
                              display: "flex",
                              flexDirection: "column",
                              fontFamily: "var(--font-mono)",
                              fontSize: 11,
                            }}
                          >
                            <span style={{ color: "var(--color-text-secondary)", fontSize: 10 }}>
                              {regName}{labelSuffix}
                            </span>
                            <span style={{ fontWeight: 600, color: "var(--color-text-primary)" }}>
                              {val} <span style={{ color: "var(--color-text-muted)", fontWeight: 400, fontSize: 10 }}>({toBalancedTernary(val)})</span>
                            </span>
                          </div>
                        );
                      })}
                    </div>
                  </div>
                )}

                {outputTab === "telemetry" && (
                  <div>
                    <div
                      style={{
                        display: "grid",
                        gridTemplateColumns: "repeat(2, 1fr)",
                        gap: 10,
                        marginBottom: 16,
                      }}
                    >
                      {[
                        ["Compiler VM cycles", telemetry.compilerCycles.toLocaleString()],
                        ["Program VM CPU cycles", telemetry.programCycles.toLocaleString()],
                        ["TASM Instruction words", telemetry.instructionCount],
                        ["VM Halt status", telemetry.haltStatus],
                      ].map(([k, v]) => (
                        <div
                          key={k}
                          style={{
                            background: "var(--color-background-primary)",
                            border: "0.5px solid var(--color-border-tertiary)",
                            borderRadius: 6,
                            padding: "8px 12px",
                          }}
                        >
                          <div style={{ fontSize: 10, color: "var(--color-text-secondary)", textTransform: "uppercase" }}>{k}</div>
                          <div style={{ fontSize: 18, fontWeight: 700, marginTop: 2 }}>{v}</div>
                        </div>
                      ))}
                    </div>
                    <div style={{ fontSize: 11, fontWeight: 500, color: "var(--color-text-secondary)", marginBottom: 6 }}>
                      Raw Compiler Output:
                    </div>
                    <pre
                      style={{
                        background: "var(--color-background-primary)",
                        border: "0.5px solid var(--color-border-tertiary)",
                        padding: "8px 12px",
                        borderRadius: 6,
                        fontFamily: "var(--font-mono)",
                        fontSize: 10.5,
                        lineHeight: 1.4,
                        whiteSpace: "pre-wrap",
                        color: "var(--color-text-secondary)",
                      }}
                    >
                      {telemetry.compilerLog}
                    </pre>
                  </div>
                )}

                {outputTab === "leaderboard" && (
                  <div>
                    <table style={{ width: "100%", borderCollapse: "collapse", fontSize: 12 }}>
                      <thead>
                        <tr style={{ borderBottom: "1px solid var(--color-border-tertiary)", textAlign: "left" }}>
                          <th style={{ padding: "4px 8px", color: "var(--color-text-secondary)" }}>Rank</th>
                          <th style={{ padding: "4px 8px", color: "var(--color-text-secondary)" }}>Handle</th>
                          <th style={{ padding: "4px 8px", color: "var(--color-text-secondary)" }}>Compiler</th>
                          <th style={{ padding: "4px 8px", color: "var(--color-text-secondary)" }}>Cycles</th>
                          <th style={{ padding: "4px 8px", color: "var(--color-text-secondary)" }}>Date</th>
                        </tr>
                      </thead>
                      <tbody>
                        {filteredLeaderboard.length === 0 ? (
                          <tr>
                            <td colSpan={5} style={{ padding: 12, textAlign: "center", color: "var(--color-text-muted)" }}>
                              No optimizations submitted yet. Be the first!
                            </td>
                          </tr>
                        ) : (
                          filteredLeaderboard.map((u, i) => (
                            <tr
                              key={u.id}
                              style={{
                                borderBottom: "0.5px solid var(--color-border-tertiary)",
                                background: i % 2 === 0 ? "var(--color-background-primary)" : "transparent",
                              }}
                            >
                              <td style={{ padding: "6px 8px", fontWeight: 600 }}>#{i + 1}</td>
                              <td style={{ padding: "6px 8px", fontFamily: "var(--font-mono)" }}>{u.name}</td>
                              <td style={{ padding: "6px 8px" }}>
                                <span
                                  style={{
                                    fontSize: 10,
                                    padding: "2px 5px",
                                    borderRadius: 3,
                                    background: u.engine === "native" ? "#f3e8ff" : "#dbeafe",
                                    color: u.engine === "native" ? "#6b21a8" : "#1e40af",
                                    fontWeight: 500,
                                  }}
                                >
                                  {u.engine === "native" ? "Native VM" : "C++ Boot"}
                                </span>
                              </td>
                              <td style={{ padding: "6px 8px", fontWeight: 600, fontFamily: "var(--font-mono)" }}>
                                {u.cycles.toLocaleString()}
                              </td>
                              <td style={{ padding: "6px 8px", color: "var(--color-text-secondary)" }}>{u.date}</td>
                            </tr>
                          ))
                        )}
                      </tbody>
                    </table>
                  </div>
                )}
              </div>
            </div>
          </div>
        </div>
      </div>
    );
  }

  // ── CHALLENGES LIST ───────────────────────────────────────────────────────────
  if (view === "problems") {
    return (
      <div
        style={{
          fontFamily: "var(--font-sans)",
          maxWidth: 1060,
          margin: "0 auto",
          padding: "0 24px",
        }}
      >
        <Nav view={view} setView={setView} auth={authUi} />
        <div style={{ padding: "28px 0" }}>
          <div
            style={{
              display: "grid",
              gridTemplateColumns: "repeat(5, 1fr)",
              gap: 10,
              marginBottom: 24,
            }}
          >
            {[
              ["total", CHALLENGES.length, "var(--color-text-primary)"],
              ["solved", stats.solved, "#3B6D11"],
              ["easy", CHALLENGES.filter((p) => p.difficulty === "easy").length, "#3B6D11"],
              ["medium", CHALLENGES.filter((p) => p.difficulty === "medium").length, "#854F0B"],
              ["hard", CHALLENGES.filter((p) => p.difficulty === "hard").length, "#A32D2D"],
            ].map(([k, v, c]) => (
              <div
                key={k as string}
                style={{
                  background: "var(--color-background-secondary)",
                  borderRadius: 8,
                  padding: "12px 16px",
                  border: "0.5px solid var(--color-border-tertiary)",
                }}
              >
                <div
                  style={{
                    fontSize: 10,
                    color: "var(--color-text-secondary)",
                    marginBottom: 4,
                    textTransform: "uppercase",
                    letterSpacing: "0.08em",
                  }}
                >
                  {k as string}
                </div>
                <div
                  style={{
                    fontSize: 24,
                    fontFamily: "var(--font-mono)",
                    fontWeight: 500,
                    color: c as string,
                  }}
                >
                  {v as number}
                </div>
              </div>
            ))}
          </div>

          <div style={{ display: "flex", gap: 8, marginBottom: 18, flexWrap: "wrap" }}>
            {["all", "easy", "medium", "hard"].map((d) => (
              <button
                key={d}
                onClick={() => setDiffFilter(d)}
                style={{
                  fontSize: 12,
                  padding: "4px 12px",
                  borderRadius: 4,
                  border: "0.5px solid var(--color-border-secondary)",
                  background:
                    diffFilter === d
                      ? "var(--color-background-tertiary)"
                      : "var(--color-background-primary)",
                  cursor: "pointer",
                  fontFamily: "var(--font-mono)",
                  color: "var(--color-text-primary)",
                }}
              >
                {d}
              </button>
            ))}
            <div
              style={{
                width: 1,
                background: "var(--color-border-tertiary)",
                margin: "0 4px",
              }}
            />
            {CATEGORIES.map((c) => (
              <button
                key={c.id}
                onClick={() => setCatFilter(c.id)}
                style={{
                  fontSize: 12,
                  padding: "4px 12px",
                  borderRadius: 4,
                  border: "0.5px solid var(--color-border-secondary)",
                  background:
                    catFilter === c.id
                      ? "var(--color-background-tertiary)"
                      : "var(--color-background-primary)",
                  cursor: "pointer",
                  color: "var(--color-text-primary)",
                }}
              >
                {c.label}
              </button>
            ))}
          </div>

          <div
            aria-label="Challenge search and facets"
            style={{
              display: "grid",
              gridTemplateColumns: "minmax(220px, 1.3fr) repeat(3, minmax(145px, 1fr))",
              gap: 8,
              marginBottom: 18,
            }}
          >
            <label style={{ display: "flex", flexDirection: "column", gap: 5, fontSize: 10, color: "var(--color-text-secondary)", textTransform: "uppercase", letterSpacing: "0.07em" }}>
              Search
              <input
                value={query}
                onChange={(event) => setQuery(event.target.value)}
                placeholder="title, tag, facet, or ID"
                aria-label="Search challenges"
                style={{ padding: "8px 10px", border: "0.5px solid var(--color-border-secondary)", borderRadius: 5, background: "var(--color-background-primary)", color: "var(--color-text-primary)", fontSize: 12, textTransform: "none", letterSpacing: "normal" }}
              />
            </label>
            {FACET_KEYS.slice(0, 3).map((key) => (
              <label key={key} style={{ display: "flex", flexDirection: "column", gap: 5, fontSize: 10, color: "var(--color-text-secondary)", textTransform: "uppercase", letterSpacing: "0.07em" }}>
                {key.replace(/_/g, " ")}
                <select
                  value={facetFilters[key]}
                  onChange={(event) => setFacetFilters((current) => ({ ...current, [key]: event.target.value }))}
                  aria-label={`Filter by ${key.replace(/_/g, " ")}`}
                  style={{ padding: "7px 8px", border: "0.5px solid var(--color-border-secondary)", borderRadius: 5, background: "var(--color-background-primary)", color: "var(--color-text-primary)", fontSize: 12 }}
                >
                  <option value="all">All</option>
                  {CHALLENGE_DATA.facet_dimensions[key].map((value) => <option key={value} value={value}>{value}</option>)}
                </select>
              </label>
            ))}
          </div>
          <div style={{ display: "flex", gap: 8, flexWrap: "wrap", alignItems: "center", marginBottom: 18 }}>
            {FACET_KEYS.slice(3).map((key) => (
              <label key={key} style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 11, color: "var(--color-text-secondary)" }}>
                <span>{key.replace(/_/g, " ")}</span>
                <select
                  value={facetFilters[key]}
                  onChange={(event) => setFacetFilters((current) => ({ ...current, [key]: event.target.value }))}
                  aria-label={`Filter by ${key.replace(/_/g, " ")}`}
                  style={{ padding: "4px 6px", border: "0.5px solid var(--color-border-secondary)", borderRadius: 4, background: "var(--color-background-primary)", color: "var(--color-text-primary)", fontSize: 11 }}
                >
                  <option value="all">all</option>
                  {CHALLENGE_DATA.facet_dimensions[key].map((value) => <option key={value} value={value}>{value}</option>)}
                </select>
              </label>
            ))}
            <span style={{ marginLeft: "auto", fontSize: 11, color: "var(--color-text-secondary)" }}>
              {filteredProblems.length} shown · draft entries are compile-only
            </span>
          </div>

          <div
            style={{
              border: "0.5px solid var(--color-border-tertiary)",
              borderRadius: 8,
              overflow: "hidden",
            }}
          >
            <table style={{ width: "100%", borderCollapse: "collapse", fontSize: 13 }}>
              <thead>
                <tr style={{ background: "var(--color-background-secondary)" }}>
                  {["", "ID", "Title", "Difficulty", "Category", "Tags", "Acceptance", "Pts"].map((h) => (
                    <th
                      key={h}
                      style={{
                        padding: "9px 14px",
                        textAlign: "left",
                        fontSize: 10,
                        fontWeight: 500,
                        color: "var(--color-text-secondary)",
                        textTransform: "uppercase",
                        letterSpacing: "0.07em",
                        borderBottom: "0.5px solid var(--color-border-tertiary)",
                      }}
                    >
                      {h}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {filteredProblems.map((p, i) => (
                  <tr
                    key={p.id}
                    onClick={() => openProblem(p)}
                    style={{
                      borderBottom: "0.5px solid var(--color-border-tertiary)",
                      cursor: "pointer",
                      background:
                        i % 2 === 0
                          ? "var(--color-background-primary)"
                          : "var(--color-background-secondary)",
                    }}
                  >
                    <td style={{ padding: "10px 14px", width: 28 }}>
                      <span
                        style={{
                          fontSize: 13,
                          color: p.solved ? "#3B6D11" : "var(--color-border-secondary)",
                        }}
                      >
                        {p.solved ? "✓" : "○"}
                      </span>
                    </td>
                    <td
                      style={{
                        padding: "10px 14px",
                        fontFamily: "var(--font-mono)",
                        fontSize: 11,
                        color: "var(--color-text-secondary)",
                      }}
                    >
                      {p.id}
                    </td>
                    <td style={{ padding: "10px 14px", fontWeight: 500 }}>
                      <div>{p.title}</div>
                      {p.lifecycle !== "published" && <div style={{ marginTop: 3, fontSize: 10, color: "var(--color-text-secondary)", fontFamily: "var(--font-mono)" }}>draft / compile-only</div>}
                    </td>
                    <td style={{ padding: "10px 14px" }}>
                      {diff(p.difficulty)}
                    </td>
                    <td style={{ padding: "10px 14px" }}>{cat(p.category)}</td>
                    <td style={{ padding: "10px 14px" }}>
                      <div style={{ display: "flex", gap: 4, flexWrap: "wrap" }}>
                        {p.tags.slice(0, 2).map((t) => (
                          <span
                            key={t}
                            style={{
                              fontSize: 10,
                              padding: "1px 6px",
                              border: "0.5px solid var(--color-border-tertiary)",
                              borderRadius: 3,
                              fontFamily: "var(--font-mono)",
                              color: "var(--color-text-secondary)",
                            }}
                          >
                            {t}
                          </span>
                        ))}
                      </div>
                    </td>
                    <td
                      style={{
                        padding: "10px 14px",
                        fontFamily: "var(--font-mono)",
                        fontSize: 12,
                        color: "var(--color-text-secondary)",
                      }}
                    >
                      {p.lifecycle === "published" ? `${p.acceptance}%` : "-"}
                    </td>
                    <td
                      style={{
                        padding: "10px 14px",
                        fontFamily: "var(--font-mono)",
                        fontSize: 12,
                      }}
                    >
                      {p.lifecycle === "published" ? p.points : "-"}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      </div>
    );
  }

  // ── LEADERBOARD ─────────────────────────────────────────────────────────────
  if (view === "leaderboard") {
    const bc = { gold: "#d97706", silver: "#888780", bronze: "#854F0B" };
    
    // Aggregate leaderboard entries for top list
    const aggregatedLeaderboard = dynamicLeaderboard
      .slice()
      .sort((a, b) => a.cycles - b.cycles);

    return (
      <div
        style={{
          fontFamily: "var(--font-sans)",
          maxWidth: 760,
          margin: "0 auto",
          padding: "0 24px",
        }}
      >
        <Nav view={view} setView={setView} auth={authUi} />
        <div style={{ padding: "28px 0" }}>
          <div
            style={{
              display: "grid",
              gridTemplateColumns: "repeat(3, 1fr)",
              gap: 10,
              marginBottom: 28,
            }}
          >
            {aggregatedLeaderboard.slice(0, 3).map((u, i) => (
              <div
                key={u.id}
                style={{
                  background: "var(--color-background-secondary)",
                  borderRadius: 10,
                  padding: "16px 20px",
                  border: `0.5px solid var(--color-border-tertiary)`,
                  textAlign: "center",
                }}
              >
                <div style={{ fontSize: 22, marginBottom: 6 }}>
                  {["🥇", "🥈", "🥉"][i]}
                </div>
                <div
                  style={{
                    fontFamily: "var(--font-mono)",
                    fontSize: 14,
                    fontWeight: 500,
                    marginBottom: 4,
                    color: bc.gold,
                  }}
                >
                  {u.name}
                </div>
                <div
                  style={{
                    fontFamily: "var(--font-mono)",
                    fontSize: 20,
                    fontWeight: 500,
                  }}
                >
                  {u.cycles.toLocaleString()}
                </div>
                <div
                  style={{
                    fontSize: 11,
                    color: "var(--color-text-secondary)",
                    marginTop: 4,
                  }}
                >
                  {u.problemId} · {u.engine === "native" ? "Native VM" : "C++ Boot"}
                </div>
              </div>
            ))}
          </div>

          <div
            style={{
              border: "0.5px solid var(--color-border-tertiary)",
              borderRadius: 8,
              overflow: "hidden",
            }}
          >
            <table style={{ width: "100%", borderCollapse: "collapse", fontSize: 13 }}>
              <thead>
                <tr style={{ background: "var(--color-background-secondary)" }}>
                  {["Rank", "Problem", "Handle", "Compiler", "Cycles", "Date"].map((h) => (
                    <th
                      key={h}
                      style={{
                        padding: "9px 18px",
                        textAlign: "left",
                        fontSize: 10,
                        fontWeight: 500,
                        color: "var(--color-text-secondary)",
                        textTransform: "uppercase",
                        letterSpacing: "0.07em",
                        borderBottom: "0.5px solid var(--color-border-tertiary)",
                      }}
                    >
                      {h}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {aggregatedLeaderboard.map((u, i) => (
                  <tr
                    key={u.id}
                    style={{
                      borderBottom: "0.5px solid var(--color-border-tertiary)",
                      background:
                        i % 2 === 0
                          ? "var(--color-background-primary)"
                          : "var(--color-background-secondary)",
                    }}
                  >
                    <td
                      style={{
                        padding: "11px 18px",
                        fontFamily: "var(--font-mono)",
                        fontWeight: 500,
                      }}
                    >
                      #{i + 1}
                    </td>
                    <td style={{ padding: "11px 18px", fontWeight: 600 }}>{u.problemId}</td>
                    <td
                      style={{
                        padding: "11px 18px",
                        fontFamily: "var(--font-mono)",
                        fontWeight: 500,
                      }}
                    >
                      {u.name}
                    </td>
                    <td style={{ padding: "11px 18px" }}>
                      <span
                        style={{
                          fontSize: 11,
                          padding: "2px 8px",
                          borderRadius: 4,
                          background: u.engine === "native" ? "#f3e8ff" : "#dbeafe",
                          color: u.engine === "native" ? "#6b21a8" : "#1e40af",
                          fontWeight: 500,
                        }}
                      >
                        {u.engine === "native" ? "Native VM" : "C++ Boot"}
                      </span>
                    </td>
                    <td
                      style={{
                        padding: "11px 18px",
                        fontFamily: "var(--font-mono)",
                        fontWeight: 600,
                      }}
                    >
                      {u.cycles.toLocaleString()}
                    </td>
                    <td style={{ padding: "11px 18px", color: "var(--color-text-secondary)" }}>
                      {u.date}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      </div>
    );
  }

  // ── CONTRIBUTE ──────────────────────────────────────────────────────────────
  if (view === "contribute") {
    return (
      <div
        style={{
          fontFamily: "var(--font-sans)",
          maxWidth: 820,
          margin: "0 auto",
          padding: "0 24px",
        }}
      >
        <Nav view={view} setView={setView} auth={authUi} />
        <div style={{ padding: "28px 0" }}>
          <h2 style={{ fontSize: 18, fontWeight: 500, margin: "0 0 6px" }}>
            Contribute to ulib
          </h2>
          <p
            style={{
              fontSize: 13,
              color: "var(--color-text-secondary)",
              margin: "0 0 28px",
              lineHeight: 1.7,
            }}
          >
            Functions accepted by the community are merged into {mono("ulib.trit")} with full credit. Every accepted function ships with an advisory spec document covering struct layout, constraints, and what the function is not.
          </p>

          <div
            style={{
              display: "grid",
              gridTemplateColumns: "1fr 1fr",
              gap: 10,
              marginBottom: 28,
            }}
          >
            {[
              {
                level: "L1",
                title: "Solver",
                desc: "Solve problems using existing ulib primitives.",
                bg: "#EAF3DE",
                c: "#3B6D11",
              },
              {
                level: "L2",
                title: "Optimizer",
                desc: "Beat the reference solution's VM cycle count.",
                bg: "#E6F1FB",
                c: "#185FA5",
              },
              {
                level: "L3",
                title: "Contributor",
                desc: "Submit a new function with impl, tests, and spec.",
                bg: "#EEEDFE",
                c: "#534AB7",
              },
              {
                level: "L4",
                title: "Auditor",
                desc: "Review L3 submissions for correctness and ownership.",
                bg: "#FAEEDA",
                c: "#854F0B",
              },
            ].map((l) => (
              <div
                key={l.level}
                style={{
                  background: "var(--color-background-primary)",
                  border: "0.5px solid var(--color-border-tertiary)",
                  borderRadius: 8,
                  padding: "14px 18px",
                }}
              >
                <div
                  style={{
                    display: "flex",
                    alignItems: "center",
                    gap: 8,
                    marginBottom: 7,
                  }}
                >
                  <span
                    style={{
                      background: l.bg,
                      color: l.c,
                      fontSize: 11,
                      padding: "2px 8px",
                      borderRadius: 4,
                      fontFamily: "var(--font-mono)",
                      fontWeight: 500,
                    }}
                  >
                    {l.level}
                  </span>
                  <span style={{ fontSize: 14, fontWeight: 500 }}>
                    {l.title}
                  </span>
                </div>
                <p
                  style={{
                    fontSize: 12,
                    color: "var(--color-text-secondary)",
                    margin: 0,
                    lineHeight: 1.6,
                  }}
                >
                  {l.desc}
                </p>
              </div>
            ))}
          </div>

          <div
            style={{
              background: "var(--color-background-secondary)",
              borderRadius: 8,
              padding: "18px 22px",
              marginBottom: 24,
              border: "0.5px solid var(--color-border-tertiary)",
            }}
          >
            <div
              style={{
                fontSize: 11,
                fontWeight: 500,
                color: "var(--color-text-secondary)",
                marginBottom: 12,
                textTransform: "uppercase",
                letterSpacing: "0.08em",
              }}
            >
              L3 submission checklist
            </div>
            {[
              "Implementation in TCL 1.0 with correct ownership semantics",
              "Test suite: edge cases, null/empty, overflow, concurrent access",
              "Complexity analysis in ternary terms (base-3 where applicable)",
              "Advisory spec: constraints, struct layout, public API, what this is not",
              "A motivating problem that existing ulib handles poorly",
              "No internal locking — concurrency is the caller's responsibility",
            ].map((item, i) => (
              <div
                key={i}
                style={{
                  display: "flex",
                  gap: 10,
                  marginBottom: 7,
                  fontSize: 13,
                  alignItems: "flex-start",
                }}
              >
                <span
                  style={{
                    color: "var(--color-text-secondary)",
                    fontFamily: "var(--font-mono)",
                    fontSize: 11,
                    marginTop: 3,
                    flexShrink: 0,
                  }}
                >
                  □
                </span>
                <span style={{ lineHeight: 1.55 }}>{item}</span>
              </div>
            ))}
          </div>

          <div style={{ fontSize: 14, fontWeight: 500, marginBottom: 12 }}>
            Open Proposals
          </div>
          <div
            style={{
              border: "0.5px solid var(--color-border-tertiary)",
              borderRadius: 8,
              overflow: "hidden",
            }}
          >
            {PROPOSALS.map((p, i) => (
              <div
                key={p.id}
                style={{
                  padding: "13px 16px",
                  borderBottom:
                    i < PROPOSALS.length - 1
                      ? "0.5px solid var(--color-border-tertiary)"
                      : "none",
                  background:
                    i % 2 === 0
                      ? "var(--color-background-primary)"
                      : "var(--color-background-secondary)",
                }}
              >
                <div
                  style={{
                    display: "flex",
                    alignItems: "center",
                    gap: 12,
                    marginBottom: 4,
                  }}
                >
                  <span
                    style={{
                      fontFamily: "var(--font-mono)",
                      fontSize: 11,
                      color: "var(--color-text-secondary)",
                      minWidth: 56,
                    }}
                  >
                    {p.id}
                  </span>
                  <span
                    style={{
                      fontFamily: "var(--font-mono)",
                      fontSize: 13,
                      fontWeight: 500,
                      flex: 1,
                    }}
                  >
                    {p.title}
                  </span>
                  <span
                    style={{ fontSize: 12, color: "var(--color-text-secondary)" }}
                  >
                    by {p.author}
                  </span>
                  <span
                    style={{
                      fontSize: 11,
                      padding: "2px 8px",
                      borderRadius: 4,
                      background: p.status === "review" ? "#E1F5EE" : "#F1EFE8",
                      color: p.status === "review" ? "#0F6E56" : "#5F5E5A",
                    }}
                  >
                    {p.status}
                  </span>
                  <span
                    style={{
                      fontFamily: "var(--font-mono)",
                      fontSize: 12,
                      color: "var(--color-text-secondary)",
                      minWidth: 36,
                      textAlign: "right",
                    }}
                  >
                    ▲ {p.votes}
                  </span>
                </div>
                <div
                  style={{
                    paddingLeft: 68,
                    fontSize: 12,
                    color: "var(--color-text-secondary)",
                    lineHeight: 1.5,
                  }}
                >
                  {p.desc}
                </div>
              </div>
            ))}
          </div>
        </div>
      </div>
    );
  }

  // ── GUIDE ───────────────────────────────────────────────────────────────────
  if (view === "guide") {
    const activePath =
      LEARNING_CATALOG.paths.find((path) => path.id === activePathId) ||
      LEARNING_CATALOG.paths[0];
    const pathPages = activePath.page_ids
      .map((pageId) => LEARNING_PAGES.find((page) => page.id === pageId))
      .filter((page): page is (typeof LEARNING_PAGES)[number] => Boolean(page));
    const flatTopics = pathPages.map((page) => ({
      ...page,
      moduleTitle: page.module,
    }));

    const activeIndex = flatTopics.findIndex((t) => t.id === activeTopicId);
    const activeTopic = flatTopics[activeIndex] || flatTopics[0];

    const prevTopic = activeIndex > 0 ? flatTopics[activeIndex - 1] : null;
    const nextTopic = activeIndex < flatTopics.length - 1 ? flatTopics[activeIndex + 1] : null;

    const selectPath = (pathId: string) => {
      const selectedPath = LEARNING_CATALOG.paths.find((path) => path.id === pathId);
      setActivePathId(pathId);
      if (selectedPath) {
        setActiveTopicId(selectedPath.page_ids[0]);
      }
    };

    return (
      <div
        style={{
          fontFamily: "var(--font-sans)",
          maxWidth: 1060,
          margin: "0 auto",
          padding: "0 24px",
        }}
      >
        <Nav view={view} setView={setView} auth={authUi} />
        <section
          aria-labelledby="learning-title"
          style={{
            padding: "30px 0 24px",
            borderBottom: "1px solid var(--color-border-tertiary)",
          }}
        >
          <div
            style={{
              fontSize: 11,
              fontWeight: 600,
              letterSpacing: "0.08em",
              textTransform: "uppercase",
              color: "var(--color-text-muted)",
              marginBottom: 8,
            }}
          >
            Repository-backed learning
          </div>
          <h1
            id="learning-title"
            style={{
              fontSize: 28,
              fontWeight: 600,
              margin: "0 0 10px",
              letterSpacing: "-0.02em",
            }}
          >
            Learn the Trit stack from trit to app.
          </h1>
          <p
            style={{
              maxWidth: 700,
              fontSize: 14,
              lineHeight: 1.65,
              color: "var(--color-text-secondary)",
              margin: "0 0 20px",
            }}
          >
            Each page names its production source, validation evidence,
            prerequisites, and next step. Choose a path for the amount of
            implementation context you want.
          </p>
          <div
            role="group"
            aria-label="Learning path"
            style={{ display: "flex", flexWrap: "wrap", gap: 8 }}
          >
            {LEARNING_CATALOG.paths.map((path) => {
              const isActive = path.id === activePath.id;
              return (
                <button
                  key={path.id}
                  type="button"
                  aria-pressed={isActive}
                  onClick={() => selectPath(path.id)}
                  style={{
                    flex: "1 1 200px",
                    maxWidth: 320,
                    padding: "11px 14px",
                    borderRadius: 7,
                    border: isActive
                      ? "1px solid var(--color-border-primary)"
                      : "1px solid var(--color-border-tertiary)",
                    background: isActive
                      ? "var(--color-background-tertiary)"
                      : "var(--color-background-primary)",
                    color: "var(--color-text-primary)",
                    cursor: "pointer",
                    textAlign: "left",
                  }}
                >
                  <span style={{ display: "block", fontSize: 13, fontWeight: 600 }}>
                    {path.title}
                  </span>
                  <span
                    style={{
                      display: "block",
                      marginTop: 4,
                      fontSize: 11,
                      lineHeight: 1.4,
                      color: "var(--color-text-secondary)",
                    }}
                  >
                    {path.audience}
                  </span>
                </button>
              );
            })}
          </div>
        </section>
        <div
          style={{
            display: "flex",
            gap: 32,
            padding: "28px 0",
            alignItems: "stretch",
          }}
        >
          {/* Sidebar */}
          <aside aria-label="Learning page navigation"
            style={{
              width: 280,
              flexShrink: 0,
              borderRight: "1px solid var(--color-border-tertiary)",
              paddingRight: 24,
            }}
          >
            {[
              {
                id: activePath.id,
                title: activePath.title,
                topics: pathPages,
              },
            ].map((mod) => (
              <div key={mod.id} style={{ marginBottom: 20 }}>
                <h3
                  style={{
                    fontSize: 11,
                    fontWeight: 600,
                    textTransform: "uppercase",
                    letterSpacing: "0.05em",
                    color: "var(--color-text-primary)",
                    marginBottom: 8,
                    padding: "2px 0",
                  }}
                >
                  {mod.title}
                </h3>
                <div style={{ display: "flex", flexDirection: "column", gap: 2 }}>
                  {mod.topics.map((top) => {
                    const isActive = activeTopicId === top.id;
                    return (
                      <button
                        key={top.id}
                        onClick={() => setActiveTopicId(top.id)}
                        style={{
                          textAlign: "left",
                          fontSize: 13,
                          padding: "6px 12px",
                          borderRadius: 4,
                          border: "none",
                          background: isActive
                            ? "var(--color-background-tertiary)"
                            : "transparent",
                          color: isActive
                            ? "var(--color-text-primary)"
                            : "var(--color-text-secondary)",
                          fontWeight: isActive ? 500 : 400,
                          cursor: "pointer",
                          borderLeft: isActive
                            ? "3px solid var(--color-border-primary)"
                            : "3px solid transparent",
                          paddingLeft: isActive ? 9 : 12,
                        }}
                      >
                        {top.title}
                      </button>
                    );
                  })}
                </div>
              </div>
            ))}
          </aside>

          {/* Main Content */}
          <main aria-labelledby="learning-page-title"
            style={{
              flex: 1,
              minWidth: 0,
              paddingLeft: 8,
            }}
          >
            {/* Header */}
            <div
              style={{
                marginBottom: 24,
                borderBottom: "1px solid var(--color-border-tertiary)",
                paddingBottom: 16,
              }}
            >
              <div
                style={{
                  fontSize: 11,
                  fontWeight: 600,
                  textTransform: "uppercase",
                  letterSpacing: "0.05em",
                  color: "var(--color-text-muted)",
                  marginBottom: 6,
                }}
              >
                {activeTopic.moduleTitle}
              </div>
              <h1
                id="learning-page-title"
                style={{
                  fontSize: 24,
                  fontWeight: 600,
                  color: "var(--color-text-primary)",
                  margin: 0,
                }}
              >
                {activeTopic.title}
              </h1>
              <p
                style={{
                  margin: "9px 0 0",
                  maxWidth: 700,
                  fontSize: 14,
                  lineHeight: 1.55,
                  color: "var(--color-text-secondary)",
                }}
              >
                {activeTopic.summary}
              </p>
              <div
                style={{
                  marginTop: 12,
                  fontSize: 11,
                  color: "var(--color-text-muted)",
                }}
              >
                Prerequisites: {activeTopic.prerequisites.length > 0
                  ? activeTopic.prerequisites
                      .map((id) => LEARNING_PAGES.find((page) => page.id === id)?.title || id)
                      .join(", ")
                  : "none"}
              </div>
            </div>

            {/* Content */}
            <div style={{ marginBottom: 40 }}>
              {renderMarkdown(activeTopic.content)}
            </div>

            <LearningInteractiveModule
              key={activeTopic.id}
              module={activeTopic.interactive}
              onOpenChallenges={() => openProblem(CHALLENGES[0])}
            />

            <LearningProvenance page={activeTopic} />

            {/* Footer Navigation */}
            <div
              aria-label="Learning page navigation"
              style={{
                display: "flex",
                justifyContent: "space-between",
                alignItems: "center",
                borderTop: "1px solid var(--color-border-tertiary)",
                paddingTop: 24,
                marginTop: 24,
              }}
            >
              {prevTopic ? (
                <button
                  type="button"
                  onClick={() => setActiveTopicId(prevTopic.id)}
                  style={{
                    display: "inline-flex",
                    alignItems: "center",
                    gap: 8,
                    padding: "8px 16px",
                    borderRadius: 6,
                    border: "1px solid var(--color-border-secondary)",
                    background: "var(--color-background-primary)",
                    color: "var(--color-text-primary)",
                    fontSize: 13,
                    fontWeight: 500,
                    cursor: "pointer",
                    textAlign: "left",
                  }}
                >
                  <span style={{ fontSize: 16 }}>←</span>
                  <div>
                    <div
                      style={{
                        fontSize: 10,
                        color: "var(--color-text-muted)",
                        textTransform: "uppercase",
                      }}
                    >
                      Previous
                    </div>
                    <div>{prevTopic.title}</div>
                  </div>
                </button>
              ) : (
                <div />
              )}

              {nextTopic ? (
                <button
                  type="button"
                  onClick={() => setActiveTopicId(nextTopic.id)}
                  style={{
                    display: "inline-flex",
                    alignItems: "center",
                    gap: 8,
                    padding: "8px 16px",
                    borderRadius: 6,
                    border: "1px solid var(--color-border-secondary)",
                    background: "var(--color-background-primary)",
                    color: "var(--color-text-primary)",
                    fontSize: 13,
                    fontWeight: 500,
                    cursor: "pointer",
                    textAlign: "right",
                  }}
                >
                  <div>
                    <div
                      style={{
                        fontSize: 10,
                        color: "var(--color-text-muted)",
                        textTransform: "uppercase",
                      }}
                    >
                      Next
                    </div>
                    <div>{nextTopic.title}</div>
                  </div>
                  <span style={{ fontSize: 16 }}>→</span>
                </button>
              ) : (
                <div />
              )}
            </div>
          </main>
        </div>
      </div>
    );
  }

  // ── HOME ────────────────────────────────────────────────────────────────────
  return (
    <div
      style={{
        fontFamily: "var(--font-sans)",
        maxWidth: 1060,
        margin: "0 auto",
        padding: "0 24px",
      }}
    >
      <Nav view={view} setView={setView} auth={authUi} />

      <div
        style={{
          padding: "52px 0 40px",
          borderBottom: "0.5px solid var(--color-border-tertiary)",
        }}
      >
        <div
          style={{
            display: "inline-flex",
            alignItems: "center",
            gap: 8,
            background: "var(--color-background-secondary)",
            border: "0.5px solid var(--color-border-tertiary)",
            borderRadius: 20,
            padding: "4px 12px",
            marginBottom: 20,
            fontSize: 12,
            color: "var(--color-text-secondary)",
          }}
        >
          <span
            style={{
              width: 6,
              height: 6,
              borderRadius: "50%",
              background: "#3B6D11",
              display: "inline-block",
            }}
          />
          VM online · tritc v2.1 · Phase B+C complete
        </div>
        <h1
          style={{
            fontSize: 36,
            fontWeight: 500,
            margin: "0 0 14px",
            fontFamily: "var(--font-mono)",
            letterSpacing: "-0.02em",
            lineHeight: 1.1,
          }}
        >
          Competitive programming
          <br />
          for the Triton-27 ISA.
        </h1>
        <p
          style={{
            fontSize: 15,
            color: "var(--color-text-secondary)",
            margin: "0 0 6px",
            lineHeight: 1.7,
            maxWidth: 580,
          }}
        >
          Solve data structure and algorithm problems in native TCL 1.0. Every
          solution compiles to TASM and runs in the ternary VM. The best
          algorithms ship in {mono("ulib.trit")}.
        </p>
        <p
          style={{
            fontSize: 13,
            color: "var(--color-text-secondary)",
            margin: "0 0 28px",
            lineHeight: 1.7,
            maxWidth: 560,
          }}
        >
          Three-valued logic. Ownership types. Balanced arithmetic.
          Hardware-native concurrency primitives. No binary shortcuts.
        </p>
        <div style={{ display: "flex", gap: 10 }}>
          <button
            onClick={() => setView("problems")}
            style={{
              padding: "9px 22px",
              borderRadius: 6,
              border: "0.5px solid var(--color-border-primary)",
              background: "var(--color-background-primary)",
              cursor: "pointer",
              fontSize: 13,
              fontWeight: 500,
            }}
          >
            Browse Problems →
          </button>
          <button
            onClick={() => openProblem(CHALLENGES[0])}
            style={{
              padding: "9px 22px",
              borderRadius: 6,
              border: "0.5px solid var(--color-border-tertiary)",
              background: "transparent",
              cursor: "pointer",
              fontSize: 13,
              color: "var(--color-text-secondary)",
            }}
          >
            Try T001 — Sign Test
          </button>
        </div>
      </div>

      <div
        style={{
          display: "grid",
          gridTemplateColumns: "repeat(4, 1fr)",
          gap: 10,
          padding: "28px 0 0",
        }}
      >
        {[
          ["Problems", CHALLENGES.length],
          ["Active Solvers", "847"],
          ["ulib Functions", "47"],
          ["Open Proposals", "4"],
        ].map(([k, v]) => (
          <div
            key={k as string}
            style={{
              background: "var(--color-background-secondary)",
              borderRadius: 8,
              padding: "14px 18px",
              border: "0.5px solid var(--color-border-tertiary)",
            }}
          >
            <div
              style={{
                fontSize: 10,
                color: "var(--color-text-secondary)",
                marginBottom: 5,
                textTransform: "uppercase",
                letterSpacing: "0.08em",
              }}
            >
              {k as string}
            </div>
            <div style={{ fontSize: 26, fontFamily: "var(--font-mono)", fontWeight: 500 }}>
              {v}
            </div>
          </div>
        ))}
      </div>

      <div
        style={{
          display: "grid",
          gridTemplateColumns: "1fr 320px",
          gap: 24,
          padding: "28px 0 40px",
        }}
      >
        <div>
          <div
            style={{
              display: "flex",
              justifyContent: "space-between",
              alignItems: "center",
              marginBottom: 12,
            }}
          >
            <div style={{ fontSize: 13, fontWeight: 500 }}>Featured Problems</div>
            <span
              onClick={() => setView("problems")}
              style={{
                fontSize: 12,
                color: "var(--color-text-secondary)",
                cursor: "pointer",
              }}
            >
              all problems →
            </span>
          </div>
          <div
            style={{
              border: "0.5px solid var(--color-border-tertiary)",
              borderRadius: 8,
              overflow: "hidden",
            }}
          >
            {CHALLENGES.slice(0, 7).map((p, i) => (
              <div
                key={p.id}
                onClick={() => openProblem(p)}
                style={{
                  padding: "11px 14px",
                  borderBottom: i < 6 ? "0.5px solid var(--color-border-tertiary)" : "none",
                  display: "flex",
                  alignItems: "center",
                  gap: 10,
                  cursor: "pointer",
                  background:
                    i % 2 === 0
                      ? "var(--color-background-primary)"
                      : "var(--color-background-secondary)",
                }}
              >
                <span
                  style={{
                    fontSize: 12,
                    color: p.solved ? "#3B6D11" : "var(--color-border-secondary)",
                    minWidth: 14,
                  }}
                >
                  {p.solved ? "✓" : "○"}
                </span>
                <span
                  style={{
                    fontFamily: "var(--font-mono)",
                    fontSize: 11,
                    color: "var(--color-text-secondary)",
                    minWidth: 46,
                  }}
                >
                  {p.id}
                </span>
                <span style={{ fontSize: 13, flex: 1 }}>{p.title}</span>
                <span
                  style={{
                    fontSize: 11,
                    color: "var(--color-text-secondary)",
                    minWidth: 32,
                    textAlign: "right",
                  }}
                >
                  {p.acceptance}%
                </span>
                {diff(p.difficulty)}
              </div>
            ))}
          </div>
        </div>

        <div style={{ display: "flex", flexDirection: "column", gap: 20 }}>
          <div>
            <div
              style={{
                display: "flex",
                justifyContent: "space-between",
                alignItems: "center",
                marginBottom: 12,
              }}
            >
              <div style={{ fontSize: 13, fontWeight: 500 }}>Top Solvers</div>
              <span
                onClick={() => setView("leaderboard")}
                style={{
                  fontSize: 12,
                  color: "var(--color-text-secondary)",
                  cursor: "pointer",
                }}
              >
                full board →
              </span>
            </div>
            <div
              style={{
                border: "0.5px solid var(--color-border-tertiary)",
                borderRadius: 8,
                overflow: "hidden",
              }}
            >
              {dynamicLeaderboard.slice(0, 5).map((u, i) => {
                return (
                  <div
                    key={u.id}
                    style={{
                      padding: "9px 14px",
                      borderBottom:
                        i < 4 ? "0.5px solid var(--color-border-tertiary)" : "none",
                      display: "flex",
                      alignItems: "center",
                      gap: 8,
                      background:
                        i % 2 === 0
                          ? "var(--color-background-primary)"
                          : "var(--color-background-secondary)",
                    }}
                  >
                    <span
                      style={{
                        fontFamily: "var(--font-mono)",
                        fontSize: 11,
                        color: "var(--color-text-secondary)",
                        minWidth: 22,
                      }}
                    >
                      #{i + 1}
                    </span>
                    <span
                      style={{
                        fontFamily: "var(--font-mono)",
                        fontSize: 12,
                        flex: 1,
                        fontWeight: 500,
                      }}
                    >
                      {u.name}
                    </span>
                    <span
                      style={{
                        fontFamily: "var(--font-mono)",
                        fontSize: 11,
                        color: "var(--color-text-secondary)",
                      }}
                    >
                      {u.cycles.toLocaleString()}
                    </span>
                  </div>
                );
              })}
            </div>
          </div>

          <div>
            <div style={{ fontSize: 13, fontWeight: 500, marginBottom: 12 }}>
              Categories
            </div>
            <div style={{ display: "flex", flexDirection: "column", gap: 5 }}>
              {CATEGORIES.filter((c) => c.id !== "all").map((c) => {
                const total = CHALLENGES.filter((p) => p.category === c.id).length;
                const solved = CHALLENGES.filter((p) => p.category === c.id && p.lifecycle === "published" && p.solved).length;
                const pct = Math.round((solved / total) * 100);
                return (
                  <div
                    key={c.id}
                    onClick={() => {
                      setCatFilter(c.id);
                      setView("problems");
                    }}
                    style={{
                      padding: "8px 12px",
                      borderRadius: 6,
                      border: "0.5px solid var(--color-border-tertiary)",
                      cursor: "pointer",
                      background: "var(--color-background-primary)",
                    }}
                  >
                    <div
                      style={{
                        display: "flex",
                        justifyContent: "space-between",
                        alignItems: "center",
                        marginBottom: 5,
                      }}
                    >
                      <span style={{ fontSize: 12 }}>{c.label}</span>
                      <span
                        style={{
                          fontFamily: "var(--font-mono)",
                          fontSize: 11,
                          color: "var(--color-text-secondary)",
                        }}
                      >
                        {solved}/{total}
                      </span>
                    </div>
                    <div
                      style={{
                        height: 3,
                        background: "var(--color-border-tertiary)",
                        borderRadius: 2,
                      }}
                    >
                      <div
                        style={{
                          height: 3,
                          width: pct + "%",
                          background: "#3B6D11",
                          borderRadius: 2,
                        }}
                      />
                    </div>
                  </div>
                );
              })}
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}

// ── CUSTOM MARKDOWN RENDERERS FOR THE GUIDE ──────────────────────────────────
function LearningInteractiveModule({
  module,
  onOpenChallenges,
}: {
  module: LearningInteractive;
  onOpenChallenges: () => void;
}) {
  const [selected, setSelected] = useState<number | null>(null);
  const [feedback, setFeedback] = useState("");
  const [code, setCode] = useState(module.kind === "code" ? module.starter : "");

  const panelStyle = {
    margin: "0 0 28px",
    padding: 16,
    border: "1px solid var(--color-border-secondary)",
    borderRadius: 8,
    background: "var(--color-background-secondary)",
  } as const;

  if (module.kind === "choice") {
    return (
      <section style={panelStyle} aria-labelledby="learning-check-title">
        <div
          style={{
            fontSize: 11,
            fontWeight: 600,
            textTransform: "uppercase",
            letterSpacing: "0.05em",
            color: "var(--color-text-muted)",
            marginBottom: 6,
          }}
        >
          Interactive module
        </div>
        <h2
          id="learning-check-title"
          style={{ fontSize: 17, margin: "0 0 10px", fontWeight: 600 }}
        >
          {module.title}
        </h2>
        <fieldset style={{ border: 0, padding: 0, margin: 0 }}>
          <legend
            style={{
              fontSize: 14,
              lineHeight: 1.5,
              color: "var(--color-text-primary)",
              marginBottom: 10,
            }}
          >
            {module.prompt}
          </legend>
          <div style={{ display: "flex", flexDirection: "column", gap: 7 }}>
            {module.options.map((option, index) => (
              <label
                key={option}
                style={{
                  display: "flex",
                  alignItems: "center",
                  gap: 8,
                  fontSize: 13,
                  color: "var(--color-text-secondary)",
                  cursor: "pointer",
                }}
              >
                <input
                  type="radio"
                  name={`learning-${module.title}`}
                  checked={selected === index}
                  aria-checked={selected === index}
                  onChange={() => {
                    setSelected(index);
                    setFeedback("");
                  }}
                />
                {option}
              </label>
            ))}
          </div>
        </fieldset>
        <button
          type="button"
          disabled={selected === null}
          onClick={() => {
            if (selected === null) return;
            setFeedback(
              selected === module.answer
                ? `Correct. ${module.explanation}`
                : "Not yet. Re-read the representation boundary and try again."
            );
          }}
          style={{
            marginTop: 14,
            padding: "7px 12px",
            borderRadius: 5,
            border: "1px solid var(--color-border-secondary)",
            background: "var(--color-background-primary)",
            color: "var(--color-text-primary)",
            cursor: selected === null ? "not-allowed" : "pointer",
            fontSize: 12,
          }}
        >
          Check answer
        </button>
        <p
          aria-live="polite"
          style={{
            minHeight: 20,
            margin: "10px 0 0",
            fontSize: 12,
            lineHeight: 1.5,
            color: "var(--color-text-secondary)",
          }}
        >
          {feedback}
        </p>
      </section>
    );
  }

  const validateCode = () => {
    const missing = module.expectedIncludes.filter((fragment) => !code.includes(fragment));
    setFeedback(
      missing.length === 0
        ? `Example shape verified. ${module.explanation}`
        : `Add the expected TCL shape: ${missing.join(", ")}.`
    );
  };

  return (
    <section style={panelStyle} aria-labelledby="learning-check-title">
      <div
        style={{
          fontSize: 11,
          fontWeight: 600,
          textTransform: "uppercase",
          letterSpacing: "0.05em",
          color: "var(--color-text-muted)",
          marginBottom: 6,
        }}
      >
        Interactive module
      </div>
      <h2
        id="learning-check-title"
        style={{ fontSize: 17, margin: "0 0 10px", fontWeight: 600 }}
      >
        {module.title}
      </h2>
      <label
        htmlFor="tcl-practice-code"
        style={{
          display: "block",
          fontSize: 12,
          color: "var(--color-text-secondary)",
          marginBottom: 6,
        }}
      >
        Edit the example, then validate its shape before opening a real challenge.
      </label>
      <textarea
        id="tcl-practice-code"
        aria-label="TCL practice code"
        value={code}
        onChange={(event) => setCode(event.target.value)}
        spellCheck={false}
        style={{
          display: "block",
          width: "100%",
          minHeight: 150,
          resize: "vertical",
          padding: 12,
          border: "1px solid var(--color-border-secondary)",
          borderRadius: 6,
          background: "var(--color-background-primary)",
          color: "var(--color-text-primary)",
          fontFamily: "var(--font-mono)",
          fontSize: 13,
          lineHeight: 1.5,
        }}
      />
      <div style={{ display: "flex", flexWrap: "wrap", gap: 8, marginTop: 12 }}>
        <button
          type="button"
          onClick={validateCode}
          style={{
            padding: "7px 12px",
            borderRadius: 5,
            border: "1px solid var(--color-border-primary)",
            background: "var(--color-background-primary)",
            color: "var(--color-text-primary)",
            cursor: "pointer",
            fontSize: 12,
            fontWeight: 500,
          }}
        >
          Validate example
        </button>
        <button
          type="button"
          onClick={onOpenChallenges}
          style={{
            padding: "7px 12px",
            borderRadius: 5,
            border: "1px solid var(--color-border-secondary)",
            background: "transparent",
            color: "var(--color-text-secondary)",
            cursor: "pointer",
            fontSize: 12,
          }}
        >
          Open challenges
        </button>
      </div>
      <p
        aria-live="polite"
        style={{
          minHeight: 20,
          margin: "10px 0 0",
          fontSize: 12,
          lineHeight: 1.5,
          color: "var(--color-text-secondary)",
        }}
      >
        {feedback}
      </p>
    </section>
  );
}

function repositoryHref(repositoryPath: string): string {
  return `https://github.com/JonasComlita/dualrail/blob/main/${repositoryPath}`;
}

function LearningProvenance({ page }: { page: LearningPage }) {
  const renderReferences = (title: string, references: LearningPage["sources"]) => (
    <div style={{ flex: "1 1 260px", minWidth: 0 }}>
      <h3
        style={{
          fontSize: 12,
          textTransform: "uppercase",
          letterSpacing: "0.05em",
          color: "var(--color-text-muted)",
          margin: "0 0 8px",
        }}
      >
        {title}
      </h3>
      <ul
        style={{
          listStyle: "none",
          display: "flex",
          flexDirection: "column",
          gap: 7,
          margin: 0,
          padding: 0,
        }}
      >
        {references.map((reference) => (
          <li key={reference.path} style={{ fontSize: 12, lineHeight: 1.4 }}>
            <a
              href={repositoryHref(reference.path)}
              target="_blank"
              rel="noreferrer"
              style={{ color: "var(--color-accent-blue)" }}
            >
              {reference.label}
            </a>
            <code
              style={{
                display: "block",
                marginTop: 2,
                color: "var(--color-text-muted)",
                fontSize: 10,
              }}
            >
              {reference.path}
            </code>
          </li>
        ))}
      </ul>
    </div>
  );

  return (
    <section
      aria-labelledby="learning-provenance-title"
      style={{
        margin: "0 0 28px",
        padding: "16px 0 4px",
        borderTop: "1px solid var(--color-border-tertiary)",
      }}
    >
      <h2
        id="learning-provenance-title"
        style={{ fontSize: 17, margin: "0 0 12px", fontWeight: 600 }}
      >
        Source and evidence
      </h2>
      <p
        style={{
          margin: "0 0 16px",
          fontSize: 12,
          lineHeight: 1.5,
          color: "var(--color-text-secondary)",
        }}
      >
        These links are the authority for this page. The guide summarizes them;
        it does not replace the implementation or its tests.
      </p>
      <div style={{ display: "flex", flexWrap: "wrap", gap: 24 }}>
        {renderReferences("Production source", page.sources)}
        {renderReferences("Validation evidence", page.evidence)}
      </div>
    </section>
  );
}

function renderInline(text: string): React.ReactNode[] {
  const parts: React.ReactNode[] = [];
  let remaining = text;
  let keyIdx = 0;

  while (remaining.length > 0) {
    const linkMatch = remaining.match(/^\[([^\]]+)\]\(([^)]+)\)/);
    if (linkMatch) {
      const external = /^(https?:|mailto:)/.test(linkMatch[2]);
      parts.push(
        <a
          key={keyIdx++}
          href={linkMatch[2]}
          target={external ? "_blank" : undefined}
          rel={external ? "noreferrer" : undefined}
          style={{ color: "var(--color-accent-blue)" }}
        >
          {linkMatch[1]}
        </a>
      );
      remaining = remaining.substring(linkMatch[0].length);
      continue;
    }

    // Check if next is inline code: `code`
    const codeMatch = remaining.match(/^`([^`]+)`/);
    if (codeMatch) {
      parts.push(
        <code
          key={keyIdx++}
          style={{
            fontFamily: "var(--font-mono)",
            background: "var(--color-background-tertiary)",
            padding: "1px 4px",
            borderRadius: 3,
            fontSize: "0.9em",
            border: "0.5px solid var(--color-border-secondary)",
            color: "var(--color-text-primary)",
          }}
        >
          {codeMatch[1]}
        </code>
      );
      remaining = remaining.substring(codeMatch[0].length);
      continue;
    }

    // Check if next is bold: **bold**
    const boldMatch = remaining.match(/^\*\*([^*]+)\*\*/);
    if (boldMatch) {
      parts.push(
        <strong key={keyIdx++} style={{ fontWeight: 600, color: "var(--color-text-primary)" }}>
          {boldMatch[1]}
        </strong>
      );
      remaining = remaining.substring(boldMatch[0].length);
      continue;
    }

    // Check if next is math: $math$
    const mathMatch = remaining.match(/^\$([^$]+)\$/);
    if (mathMatch) {
      parts.push(
        <span
          key={keyIdx++}
          style={{
            fontFamily: "var(--font-mono)",
            fontStyle: "italic",
            color: "var(--color-accent-purple)",
            background: "var(--color-background-secondary)",
            padding: "0 4px",
            borderRadius: 2,
          }}
        >
          {mathMatch[1]}
        </span>
      );
      remaining = remaining.substring(mathMatch[0].length);
      continue;
    }

    // Standard text: parse until next token identifier
    const nextTokenIdx = remaining.search(/[\[`$]|\*\*/);
    if (nextTokenIdx === -1) {
      parts.push(remaining);
      break;
    } else if (nextTokenIdx === 0) {
      parts.push(remaining[0]);
      remaining = remaining.substring(1);
    } else {
      parts.push(remaining.substring(0, nextTokenIdx));
      remaining = remaining.substring(nextTokenIdx);
    }
  }

  return parts;
}

function renderMarkdown(content: string): React.ReactNode {
  const blocks = parseMarkdown(content);
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 16 }}>
      {blocks.map((block, blockIdx) => {
        switch (block.type) {
          case "p":
            return (
              <p
                key={blockIdx}
                style={{
                  fontSize: 14,
                  lineHeight: 1.6,
                  color: "var(--color-text-secondary)",
                  margin: 0,
                }}
              >
                {renderInline(block.content)}
              </p>
            );
          case "h2":
            return (
              <h2
                key={blockIdx}
                style={{
                  fontSize: 19,
                  fontWeight: 600,
                  color: "var(--color-text-primary)",
                  marginTop: 24,
                  marginBottom: 8,
                  borderBottom: "1px solid var(--color-border-tertiary)",
                  paddingBottom: 6,
                }}
              >
                {renderInline(block.content)}
              </h2>
            );
          case "h3":
            return (
              <h3
                key={blockIdx}
                style={{
                  fontSize: 18,
                  fontWeight: 600,
                  color: "var(--color-text-primary)",
                  marginTop: 24,
                  marginBottom: 8,
                  borderBottom: "1px solid var(--color-border-tertiary)",
                  paddingBottom: 6,
                }}
              >
                {renderInline(block.content)}
              </h3>
            );
          case "h4":
            return (
              <h4
                key={blockIdx}
                style={{
                  fontSize: 15,
                  fontWeight: 600,
                  color: "var(--color-text-primary)",
                  marginTop: 16,
                  marginBottom: 6,
                }}
              >
                {renderInline(block.content)}
              </h4>
            );
          case "ul":
            return (
              <ul
                key={blockIdx}
                style={{
                  paddingLeft: 20,
                  margin: 0,
                  display: "flex",
                  flexDirection: "column",
                  gap: 6,
                }}
              >
                {block.items?.map((item, idx) => (
                  <li
                    key={idx}
                    style={{
                      fontSize: 14,
                      lineHeight: 1.6,
                      color: "var(--color-text-secondary)",
                    }}
                  >
                    {renderInline(item)}
                  </li>
                ))}
              </ul>
            );
          case "ol":
            return (
              <ol
                key={blockIdx}
                style={{
                  paddingLeft: 20,
                  margin: 0,
                  display: "flex",
                  flexDirection: "column",
                  gap: 6,
                }}
              >
                {block.items?.map((item, idx) => (
                  <li
                    key={idx}
                    style={{
                      fontSize: 14,
                      lineHeight: 1.6,
                      color: "var(--color-text-secondary)",
                    }}
                  >
                    {renderInline(item)}
                  </li>
                ))}
              </ol>
            );
          case "table":
            return (
              <div
                key={blockIdx}
                style={{
                  overflowX: "auto",
                  margin: "8px 0 16px",
                  border: "0.5px solid var(--color-border-secondary)",
                  borderRadius: 6,
                }}
              >
                <table
                  style={{
                    width: "100%",
                    borderCollapse: "collapse",
                    fontSize: 13,
                    textAlign: "left",
                  }}
                >
                  <thead>
                    <tr
                      style={{
                        background: "var(--color-background-tertiary)",
                        borderBottom: "0.5px solid var(--color-border-secondary)",
                      }}
                    >
                      {block.headers?.map((h, idx) => (
                        <th
                          key={idx}
                          style={{
                            padding: "8px 12px",
                            fontWeight: 600,
                            color: "var(--color-text-primary)",
                          }}
                        >
                          {h}
                        </th>
                      ))}
                    </tr>
                  </thead>
                  <tbody>
                    {block.rows?.map((row, rowIdx) => (
                      <tr
                        key={rowIdx}
                        style={{
                          borderBottom:
                            rowIdx === (block.rows?.length || 0) - 1
                              ? "none"
                              : "0.5px solid var(--color-border-tertiary)",
                          background:
                            rowIdx % 2 === 0
                              ? "transparent"
                              : "var(--color-background-secondary)",
                        }}
                      >
                        {row.map((cell, cellIdx) => (
                          <td
                            key={cellIdx}
                            style={{
                              padding: "8px 12px",
                              color: "var(--color-text-secondary)",
                            }}
                          >
                            {renderInline(cell)}
                          </td>
                        ))}
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            );
          case "code":
            return (
              <div
                key={blockIdx}
                style={{
                  position: "relative",
                  margin: "12px 0 16px",
                  border: "1px solid var(--color-border-secondary)",
                  borderRadius: 6,
                  overflow: "hidden",
                  background: "var(--color-background-secondary)",
                }}
              >
                <div
                  style={{
                    display: "flex",
                    justifyContent: "space-between",
                    alignItems: "center",
                    padding: "6px 12px",
                    background: "var(--color-background-tertiary)",
                    borderBottom: "1px solid var(--color-border-secondary)",
                    fontSize: 11,
                    fontFamily: "var(--font-mono)",
                    color: "var(--color-text-secondary)",
                    textTransform: "uppercase",
                  }}
                >
                  <span>{block.lang || "code"}</span>
                  <button
                    onClick={() => {
                      navigator.clipboard.writeText(block.content);
                    }}
                    style={{
                      background: "none",
                      border: "none",
                      cursor: "pointer",
                      fontSize: 11,
                      color: "var(--color-accent-blue)",
                      fontWeight: 500,
                      padding: "2px 6px",
                    }}
                  >
                    Copy
                  </button>
                </div>
                <pre
                  style={{
                    margin: 0,
                    padding: 12,
                    overflowX: "auto",
                    fontSize: 13,
                    lineHeight: 1.5,
                    fontFamily: "var(--font-mono)",
                    color: "var(--color-text-primary)",
                    background: "var(--color-background-secondary)",
                  }}
                >
                  <code>{block.content}</code>
                </pre>
              </div>
            );
          default:
            return null;
        }
      })}
    </div>
  );
}
