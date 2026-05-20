This is the production-ready master architectural specification for the TCL Native Rewrite. The three critical historical safeguards—the **VFS Plane Separation**, the **Two-Tiered Macro/Micro Scheduler**, and the **Non-Relational Bootstrap Pass**—have been integrated directly into the engineering requirements.

---

# TCL Native Rewrite — Hardened Architectural Specification

## Context & Constraints

The current compiler (`ternary_compiler.h`) is written in C++ and outputs `tasm`. The current OS substrate (`ternary_os.h`) is a transient C++ host facade. The absolute goal of this project is to implement self-hosting and a bare-metal operating system running entirely as native `.trit` programs inside the ternary VM.

To prevent systemic performance and bootstrapping traps common in historic advanced OS designs, this plan enforces strict layer isolation: **micro-mechanics must remain lightweight and flat, while macro-policies leverage the transactional relational engine.**

---

## Phase A — Bootstrap Compiler (`tritc` v2 in C++, targeting TCL 1.0)

These steps extend the existing C++ compiler to accept TCL 1.0 language features. The output is still `tasm`. This temporary compiler is strictly used to compile the native rewrite.

* **A1. Replace `match sign(expr)` with `match expr { neg / zero / pos }**`
* *Parser:* Eliminate the `sign(...)` wrapper requirement. Arm identifiers are now native ternary keywords. Enforce that all three arms are present or emit a type compile-time error.


* **A2. Add `T1` as a First-Class Type**
* *Type System:* Add `TypeKind::Trit`. *Inference:* Comparisons return `T1`. `while` conditions must be `T1`. `match` subjects may be evaluated as a `T1` directly.


* **A3. Add Width-Parametric Functions**
* *Parser:* `fn f<W: TritWidth>(...)` syntax. *Type System:* Introduce width type variables. *Unification:* Width variables unify like type variables. Monomorphize at call sites during code generation.


* **A4. Add `ptr<T, S>` Three-Valued Pointer Type**
* *Type System:* Add `TypeKind::Pointer` with compiler state tracking `{null, unknown, valid}`. These tracking states directly align with the kernel's Multi-Version Concurrency Control (MVCC) visibility states in Phase D.
* *Parser:* `ptr<T40, unknown>` syntax.
* *Inference:* A structural match block via `{ null => ... unknown => ... valid(q) => ... }` introduces `q: ptr<T40, valid>` exclusively within the valid block arm. Dereferencing a pointer outside an explicitly proven valid arm must trigger a compile-time type error.


* **A5. Add `own<T>` and Auto-Drop Lifetime Analysis**
* *Type System:* Add `TypeKind::Owned`. *Move Semantics:* Assignment of an `own<T>` structural object invalidates the source binding. *Scope Analysis:* Automatically insert a `free(ptr)` system call at scope exit for every live `own<T>` instance. Reference tracking types `borrow<T>` and `borrow_mut<T>` are validated to ensure they never outlive their root owner's lifetime scope.


* **A6. Add `shared<T, ORDER>` Atomic Type**
* *Type System:* Add `TypeKind::Shared` with an explicit ordering constant. Type-check all `atomic_load` and `atomic_store` operations: the order argument must be greater than or equal to the declared order constraint.
* *Codegen:* Lower directly to `TLDR`/`TSTR`/`FENCE` machine sequences to back the process-local read caches in Phase D.


* **A7. Wire Register Allocator into Codegen**
* Map the existing `AllocationResult` from `allocateRegisters()` straight into `CompilerImpl::emitStmt` instead of defaulting to the flat five-register pool, eliminating the one known correctness gap in the legacy compiler.


* **A8. Add `own<T>` to `ulib.trit` Signatures**
* Rewrite the core standard library function signatures to use TCL 1.0 ownership types. Verify clean compilation across all modules.



---

## Phase B — Native Compiler Frontend (`tcl_frontend.trit`)

Written in `.trit` and compiled by the Phase A compiler. The output is TCL IR serialized as a structured data format in data memory (DMEM).

* **B1. Lexer:** `fn lex(src: ptr<T40, valid>, len: T40) -> own<Vec<Token>>`. Batch tokens into a pre-allocated vector area to bypass granular heap allocations per character.
* **B2. Parser:** Recursive descent layout. Each `parse_X` function takes a token cursor (`borrow_mut`) and returns an AST node (`own`). AST nodes are allocated inside a structured memory arena. Operator precedence: `match` > `while` > binary > unary > postfix > primary.
* **B3. Type Variable Pool:** A ring buffer of fresh type variable IDs. The substitution map uses a Ternary Search Tree (TST) mapping variable IDs to concrete types. `unify(a, b)` walks substitutions, resolving width variables via the min-width rule and pointer states via dataflow rules.
* **B4. Algorithm W:** `infer_expr(env, expr) -> (Type, Substitution)`. Statement inference threads substitutions through the block list. Apply value restriction during generalization at `let` bindings.
* **B5. Pointer-State Dataflow:** Forward pass over the AST. When a pointer `p` is matched, add `q -> valid` to the local environment inside the `valid(q)` arm. At join points, meet the states from all arms (`valid ∩ unknown = unknown`).
* **B6. IR Emission:** `lower_fn(fn_ast, types) -> IRFunction`. Lowers expressions to basic blocks containing typed SSA instructions and a structural ternary terminator (`branch3`, `jump`, `return`). `own<T>` introductions emit `IR::Alloc`; scope exits emit `IR::Drop`.

---

## Phase C — Native Compiler Backend (`tcl_backend.trit`)

Transforms Phase B IR into raw `tasm` assembly text. Compiled by the Phase A compiler.

* **C1. Monomorphization:** Walk all call sites. For each width-parametric invocation, instantiate a concrete, flat function copy with resolved widths. Deduplicate using a `(function_name, width_tuple)` lookup key.
* **C2. Mem2reg:** Promote stack-allocated SSA values to register-resident values if their memory address is never explicitly taken. Insert phi nodes at dominance frontiers.
* **C3. Graph-Coloring Register Allocation:** Construct a standard interference graph from SSA liveness data. Color across 24 working registers (`r1–r24`), biasing `r1–r12` for values that must persist across function calls. Spill to stack frames via `STORE`/`LOAD` pairs.
* **C4. Instruction Selection:** Lower IR instructions directly to `tasm` per TCL Spec §9. `branch3` maps to `tcmp` paired with `brn`/`brz`/fallthrough. Pointer dereferences map to `LOAD`. `own` drops lower to `sys_free`.
* **C5. Frame Layout:** Allocate fixed-size stack frames computed from spill slots and aggregate sizes. Subtract size from the stack pointer (`sp`) at entry; preserve the Link Register (`LR`) at `sp+0`.
* **C6. Tasm Emission:** Serialize labels, function prologues, body code, epilogues, and returns. Name generic instantiations using mangled labels (e.g., `fname__W40`).

---

## Phase D — Native OS Kernel (`kernel.trit`)

Replaces `minimal_kernel_bringup.tasm` and `ternary_os.h`. Written in `.trit`, compiled by the Phase A/B/C pipeline, and executed in kernel privilege mode.

1. **D1. Low-Level Trap Entry:** Pure Assembly Stub.
Write the trap entry handler in raw tasm. This layer must remain completely non-relational and must not access any stack mechanics until all active CPU registers are securely saved to the targeted task context block. Upon complete context preservation, branch into the high-level TCL dispatch routine: `kernel_dispatch(cause: T40) -> void`.

2. **D2. Non-Relational Bootstrapper (Phase D2a):** Un-Logged Bootstrap Array.
To break the bootstrap paradox (where the buffer pool requires relations, but relations require the buffer pool), implement a temporary, un-logged, non-relational page allocator. This system relies on a primitive, static array-backed memory bitmap hardcoded into the kernel image. It provides simple page allocations before any transactional subsystems or B-trees are operational.

**Critical Implementation Note:** The bootstrap bitmap must be completely independent of relational state. Every allocation from `alloc_bootstrap_page()` must be trivially traceable—use a simple bitfield scan with zero relational queries. When Phase D5 migration begins, a single-pass scan must populate the formal `allocations` relation with every entry from the bootstrap bitmap. If any bootstrap allocation lacks a corresponding relational row, the migration is corrupted and unrecoverable.

3. **D3. Transactional Buffer Pool Manager:** The Memory Engine (Phase D2b).
Construct the formal Buffer Pool Manager on top of the bootstrap allocator. Every tracked physical page frame must carry a pin count (T40), dirty flag (T1), version identifier (T40), and an LRU clock position tracking entry (T40). Eviction targets the oldest unpinned clean page. Page versions map directly to language-level `ptr<T, S>` states.

4. **D4. Write-Ahead Log (WAL) Engine:** ACID Bedrock.
Implement the kernel-managed transaction log as a structural ring buffer tied directly to the underlying block storage device. Every transactional modification must serialize a log entry containing `(block_address, old_data, new_data, transaction_id)` before flushing dirty data frames to storage. Expose four structural system primitives: `log_write`, `log_commit`, `log_abort`, and `log_checkpoint`.

5. **D5. Relational State Store Initialization:** State Migration.
Construct fixed-size relational tables (Process Table, File Descriptor Table, Quotas Table) inside B-trees backed by the Buffer Pool and the WAL. **Critical Migration Trigger:** Once the relational store is online, loop through the initial Phase D2a bootstrap bitmap state, insert corresponding allocation rows into the new formal transactional memory map relation, and permanently deprecate the raw bootstrap allocator.

**Transaction Quiescence Protocol:** Before Phase D5 migration begins, the kernel must enter a quiescent state:
- Freeze all user-space processes (suspend scheduling, hold all thread contexts in kernel-managed state blocks).
- Complete all in-flight transactions: commit all currently outstanding transactional writes to the bootstrap WAL, then checkpoint.
- Perform the one-pass migration scan: read bootstrap bitmap → insert relational `allocations` rows → flush WAL checkpoint.
- Resume user-space processes. Any transaction that was held in flight during migration must be replayed through the new relational WAL to ensure consistency.

This ensures no in-flight transaction state is lost or corrupted during the architectural transition from bootstrap mode to relational mode.

6. **D6. Two-Tiered Scheduler: O(1) Micro-Engine:** Tier-1 Scheduling Micro-Mechanics.
To prevent relational lookup overhead from destroying context-switch times, split scheduling into two isolated tiers. Tier-1 is the micro-scheduler: implemented as a highly optimized, flat, non-relational $O(1)$ run-queue array. The core timer-interrupt handler (D1) only interacts with this flat Tier-1 layer, popping the next runnable task thread in microsecond time horizons without touching any relational B-trees.

7. **D7. Two-Tiered Scheduler: Macro-Reconciliation Loop:** Tier-2 Scheduling Policy.
Implement Tier-2 of the scheduler as a background macro-horizon reconciliation loop executing strictly every $10\text{--}50\text{ ms}$. This loop queries the formal `desired_processes` and `quotas` relations, evaluates priority shifts and resource usage balances, and flushes the resulting top-tier runnable threads into the flat Tier-1 $O(1)$ run-queue array.

Here is the rewritten, highly optimized specification for **Phase D8 (Virtual File System Isolation & Vector Data Plane)**. This version integrates our 9-trit sub-word packing strategy and the 27-lane hardware vector mechanics straight into the Virtual File System (VFS) to maximize spatial cache tiling for linear data streaming.

---

To prevent transactional locking and metadata update overhead from throttling raw throughput, the Virtual File System is bisected into two isolated operational planes that share the underlying Buffer Pool and Write-Ahead Log (WAL) substrate.

#### D8a. The Relational Control Plane (Metadata & Structural Trees)

* **Mechanics:** All file system metadata—including directories, inodes, permissions, namespace identifiers, and block allocation extent maps—are stored as strict, B-tree-indexed relational tables within the kernel's private buffer pool.
* **Transaction Guarding:** Every modification to the file hierarchy (such as creating a file, updating directory records, or extending file blocks) acts as a formal database transaction. These mutations are validated against the active process namespace column and written to the block-layer WAL before any changes are committed to disk.

#### D8b. The Linear Vector Data Plane (The Stream Engine)

* **Mechanics:** Raw file payload storage completely bypasses the B-tree relational indexing engine during active read and write operations. High-throughput data flows sequentially through raw physical page frame extents managed directly by the Buffer Pool.
* **The Commit Boundary:** Relational files system attributes (such as total file length, modified timestamps, and terminal block allocations) are only updated in the Relational Control Plane upon explicit file closure or transaction commit, keeping the sequential execution path free of relational lock contention.

#### D8c. Native 3-Element Sub-Word Layout Optimization

* **The Data Layout:** To exploit our hardware architecture, all sequential file payloads and media streams are packed and structured as native **3-element vector arrays** within each 27-trit machine word. Each word natively encapsulates a 3-dimensional coordinate tensor, an opponent-trit color slice, or a 3-part data frame:
```
[ Word Layout: 27-trits ] -> [ 9-trit Short A | 9-trit Short B | 9-trit Short C ]

```


* **Spatial Cache Tiling:** The Linear Data Plane reads and writes file bytes in blocks that are structurally aligned to the CPU's L1 cache boundaries. Because data is natively arranged in these 3-element packets, multi-word lookups populate the cache with zero bit-shifting or unpacking overhead. This layout completely avoids DRAM bus starvation during intense streaming operations.

#### D8d. Hardware Vector Pipeline Unification

* **Lining up the Lanes:** The Triton-27 CPU core defines a native hardware vector length of **27 lanes**. Because every single word handles exactly three 9-trit sub-words, a data block read of exactly **9 words** pulls precisely **27 distinct data elements** into the hardware register track ($9 \times 3 = 27$).
* **Zero-Copy Execution:** When an application executes a linear read or compute pass, the VFS streams these 9-word blocks directly out of the Buffer Pool into the CPU's native vector registers in a single transaction. A parallel operation (such as a media transformation, a cryptographic hash computation, or an array reduction) can process all 27 data elements simultaneously across the hardware lanes in a single clock cycle.

---

### Technical Guardrail for the Implementing Agent

> **Implementation Rule:** In `kernel.trit`, do not allow raw sequential read/write system calls (`sys_read`/`sys_write`) to invoke individual B-tree inserts or metadata updates per block.
> The user-space buffer pointer must be validated as a `ptr<T40, valid>` reference, mapped to raw physical memory extents via the Buffer Pool, and chunked into the 9-word (27-element) sub-word alignment before streaming. All relational index changes must be deferred and batched into a single WAL-backed atomic transaction at the end of the operation.

1. **The Relational Control Plane:** Manages directories, inodes, metadata, permissions, and file extensions through strict WAL-backed transactional relations.
2. **The Linear Data Plane:** Bypasses the transactional store entirely. Large sequential block streams utilize raw block extents and flow directly through raw buffer pool frames. File sizes and completion pointers are updated relationally *only* upon final transaction commit.

9. **D9. COW Fork, Namespaces, and Process Caching:** System Complete.
Extend Page Table Entries (PTE) with copy-on-write (COW) and namespace ID trit fields. Implement `fork()` as an $O(1)$ metadata operation that duplicates entries and marks pages as COW, postponing duplication until write-fault entry. Enforce namespace filters automatically during table queries. To bypass system call traps for reading static data, maintain a process-local memory cache of observed state; mutations invalidate this cache via a fast hardware `FENCE.0` sequence. Conclude by initializing the user-space `shell_main()`.


---

## Phase E — Self-Hosting

* **E1. Bootstrapping Evaluation:** Compile `tcl_frontend.trit`, `tcl_backend.trit`, and `ulib.trit` using the legacy Phase A C++ compiler. Run the compiled native binary inside the VM environment to compile a test program. The output must be bit-identical to the C++ compiler's native output.
* **E2. Secondary Self-Hosting Loop:** Compile `tcl_frontend.trit` and `tcl_backend.trit` using the native compiler binary generated in step E1. If the compiler binaries from E1 and E2 match identically down to the bit level, the compiler toolchain is officially self-hosting.
* **E3. Demolition:** Decommission the legacy C++ compiler source. All subsequent alterations to the compiler toolchain, language specifications, or transactional operating system kernel are written and maintained exclusively in native `.trit` source code.

---

## Dependency Order Summary

```
A1 → A2 → A3 → A4 → A5 → A6 → A7 → A8
                          ↓
                    B1 → B2 → B3 → B4 → B5 → B6
                                      ↓
                                C1 → C2 → C3 → C4 → C5 → C6
                                                  ↓
                                            D1 → D2 → D3 → D4 → D5 → D6 → D7 → D8 → D9
                                                                                  ↓
                                                                             E1 → E2 → E3

```

> **Critical Guardrail for Implementing Agent:** Do not attempt to optimize early phases by building high-level relational paradigms prematurely. Phase D must follow the structural sequence exactly: the raw bootstrap page array (`D2`) and basic buffer pages (`D3`) must be completely stable before the transactional relational store engine (`D5`) is initialized.</Vec</T40,></T,></T40,></T40,></T,></W:>