# TCL Compiler and Language Advisory: Ternary Control Flow Ergonomics and Native Data Structures

**To:** Implementing agent responsible for `tcl_frontend.trit`, `tcl_backend.trit`, and `ulib.trit`
**Status:** Normative guidance for TCL 1.1 extensions and `ulib.trit` additions.
**Depends on:** TCL Spec 1.0, Phase A compiler complete, `ulib.trit` base library stable.

---

## 1. The Problem: Exhaustive Match Creates Ergonomic Friction

TCL 1.0 requires all three arms of a `match` to be present. This is correct for
safety — it enforces exhaustive control flow. But it creates friction when a
developer genuinely only cares about one case:

```
// Developer intent: do something only when x is positive.
// TCL 1.0 forces this:
match x {
    neg  => {}      // administrative tax
    zero => {}      // administrative tax
    pos  => { real_work(); }
}
```

The empty arms are not wrong. They are noise. A developer writing kernel code
in a tight loop who has to type this for every guard condition will find ways
to avoid the pattern, producing less safe code.

The fix is syntactic sugar, not a type system change. The exhaustive match
requirement stays. The compiler desugars the sugar into a full match before
type checking. The safety guarantee is preserved.

---

## 2. TCL 1.1 Extension: Single-Arm Guard Syntax

Add three guard statement forms to the parser. These are sugar only — they
produce identical IR to the equivalent full match. No new type rules, no new
IR nodes, no new codegen cases.

### Syntax

```
if pos(expr) { body }
if neg(expr) { body }
if zero(expr) { body }
```

### Desugaring

The parser immediately rewrites these to full match expressions before the
type inferencer runs:

```
// if pos(expr) { body }
// becomes:
match expr {
    neg  => {}
    zero => {}
    pos  => { body }
}
```

```
// if neg(expr) { body }
// becomes:
match expr {
    neg  => { body }
    zero => {}
    pos  => {}
}
```

```
// if zero(expr) { body }
// becomes:
match expr {
    neg  => {}
    zero => { body }
    pos  => {}
}
```

### Chaining

The following chain is permitted and desugars to a single match:

```
if pos(expr)  { pos_body  }
if neg(expr)  { neg_body  }
if zero(expr) { zero_body }
```

The parser collapses consecutive guards on the same expression into one match.
Consecutive guards on different expressions remain separate matches.

### Constraint

`if pos`, `if neg`, `if zero` are statement forms only. They do not produce a
value and cannot appear in expression position. If the developer needs a value
from a conditional, the full `match` is required. This keeps value-producing
control flow explicit and traceable in the IR.

### Implementation Note for the Parser

Add `parse_guard_stmt` to the statement parser. Check for `if` followed by one
of the three keywords. Consume the keyword and the parenthesized expression.
Parse the block. Construct the desugared match AST node. Hand to the type
inferencer as if the programmer wrote the full match.

---

## 3. The TCMP Instruction and Three-Way Partition

The ternary ISA's `TCMP` instruction computes `sign(a - b)` in a single
instruction, producing one of {−1, 0, +1}. In binary architectures, a
three-way partition requires two comparisons per element: one to test `<` and
one to test `>`. `TCMP` does both in one instruction.

This has a real and measurable consequence for sorting algorithms that use
three-way partitioning.

### What the Advantage Actually Is

The advantage is a constant factor, not an asymptotic class change. Be precise
about this when documenting or communicating about it:

- A balanced ternary recursion tree has height log₃(N) instead of log₂(N).
  This is a real reduction in recursion depth.
- Each level of recursion does one `TCMP` per element instead of two binary
  comparisons. This is a real reduction in comparison count.
- The total asymptotic complexity remains O(n log n). The log base does not
  change the complexity class.
- The information-theoretic argument: one `TCMP` produces log₂(3) ≈ 1.58 bits
  of information versus 1 bit for a binary comparison. This means roughly 37%
  fewer comparisons to sort the same data. Measurable in benchmarks. Not a
  new complexity tier.

Do not claim O(log₃ N) sorting. Claim "37% fewer comparisons in the partition
step due to single-instruction three-way split." This is honest and still
significant.

### Implementation Task: Three-Way Partition Sort in `ulib.trit`

Replace or supplement the current insertion sort in `vec_sort` with a three-way
quicksort using `TCMP`. The implementation structure:

```
fn vec_sort_3way(vec: borrow_mut<Vec<T40>>, lo: T40, hi: T40) -> void {
    // Base case: zero or one element.
    match sign(hi - lo) {
        neg  => { return; }
        zero => { return; }
        pos  => {
            // Pivot selection: median of lo, mid, hi to avoid worst-case.
            let mid: T40 = lo + (hi - lo) / 3;
            let pivot: T40 = median3(vec, lo, mid, hi);

            // Single-pass three-way partition using TCMP.
            // After partition:
            //   vec[lo..lt-1]  < pivot
            //   vec[lt..gt]   == pivot
            //   vec[gt+1..hi]  > pivot
            var lt: T40 = lo;
            var gt: T40 = hi;
            var i:  T40 = lo;

            while pos(gt - i + 1) {
                let cmp: T1 = compare(vec_get(vec, i), pivot);
                match cmp {
                    neg  => {
                        vec_swap_elems(vec, i, lt);
                        lt = lt + 1;
                        i  = i  + 1;
                    }
                    zero => { i = i + 1; }
                    pos  => {
                        vec_swap_elems(vec, i, gt);
                        gt = gt - 1;
                        // Do not advance i: swapped element not yet examined.
                    }
                }
            }

            // Recurse on the two non-equal partitions.
            vec_sort_3way(vec, lo,    lt - 1);
            vec_sort_3way(vec, gt + 1, hi);
        }
    }
}
```

Add `vec_sort_3way` as the primary sort for `Vec` in `ulib.trit`. Keep the
existing insertion sort as `vec_sort_insertion` for small arrays (threshold:
9 elements, one ternary digit of the recursion). The quicksort switches to
insertion sort below this threshold.

Benchmark against the current insertion sort. The crossover point where
three-way quicksort wins depends on the data distribution. Document the
benchmark result in a comment in the source.

---

## 4. The Double-Ended Buffer: A Native Ternary Data Structure

This is the most original idea in this design space and has no direct binary
equivalent. It deserves a concrete implementation.

### The Structure

A single contiguous block of memory where:

- **Negative-valued entries** are written from address `base` upward (the
  "head end")
- **Positive-valued entries** are written from address `base + capacity - 1`
  downward (the "tail end")
- **The zero region** is the gap between the two ends

The two ends grow toward each other. The buffer is full when they meet.

This maps naturally to workloads where a single stream contains two logically
distinct kinds of data that must be processed by different logic — for example,
control metadata and raw payload in a network buffer, or structural nodes and
leaf values in a tree serialization.

### The Zero Region as Handoff Zone

The zero region is not merely a terminator or an empty gap. It can function as
a **shared handoff zone** between two processing tracks.

Items in the zero region are in transitional state — not yet claimed by either
the head-end processor or the tail-end processor. Either processor can claim
an item from the zero region by writing to its slot, changing it from zero to
negative or positive. This is a lock-free claim operation using `TSTR`:

```
// Attempt to claim a zero-region slot for the neg-end processor.
// Returns +1 (success), 0 (value changed, retry), -1 (reservation lost).
fn split_buf_claim_neg(buf: borrow_mut<SplitBuf>, zero_slot: T40) -> T1 {
    unsafe {
        let addr: T40 = buf.base + zero_slot;
        let reserved: T40 = tldr(addr, ACQ_REL);
        // Only claim if the slot is still zero.
        match sign(reserved) {
            zero => {
                return tstr(addr, 0 - 1, 0, ACQ_REL);
                // tstr returns: +1=claimed, 0=value changed, -1=reservation lost
            }
            neg  => { return 0 - 1; }   // already claimed by neg-end
            pos  => { return 0 - 1; }   // already claimed by pos-end
        }
    }
}
```

This makes the zero region a **work-stealing deque** expressed in the natural
geometry of a ternary buffer. Neither end needs a lock. The `TSTR` instruction
provides the atomic claim. The three-valued return of `TSTR` directly encodes
the three outcomes: claimed, contended, or lost.

### Struct Layout (4 words, malloc'd)

```
struct SplitBuf {
    base:     T40,    // base address of the data block
    capacity: T40,    // total words allocated
    neg_top:  T40,    // next write position for neg-end (grows up)
    pos_top:  T40,    // next write position for pos-end (grows down)
}
```

### API to Implement in `ulib.trit`

```
fn split_buf_new(capacity: T40) -> own<SplitBuf>

fn split_push_neg(buf: borrow_mut<SplitBuf>, val: T40) -> T1
fn split_push_pos(buf: borrow_mut<SplitBuf>, val: T40) -> T1

fn split_pop_neg(buf: borrow_mut<SplitBuf>) -> T40
fn split_pop_pos(buf: borrow_mut<SplitBuf>) -> T40

fn split_gap_start(buf: borrow<SplitBuf>) -> T40   // first zero-region address
fn split_gap_end(buf: borrow<SplitBuf>) -> T40     // last zero-region address
fn split_gap_len(buf: borrow<SplitBuf>) -> T40     // words in zero region

fn split_claim_neg(buf: borrow_mut<SplitBuf>, slot: T40) -> T1
fn split_claim_pos(buf: borrow_mut<SplitBuf>, slot: T40) -> T1

fn split_full(buf: borrow<SplitBuf>) -> T1         // neg_top meets pos_top
fn split_empty_neg(buf: borrow<SplitBuf>) -> T1
fn split_empty_pos(buf: borrow<SplitBuf>) -> T1

fn split_buf_free(buf: own<SplitBuf>) -> void
```

### Invariants the Implementation Must Maintain

1. `neg_top <= pos_top` at all times. When they are equal, the buffer is full.
2. Values at addresses `base..neg_top-1` are negative (neg-end entries).
3. Values at addresses `pos_top+1..base+capacity-1` are positive (pos-end
   entries).
4. Values at addresses `neg_top..pos_top` are zero (the gap / handoff zone).
5. `split_push_neg` writes a negative value and increments `neg_top`.
6. `split_push_pos` writes a positive value and decrements `pos_top`.
7. `split_push_neg` and `split_push_pos` must check `split_full` before
   writing and return `0 - 1` (error) if the buffer is full.

---

## 5. `parallel_match` — A Deferred Language Feature

The concept of a `parallel_match` that routes neg, zero, and pos arms to
simultaneous execution tracks is architecturally interesting. However, it
requires hardware support that does not yet exist in the VM or the FPGA target.

The current vector ISA provides data parallelism within a single instruction
across 27 lanes (`VADD`, `VCMP`, `VSEL`). This is not the same as two
independent control-flow tracks executing simultaneously on diverged branches.

The `VSEL` instruction is the correct current primitive for this pattern:

```
// Compute both paths, select results based on predicate.
// This is the compiler's current implementation of parallel-looking code.
let pred: L1 = vcmp(data, zero_vec);      // per-lane sign comparison
let neg_result: [T40] = process_neg(data); // compute neg path for all lanes
let pos_result: [T40] = process_pos(data); // compute pos path for all lanes
let out: [T40] = vsel(pred, neg_result, zero_vec, pos_result);
```

This is not true divergent execution — it computes both branches and discards
one result per lane. For workloads where one branch is cheap and the other is
expensive, this is wasteful. True multi-execute predication would be better.

**Action:** Add `parallel_match` to the TCL 2.0 planned extensions list with
a note that it requires Phase F hardware (multi-execute predication in the wide
vector unit) before it can be lowered correctly. Do not implement it in TCL 1.x.
Document the `VSEL` pattern as the current idiomatic substitute so developers
are not waiting for `parallel_match` to write parallel-feeling code today.

---

## 6. Correct Framing for Communicating These Ideas

The following table gives the accurate framing for each claim. Use the accurate
column when documenting, benchmarking, or communicating about these features.

| Claim | Inaccurate framing | Accurate framing |
|---|---|---|
| Sort comparison count | "O(log₃ N) complexity" | "37% fewer comparisons in the partition step; same O(n log n) class" |
| Three-way partition | "New field of DSA" | "Existing ternary algorithms made practically competitive by native TCMP" |
| Double-ended buffer | "Binary has no equivalent" | True. This is genuinely novel. |
| Parallel match | "Single-cycle zero-overhead divergence on Triton-27" | "Requires future multi-execute hardware; current substitute is VSEL" |
| Zero region | "Pure terminator" | "Handoff zone; enables lock-free work-stealing via TSTR" |

---

## 7. Implementation Priority Order

Following the dependency graph rule:

```
1. if pos / if neg / if zero syntax sugar          [TCL 1.1, parser only, low risk]
         ↓
2. vec_sort_3way in ulib.trit                      [replaces insertion sort]
         ↓
3. split_buf in ulib.trit                          [new native data structure]
         ↓
4. split_claim_neg / split_claim_pos               [requires unsafe TSTR, test carefully]
         ↓
5. parallel_match design document                  [TCL 2.0 placeholder, no impl yet]
```

Items 1 and 2 are independent and can be done in parallel. Item 3 depends on
the `own<T>` ownership system being stable in the compiler. Item 4 depends on
item 3 and requires careful testing of the `TSTR` loop under concurrent access.
Item 5 is documentation only and can be written at any time.

---

## 8. Test Cases to Write Before Implementation

### For `if pos` / `if neg` / `if zero`

```
// Must compile and produce the same IR as the equivalent full match.
fn test_guard_sugar(x: T40) -> T40 {
    var result: T40 = 0;
    if pos(x) { result = 1; }
    if neg(x) { result = 0 - 1; }
    return result;
}

// Must NOT compile: if pos in expression position.
// let y: T40 = if pos(x) { 1 };    // ERROR: guard is statement-only
```

### For `vec_sort_3way`

- Already-sorted array of 27 elements: verify O(n) comparison count.
- Reverse-sorted array: verify no quadratic blowup with median-of-3 pivot.
- Array of all equal elements: verify O(n) behavior (all go to equal partition).
- Array of 3 elements: verify base case and single-level recursion.
- Benchmark against insertion sort at sizes 1, 9, 27, 81, 243.

### For `split_buf`

- Push neg until full from neg end: verify pos end untouched.
- Push pos until full from pos end: verify neg end untouched.
- Interleave pushes from both ends: verify gap shrinks correctly.
- `split_claim_neg` on a zero-region slot: verify TSTR returns +1.
- `split_claim_neg` on an already-claimed slot: verify TSTR returns 0.
- Two concurrent `split_claim_neg` on the same slot: verify exactly one
  succeeds and one gets 0 (value mismatch) or -1 (reservation lost).
