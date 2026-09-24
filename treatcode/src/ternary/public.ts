import { DEFAULT_LIMITS, PILOT_LABEL, type PublicExample } from "./contracts";
export const METHODOLOGY = {
  label: PILOT_LABEL,
  scope: "A 36-family pilot of reasoning and engineering under explicitly specified ternary semantics. Difficulty labels are provisional until live calibration.",
  conditions: [
    { id: "prior", name: "Prior knowledge", description: "Complete task requirements with minimal instructional scaffolding." },
    { id: "specification", name: "Provided specification", description: "The same requirements plus a standardized language and execution reference." },
    { id: "learning", name: "Learning episode", description: "Disjoint transfer probes before feedback and after rounds one and three. Context is retained within an episode and reset between episodes." },
  ],
  experiments: [
    { id: "default", name: "Full pilot", families: 36, episodesPerModel: 36, description: "Provided specification with structured tools, once per family." },
    { id: "controlled", name: "Controlled experiment", families: 12, episodesPerModel: 72, description: "Two preselected families per capability, all three conditions, both tool modes." },
  ],
  scoring: "Episode correctness is binary. Explanatory prose and API spending do not change correctness. Learning, robustness, resource use, program execution time, and model latency are separate observations.",
  uncertainty: "95% bootstrap intervals use 10,000 deterministic samples with task family as the resampling unit. Comparisons use paired family samples, matching profiles and task coverage. The pilot does not produce IQ scores.",
  failures: "Wrong answers, refusals, invalid submissions, and exhausted task budgets count as model failures. Transport failures, interrupted exposed episodes, and worker failures remain separate and leave coverage incomplete.",
  limits: DEFAULT_LIMITS,
  execution: "One active episode at a time. Windows Job Objects constrain compiler and VM processes. Structured tools allow participant-file access and named public tests. This pilot uses Windows-native execution and does not claim container-level isolation.",
  calibration: "The initial lineup is Astra, Sol, Terra, and Luna in the local Codex harness, each at high reasoning effort. Each receives three fresh attempts per family: 432 default episodes and 864 controlled episodes across the four configurations. More than 25% universally passed or failed families requires revision and recalibration. Public examples and calibration-exposed tasks cannot become a future unseen evaluation set.",
  harness: "Harness and direct API runs use separate protocol profiles. Harness episodes have fresh, environment-free sessions and submit structured JSON actions to the benchmark host. Token usage is measured after each turn; over-budget answers fail. Subscription cost and advance input-token counts are unavailable. Harness errors or reported retries leave coverage incomplete. Effective settings and the harness version are recorded.",
  privacy: "Task prompts, hidden cases, submitted files, and traces are operator-only. Publications include aggregate observations, coverage, configuration, environment, and uncertainty. Practice activity is excluded.",
  references: [
    { title: "Codex App Server", url: "https://learn.chatgpt.com/docs/app-server" },
    { title: "OpenAI Responses function calling", url: "https://developers.openai.com/api/docs/guides/function-calling" },
    { title: "Anthropic Messages", url: "https://platform.claude.com/docs/en/build-with-claude/working-with-messages" },
    { title: "Gemini function calling", url: "https://ai.google.dev/gemini-api/docs/function-calling" },
    { title: "Windows Job Objects", url: "https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects" },
  ],
};
export const EXAMPLES: PublicExample[] = [
  { id:"addition",capability:"representation",title:"A carry you can see",kind:"arithmetic",instructions:"Enter two integers between -13 and 13. Add them in a signed three-trit word. T denotes -1; carry is shown separately.",input:'{"a": 11, "b": 7}',workedSolution:"11 + 7 = 18. A three-trit word holds -13 through 13, so carry +1 into the next word: 18 = 27 - 9. The low word is T00 and the carry is +1." },
  { id:"consensus",capability:"logic",title:"What does unknown tell us?",kind:"logic",instructions:"Enter three votes (-1=false, 0=unknown, 1=true). A decision is true when at least two votes are true, false when at least two are false, and unknown otherwise.",input:'{"votes": [1, 0, -1]}',workedSolution:"With one true, one false, and one unknown vote, neither side has a majority. The result remains unknown (0). Unknown is not treated as false." },
  { id:"orbit",capability:"learning",title:"Learn the orbit rule",kind:"rules",instructions:"The cycle is moss → amber → violet → moss. Examples: step(moss,1)=amber; step(amber,2)=moss; step(violet,-1)=amber. Infer step(violet,4), or try another color and step count.",input:'{"color": "violet", "steps": 4}',workedSolution:"Reduce the step count modulo 3. Four steps move one position from violet to moss. Negative steps move backward." },
  { id:"median",capability:"algorithms",title:"Order three observations",kind:"code",instructions:"Implement the median of three integer observations. Edit the source or download it for your local Trit compiler. The output below is a recorded example, not execution of your edits.",input:"Inputs: -4, 8, 2",source:"fn median(a:t40,b:t40,c:t40)->t40 { var x:t40=a; var y:t40=b; var z:t40=c; if(x>y){var t:t40=x;x=y;y=t;}if(y>z){var t:t40=y;y=z;z=t;}if(x>y){y=x;}return y; }\nfn main()->t40{return median(0-4,8,2);}",workedSolution:"Order the first pair, then the last pair. One final comparison between the first two positions identifies the middle value, 2.",recordedOutput:"Return Register r13: 2" },
  { id:"clamp",capability:"debugging",title:"Repair both boundaries",kind:"code",instructions:"The starter caps positive values but misses the lower bound. Repair clamp so its output is in [-4,4]. Download and run your source locally.",input:"Input: -8",source:"fn clamp(x:t40)->t40 { if(x>4){return 4;} return x; }\nfn main()->t40{return clamp(0-8);}",workedSolution:"Add `if(x<0-4){return 0-4;}` before returning x. The supplied starter returns -8; the repaired version returns -4.",recordedOutput:"Starter: -8 · repaired reference: -4" },
  { id:"alignment",capability:"systems",title:"Align a guest allocation",kind:"code",instructions:"Round nonnegative guest addresses upward to a 9-trit boundary. The function uses bounded iteration and has no host memory access.",input:"Address: 14",source:"fn align9(x:t40)->t40 { var p:t40=0; while(p<x){p=p+9;}return p; }\nfn main()->t40{return align9(14);}",workedSolution:"The 9-trit boundaries around 14 are 9 and 18. An upward alignment chooses 18; an already aligned address is unchanged.",recordedOutput:"Return Register r13: 18" },
];
