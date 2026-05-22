Created At: 2026-05-21T23:44:28Z
Completed At: 2026-05-21T23:44:28Z
File Path: `file:///c:/Users/jonas/Documents/trit/treatcode/app.jsx`
Total Lines: 526
Total Bytes: 40593
Showing lines 1 to 526
The following code has been modified to include a line number before every line, in the format: <line_number>: <original_line>. Please note that any changes targeting the original code should remove the line number, colon, and leading space.
1: import { useState, useMemo } from "react";
2: 
3: const PROBLEMS = [
4:   { id: "T001", title: "Three-Way Sign Test", difficulty: "easy", category: "three-valued", tags: ["T1", "match"], solved: true, submissions: 2841, acceptance: 94, points: 50 },
5:   { id: "T002", title: "Ternary FizzBuzz", difficulty: "easy", category: "three-valued", tags: ["T1", "match", "arithmetic"], solved: true, submissions: 2103, acceptance: 91, points: 50 },
6:   { id: "T003", title: "Three-State Machine", difficulty: "easy", category: "three-valued", tags: ["T1", "enum"], solved: false, submissions: 1654, acceptance: 88, points: 50 },
7:   { id: "T004", title: "Null Pointer Safety Chain", difficulty: "medium", category: "pointer-safety", tags: ["ptr<T,S>", "match"], solved: false, submissions: 892, acceptance: 67, points: 150 },
8:   { id: "T005", title: "Validated Buffer Walk", difficulty: "medium", category: "pointer-safety", tags: ["ptr<T,S>", "borrow"], solved: false, submissions: 743, acceptance: 61, points: 150 },
9:   { id: "T006", title: "Ownership Transfer Chain", difficulty: "hard", category: "pointer-safety", tags: ["own<T>", "move"], solved: false, submissions: 312, acceptance: 42, points: 300 },
10:   { id: "T007", title: "Balanced Ternary Addition", difficulty: "easy", category: "arithmetic", tags: ["T40", "carry"], solved: true, submissions: 3102, acceptance: 89, points: 50 },
11:   { id: "T008", title: "Sign-Free Absolute Value", difficulty: "easy", category: "arithmetic", tags: ["T40", "match"], solved: true, submissions: 2890, acceptance: 92, points: 50 },
12:   { id: "T0
<truncated 39540 bytes>
<span style={{ fontFamily: "var(--font-mono)", fontSize: 11, color: "var(--color-text-secondary)" }}>{u.points.toLocaleString()}</span>
495:                   </div>
496:                 );
497:               })}
498:             </div>
499:           </div>
500: 
501:           <div>
502:             <div style={{ fontSize: 13, fontWeight: 500, marginBottom: 12 }}>Categories</div>
503:             <div style={{ display: "flex", flexDirection: "column", gap: 5 }}>
504:               {CATEGORIES.filter(c => c.id !== "all").map(c => {
505:                 const total = PROBLEMS.filter(p => p.category === c.id).length;
506:                 const solved = PROBLEMS.filter(p => p.category === c.id && p.solved).length;
507:                 const pct = Math.round((solved / total) * 100);
508:                 return (
509:                   <div key={c.id} onClick={() => { setCatFilter(c.id); setView("problems"); }} style={{ padding: "8px 12px", borderRadius: 6, border: "0.5px solid var(--color-border-tertiary)", cursor: "pointer", background: "var(--color-background-primary)" }}>
510:                     <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 5 }}>
511:                       <span style={{ fontSize: 12 }}>{c.label}</span>
512:                       <span style={{ fontFamily: "var(--font-mono)", fontSize: 11, color: "var(--color-text-secondary)" }}>{solved}/{total}</span>
513:                     </div>
514:                     <div style={{ height: 3, background: "var(--color-border-tertiary)", borderRadius: 2 }}>
515:                       <div style={{ height: 3, width: pct + "%", background: "#3B6D11", borderRadius: 2 }} />
516:                     </div>
517:                   </div>
518:                 );
519:               })}
520:             </div>
521:           </div>
522:         </div>
523:       </div>
524:     </div>
525:   );
526: }
The above content shows the entire, complete file contents of the requested file.
