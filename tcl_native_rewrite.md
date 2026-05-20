This is the production-ready master architectural specification for the TCL Native Rewrite. The three critical historical safeguards—the **VFS Plane Separation**, the **Two-Tiered Macro/Micro Scheduler**, and the **Non-Relational Bootstrap Pass**—have been integrated directly into the engineering requirements.

---

# TCL Native Rewrite — Hardened Architectural Specification

## Context & Constraints

The current compiler (`ternary_compiler.h`) is written in C++ and outputs `tasm`. The current OS substrate (`ternary_os.h`) is a transient C++ host facade. The absolute goal of this project is to implement self-hosting and a bare-metal operating system running entirely as native `.trit` programs inside the ternary VM.

To prevent systemic performance and bootstrapping traps common in historic advanced OS designs, this plan enforces strict layer isolation: **micro-mechanics must remain lightweight and flat, while macro-policies leverage the transactional relational engine.**

---

## Phase A — Bootstrap Compiler (`tritc` v2 in C++, targeting TCL 1.0)

These steps extend the existing C++ compiler to accept TCL 1.0 language features. The output is still `tasm`. This temporary compiler is strictly used to compile the native rewrite.

* **A1. Replace `match sign(expr)` with `match expr { neg / zero / pos }`**
* *Parser:* Eliminate the `sign(...)` wrapper requirement. Arm identifiers are now native ternary keywords. Enforce that all three arms are present or emit a compile-time error.


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

## Phase C.5 - Pre-D Consolidation Gate

Phase D is not allowed to begin until the compiler/runtime/VM surface below is proven in isolation. This phase exists to avoid debugging the native kernel, VM privilege mechanics, and standard library ownership model at the same time.

* **C.5a. Text Diagnostics and `sys_write_char`:** Keep syscall id `22` reserved for `sys_write_char(c: T40) -> T40`. The C++ bootstrap compiler, VM legacy syscall path, routed kernel syscall path, and `ulib.trit` output helpers (`print_char`, `print_string`, and non-format literal output in `printf`) must render ASCII text, not decimal character codes. Any kernel that routes syscalls must either provide a raw character console CSR path or explicitly switch the console device into character mode around `console_out`.

* **C.5b. TCL 1.0 `ulib.trit` Ownership Rewrite:** Public allocation APIs must expose `own<ptr<T, unknown>>`, release APIs must accept `borrow<ptr<T, unknown>>` or `borrow_mut<...>` as appropriate, and user/kernel buffer parameters must use `ptr<T, user, S>` or `ptr<T, kernel, S>` states where the caller relies on validation. Raw integer address helpers may remain as private/internal compatibility functions only when they are isolated behind typed public wrappers. The entire native compiler source set must compile cleanly against the rewritten ulib before Phase D.

* **C.5c. Compiler Golden Program Suite:** Maintain 15-20 small TCL 1.0 programs with known outputs covering arithmetic, control flow, recursion, structs, arrays, constants, width-parametric functions, ownership moves/drops, pointer state transitions, unsafe load/store, atomics, vector helpers, and ulib text output. Each case must run through the C++ bootstrap compiler and VM now, and through the native compiler as soon as Phase B/C can emit runnable images. Phase D requires output equality between the bootstrap and native compiler for this suite.

* **C.5d. Privilege/Trap Harness:** Maintain a raw TASM harness that installs `TVEC`, enters user mode, triggers a syscall/trap, observes handler-mode CSR state, returns through `ERET`, and verifies that `EPC`, `CAUSE`, `STATUS`, previous privilege, and final user privilege are correct. D1's trap entry stub depends on this exact machine sequence.

---

## Phase D — Native OS Kernel (`kernel.trit`)

Replaces `minimal_kernel_bringup.tasm` and `ternary_os.h`. Written in `.trit`, compiled by the Phase A/B/C pipeline, and executed in kernel privilege mode.

1. **D1. Low-Level Trap Entry:** Pure Assembly Stub.
Write the trap entry handler in raw tasm. This layer must remain completely non-relational and must not access any stack mechanics until all active CPU registers are securely saved to the targeted task context block. Upon complete context preservation, branch into the high-level TCL dispatch routine: `kernel_dispatch(cause: T40) -> void`.

2. **D2. Non-Relational Bootstrapper (Phase D2a):** Un-Logged Bootstrap Array.
To break the bootstrap paradox (where the buffer pool requires relations, but relations require the buffer pool), implement a temporary, un-logged, non-relational page allocator. This system relies on a primitive, static array-backed memory bitmap hardcoded into the kernel image. It provides simple page allocations before any transactional subsystems or B-trees are operational.

**Critical Implementation Note:** The bootstrap bitmap must be completely independent of relational state. Every allocation from `alloc_bootstrap_page()` must be trivially traceable—use a simple bitfield scan with zero relational queries. When Phase D5 migration begins, a single-pass scan must populate the formal `allocations` relation with every entry from the bootstrap bitmap. If any bootstrap allocation lacks a corresponding relational row, the migration is corrupted and unrecoverable.

3. **D3. Transactional Buffer Pool Manager:** The Memory Engine (Phase D2b).
Construct the formal Buffer Pool Manager on top of the bootstrap allocator. Every tracked physical page frame is described by a `PageHeader` struct. This struct must be defined exactly as follows and must not be changed after D3 is complete, as every subsequent phase depends on it:

```
struct PageHeader {
    ppn:       T40,   // physical page number
    pin_count: T40,   // number of active DMA or accelerator references;
                      //   >0 means the page cannot be evicted or remapped
    dirty:     T1,    // pos = page has uncommitted writes
    version:   T40,   // MVCC version identifier; maps to ptr<T, S> states
    lru_clock: T40,   // position in the LRU eviction clock
    namespace: T40,   // owning namespace ID, matches PTE namespace field
}
```

**Eviction rule:** The eviction path must check `pin_count` before selecting a candidate. A page with `pin_count > 0` is ineligible for eviction regardless of LRU position. This check must be in the eviction hot path from the start — retrofitting it after the eviction logic is written requires rewriting the entire path.

**`pin_count` interaction contracts** — three interactions with other kernel subsystems must be resolved at D3, before those subsystems are built, so that each subsystem is designed with the constraint in mind from the start:

* **COW interaction (relevant at D9):** The COW fault handler remaps pages on write. A page with `pin_count > 0` holds a live physical address reference from an external agent. The COW handler must read `pin_count` before remapping and must block or return an error if the page is pinned. It must not remap a pinned page under any circumstance.
* **Fork interaction (relevant at D9):** `fork()` copies page table entries. If a parent process holds DMA-pinned pages, the child's address space cannot safely share those physical pages with an in-flight external operation. Fork must either refuse to copy pinned PTEs into the child, or wait for all operations on those pages to drain to `pin_count == 0` before proceeding.
* **MVCC version chain interaction (relevant at D4/D5):** A write operation that pins a page for external access must ensure no reader holds a reference to a version of that page that will be overwritten. This is the same acquire-release problem as `shared<T, ACQ_REL>` in the type system and must use the same `TLDR`/`TSTR` primitives to coordinate. The version field in `PageHeader` is the authoritative record of which MVCC generation is currently pinned.

These are not future concerns. They are constraints on the design of D9's COW handler and fork logic, and they must be stated here so that D9 is not designed in ignorance of them.

Page versions map directly to language-level `ptr<T, S>` states.

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
To prevent relational lookup overhead from destroying context-switch times, split scheduling into two isolated tiers. Tier-1 is the micro-scheduler: implemented as a highly optimized, flat, non-relational O(1) run-queue array. The core timer-interrupt handler (D1) only interacts with this flat Tier-1 layer, popping the next runnable task thread in microsecond time horizons without touching any relational B-trees.

7. **D7. Two-Tiered Scheduler: Macro-Reconciliation Loop:** Tier-2 Scheduling Policy.
Implement Tier-2 of the scheduler as a background macro-horizon reconciliation loop executing strictly every 10–50 ms. This loop queries the formal `desired_processes` and `quotas` relations, evaluates priority shifts and resource usage balances, and flushes the resulting top-tier runnable threads into the flat Tier-1 O(1) run-queue array.

8. **D8. VFS Isolation: Relational Control vs. Linear Vector Data Plane.**

To prevent transactional locking and metadata update overhead from throttling raw throughput, the Virtual File System is bisected into two isolated operational planes that share the underlying Buffer Pool and Write-Ahead Log (WAL) substrate.

### D8a. The Relational Control Plane (Metadata & Structural Trees)

* **Mechanics:** All file system metadata—including directories, inodes, permissions, namespace identifiers, and block allocation extent maps—are stored as strict, B-tree-indexed relational tables within the kernel's private buffer pool.
* **Transaction Guarding:** Every modification to the file hierarchy (such as creating a file, updating directory records, or extending file blocks) acts as a formal database transaction. These mutations are validated against the active process namespace column and written to the block-layer WAL before any changes are committed to disk.

### D8b. The Linear Vector Data Plane (The Stream Engine)

* **Mechanics:** Raw file payload storage completely bypasses the B-tree relational indexing engine during active, ongoing read and write operations. High-throughput data flows sequentially through raw physical page frame extents managed directly by the Buffer Pool.
* **The Extent Resolution Boundary:** Before data streaming can begin, the initial file offset must be translated to a physical page number. This mapping step explicitly queries the extent maps residing within the Relational Control Plane. Once this initial extent resolution is complete, the hot path for sequential I/O completely avoids per-block relational updates.
* **The Commit Boundary:** Relational file system attributes (such as total file length, modified timestamps, and terminal block allocations) are only updated in the Relational Control Plane upon explicit file closure or transaction commit, keeping the sequential execution path free of relational lock contention.

### D8c. Recommended 3-Element Sub-Word Layout Optimization

* **The Data Layout:** To maximize performance on data structures that naturally decompose into multi-dimensional components (such as spatial coordinates, color channels, or tensor slices), the platform establishes a recommended 3-element vector layout within each 27-trit machine word:

```
[ Word Layout: 27-trits ] -> [ 9-trit Short A | 9-trit Short B | 9-trit Short C ]

```

* **VFS Neutrality:** This layout is a *recommendation* for specialized workloads, **not a mandatory VFS invariant**. The VFS functions strictly as a neutral data router; it does not perform data transformations, packing, or chunking on arbitrary streams. Applications requiring raw, un-transformed byte streams (e.g., text files, source code, executables) receive raw data without VFS overhead or forced alignment.
* **Spatial Cache Tiling:** For files matching this layout, data is arranged sequentially in blocks aligned to the CPU's L1 cache boundaries. Multi-word lookups populate the cache with zero bit-shifting or unpacking overhead, minimizing DRAM bus starvation during intense streaming pipelines.

### D8d. Hardware Vector Pipeline Unification & Execution Boundaries

* **Lining up the Lanes:** The Triton-27 CPU core defines a native hardware vector length of **27 lanes**. When an application utilizes the recommended layout, a sequential data block read of exactly 9 words pulls precisely 27 distinct data elements into the hardware register track (9 words × 3 sub-words = 27 data elements).
* **Instruction Efficiency:** A parallel compute pass (such as a media transformation, a cryptographic hash computation, or an array reduction) can process all 27 data elements simultaneously across the hardware lanes using a **single vector instruction**. *Note: Actual clock cycle retirement per instruction depends on execution unit latencies (e.g., multiplies vs. additions) and cache pipeline state.*
* **Separation of Concerns:** The VFS responsibility terminates at delivering cache-line-aligned data blocks to the buffer pool. The mechanisms for exploiting sub-word layouts and managing how data lands in vector registers are exclusively owned by the compiler (via width-parametric functions and the `#[parallel]` path) and the runtime, keeping file system layout completely decoupled from future shifts in the hardware execution model.

---

### Technical Guardrail for the Implementing Agent

> **Implementation Rule:** In `kernel.trit`, do not allow raw sequential read/write system calls (`sys_read`/`sys_write`) to invoke individual B-tree inserts or metadata updates per block.
> The user-space buffer pointer must be validated as a `ptr<T40, valid>` reference and mapped to raw physical memory extents via the Buffer Pool. The VFS must deliver data as a transparent stream without enforcing arbitrary layout transformations on the payload. All relational index changes must be deferred and batched into a single WAL-backed atomic transaction at the end of the operation.

---

9. **D9. COW Fork, Namespaces, Process Caching, and Final PTE Layout:** System Complete.

#### PTE Layout (27-trit word — finalised here, frozen after D9)

The original PTE design allocated all 27 trits without a spare for MMIO region marking. MMIO regions must be excluded from COW copies and snapshots, which requires a dedicated permission trit. To accommodate this without expanding the word, the Namespace ID field is narrowed from 6 trits to 5 trits:

| Trits  | Field        | Encoding                                              | Notes                                      |
|--------|--------------|-------------------------------------------------------|--------------------------------------------|
| 0–14   | PPN          | 3^15 = 14,348,907 physical page addresses             | Unchanged                                  |
| 15–19  | Namespace ID | 3^5 = 243 hardware-isolated address domains           | Narrowed from 6 trits (729) to free trit 20 |
| 20     | MMIO         | neg = normal memory;  pos = MMIO-mapped region        | New. MMIO pages excluded from COW and snapshots |
| 21     | COW          | neg = writable;  pos = copy-on-write active           | Unchanged                                  |
| 22     | R            | neg = read disabled;  pos = read allowed              | Unchanged                                  |
| 23     | W            | neg = write disabled;  pos = write allowed            | Unchanged                                  |
| 24     | X            | neg = non-executable;  pos = executable               | Unchanged                                  |
| 25     | Privilege    | neg = kernel;  zero = supervisor;  pos = user         | Unchanged                                  |
| 26     | Valid        | neg = fault/unmapped;  zero = reserved;  pos = present | zero state explicitly reserved — see note  |

**Rationale for narrowing Namespace ID:** 243 hardware-isolated address domains is sufficient for any foreseeable single-node deployment. The practical ceiling for concurrent isolated namespaces on a single Triton-27 core is constrained well below 243 by process table size and scheduler capacity long before the namespace field saturates. The 486-domain reduction in theoretical capacity (729 → 243) has no practical cost and frees the trit needed for MMIO without requiring a second PTE word.

**Valid field zero state:** The `zero` state is explicitly reserved as transitional/undefined. On cold boot, DMEM initialises to zero, meaning all PTEs start in the `zero` (reserved) state, not in the `neg` (fault) state. The MMU fault handler must treat `zero` the same as `neg` — as an unmapped page that triggers a fault. This must be implemented in the MMU RTL and in the software page-walk code. Do not assume a zeroed PTE is safe to dereference.

**MMIO trit behaviour:** Pages with the MMIO trit set to `pos` must be excluded from COW duplication, fork propagation, and snapshot inclusion. The COW fault handler, the fork path, and any snapshotting mechanism must check this trit before operating on a page. The `pin_count` constraint from D3 also applies to MMIO-mapped pages — an MMIO page is implicitly pinned for the lifetime of the MMIO mapping and must not be evicted.

Implement `fork()` as an O(1) metadata operation that duplicates PTEs and marks non-MMIO pages as COW, postponing physical duplication until write-fault entry. Enforce namespace filters automatically during table queries. To bypass system call traps for reading static data, maintain a process-local memory cache of observed state; mutations invalidate this cache via a fast hardware `FENCE.0` sequence. Conclude by initializing the user-space `shell_main()`.

---

## Phase E — Self-Hosting

* **E1. Bootstrapping Evaluation:** Compile `tcl_frontend.trit`, `tcl_backend.trit`, and `ulib.trit` using the legacy Phase A C++ compiler. Run the compiled native binary inside the VM environment to compile a test program. The output must be bit-identical to the C++ compiler's native output.
* **E2. Secondary Self-Hosting Loop:** Compile `tcl_frontend.trit` and `tcl_backend.trit` using the native compiler binary generated in step E1. If the compiler binaries from E1 and E2 match identically down to the bit level, the compiler toolchain is officially self-hosting.
* **E3. Demolition:** Decommission the legacy C++ compiler source. All subsequent alterations to the compiler toolchain, language specifications, or transactional operating system kernel are written and maintained exclusively in native `.trit` source code.

---

## Dependency Order Summary

```
A1 -> A2 -> A3 -> A4 -> A5 -> A6 -> A7 -> A8
                          |
                    B1 -> B2 -> B3 -> B4 -> B5 -> B6
                                      |
                                C1 -> C2 -> C3 -> C4 -> C5 -> C6
                                                  |
                                                C.5
                                                  |
                                            D1 -> D2 -> D3 -> D4 -> D5 -> D6 -> D7 -> D8 -> D9
                                                                                  |
                                                                             E1 -> E2 -> E3
```

> **Critical Guardrail for Implementing Agent:** Do not attempt to optimize early phases by building high-level relational paradigms prematurely. Phase D must follow the structural sequence exactly: the raw bootstrap page array (`D2`) and basic buffer pages (`D3`) must be completely stable before the transactional relational store engine (`D5`) is initialized.
