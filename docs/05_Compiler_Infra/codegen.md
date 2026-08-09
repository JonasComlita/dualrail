# Code generation and lowering

| Status | Last verified | Authority |
| :--- | :--- | :--- |
| Implemented, with fail-closed aggregate/vector gaps | 2026-08-08 | `ternary_compiler_codegen.h` |

The compiler does not emit assembly by replaying the AST. The authoritative
target pipeline is:

1. Typed AST to address-based CFG IR.
2. Predecessor/successor construction, dominators, and dominance frontiers.
3. `mem2reg` with explicit `(predecessor, value)` phi inputs.
4. SSA, type, effect, and dominance verification.
5. Global optimization.
6. Phi and wide-value lowering.
7. CFG-wide liveness, graph coloring/coalescing, and iterative spill rewrite.
8. Target selection and v2 assembly/object emission.

Target metadata reports `target.ast_replay_functions = 0`. If the target cannot
prove a lowering correct, compilation rejects that function; there is no
production AST-assembly fallback.

## Global optimization

The implemented portfolio includes sparse conditional constant propagation,
global value numbering/common-subexpression elimination, dead-code and copy
elimination, loop-invariant motion, induction simplification, branch folding,
and cost-controlled `TSEL` conversion. Promotion is limited to non-escaping
scalar storage; aliased, atomic, unsafe-pointer, and unsupported aggregate
storage remains explicit memory.

The allocator uses separate target constraints, call-clobber interference,
loop-weighted spill costs, simplify/coalesce/freeze/spill decisions, and
repeated spill rewriting until colorable or explicitly rejected. Wide T50
values retain pair constraints through ABI and target lowering.

## Quantitative gate

`test_compiler_corpus_gate` compares unoptimized and default SSA pipelines on
loops/phis, calls, spills, aggregates, ownership, atomics, branches, and alias
cases. Candidate `45539f7` passed all correctness/determinism checks with a
23.29% median dynamic-instruction reduction and 0% maximum workload regression,
exceeding the 15% / 5% acceptance contract.

## ABI and remaining fail-closed cases

Struct and array parameters cross function ABI v2 as one-word caller-owned
addresses. Their ABI word indices are shared by frontend IR, caller lowering,
callee lowering, register arguments, and outgoing-stack arguments; the focused
ABI contract and deterministic corpus execute an eight-word mixed aggregate
call in both optimized and unoptimized modes. Live raw addresses across calls
are also verified to remain in callee-saved storage while memory effects stay
ordered.

ABI v2 defines only scalar/T50 returns and does not define a vector register
call convention. Aggregate-valued returns and first-class vector function
boundaries therefore fail closed with explicit diagnostics. Supporting either
requires a versioned public ABI decision (such as an sret contract or vector
argument/return registers), not an emitter-local convention or hidden AST
replay. See [Known Gaps](../../KNOWN_GAPS.md).
