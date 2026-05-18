This is fundamentally a **dependency graph problem** dressed as a planning problem. The compiler-before-substrate mistake happened because the plan was written as a linear list when the actual structure is a DAG with hard precedence constraints. The fix is to make the dependency graph explicit and assign agents to monitor specific graph properties rather than specific features.

---

## The Core Architecture: Three Layers of Agents

### Layer 1 — The Dependency Auditor

One agent whose only job is maintaining a live dependency graph of every artifact in the project. Not a Gantt chart, a literal directed graph where an edge from A to B means "B cannot be correct without A being correct first."

For this project it would look like:

```
ISA spec
  → VM executor
    → tasm assembler
      → kernel.tasm (proves the substrate)
        → syscall ABI (frozen here)
          → ulib.trit
            → user_app.trit
  → register allocator
    → compiler codegen (blocked until allocator wired)
      → native kernel tracks
```

The auditor's job is to **reject any plan that proposes work on a node before its parents are green**. The compiler-before-substrate mistake would have been caught here — the native kernel tracks have `compiler codegen` as a parent, which has `register allocator wired` as a parent, which wasn't green.

The auditor also flags **orphaned work** — code that has no downstream dependents, which is where boilerplate accumulates. The `language_optimizations.md` document had design for features (trit-width polymorphism, `shared<T>` types) with no corresponding compiler nodes depending on them. That's an orphan subgraph and a signal that the work is premature.

---

### Layer 2 — The Leverage Ranker

A separate agent that continuously scores every proposed task on two axes:

**Unblocking power** — how many currently-blocked nodes does completing this task ungreen? The register allocator fix has high unblocking power because five or six downstream tracks are waiting on it. Writing an LRU block cache has low unblocking power because nothing is blocked on it.

**Reversibility cost** — how expensive is it to undo this decision if it turns out to be wrong? Freezing the syscall ABI has very high reversal cost (everything downstream must change). Choosing variable names has zero reversal cost. The agent should flag any high-reversal-cost decision that is being made before its dependencies are fully stable.

This is where your RISC vs ARM example lives. RISC (load/store, fixed-width instructions, orthogonal register file) is the correct choice here not because of ideology but because:
- Fixed 27-trit instruction words make FPGA decode trivial — reversal cost of changing this after FPGA work begins is enormous
- Load/store means the compiler's register allocator only needs to reason about one memory interface — reducing implementation complexity at a high-leverage point
- The choice was made early when reversal cost was low, which is exactly right

An agent monitoring reversal cost would have flagged the instruction encoding as a decision to make and lock *before* writing the assembler, not after.

---

### Layer 3 — The Consistency Verifier

This agent operates as a continuous cross-reference checker between artifacts. It holds a set of **contracts** — explicit claims one artifact makes about another — and checks that they hold bidirectionally.

Contracts in this project include things like:

| Claim | Made by | Must be verified against |
|---|---|---|
| Syscall IDs 1-21 are stable | `TERNARY_ARCHITECTURE_CONTRACTS.md` | `minimal_kernel_bringup.tasm`, `ternary_compiler.h` runtime namespace, `ulib.trit` |
| Page size is 27 words | Architecture contracts | PTE layout, MMU_PAGE_WORDS constant, `.org` spacing in tasm |
| TSTR returns T1 status {-1,0,+1} | ISA spec | Every use of TSTR in kernel and compiler |
| Stack grows downward | ABI doc | Every function prologue emitted by compiler |

The verifier's job is to **alert when a contract is asserted in one place but violated or unverified in another**. The `tldr`-used-as-load bug was a contract violation — `tldr` has a documented reservation side effect in the ISA spec, and `ulib.trit` was using it in a context where that side effect is incorrect. A contract verifier watching "every use of TLDR must have a corresponding TSTR in the same reservation scope" would have caught it.

---

## How They Communicate

The three agents feed a single **planning gate**:

```
Proposed task
      ↓
Dependency Auditor: are all parents green?
      ↓ (pass)
Leverage Ranker: is this the highest unblocking-power task available?
      ↓ (pass or override with justification)
Consistency Verifier: does this task introduce any contract violations?
      ↓ (pass)
Approved for implementation
      ↓
Implementation
      ↓
Consistency Verifier: post-implementation contract check
      ↓
Dependency Auditor: mark node green, update downstream
```

The key property is that **no task gets approved without passing all three gates**, and **every completed task immediately triggers re-scoring of the leverage ranker** since the unblocking power of remaining tasks changes when a node goes green.

## The Meta-Principle

The system works because it enforces one rule that linear planning consistently violates: **you cannot know the right order to build things by reading a feature list — you can only know it by reading the dependency graph**. Every planning mistake in this project so far (compiler before substrate, tldr misuse, directory persistence gap) traces back to a plan that was written as "here are the features we want" instead of "here are the constraints between the features." The three-agent architecture makes the constraints the primary artifact and derives the plan from them, rather than the reverse.