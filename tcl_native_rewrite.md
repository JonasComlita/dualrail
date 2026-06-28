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

Status: completed for the current native-kernel acceptance scope. Keep
`test_phase_d_kernel`, `test_native_apps`, `test_process_handoff`, and the
production gate as regression coverage when changing this layer.

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

Page versions map directly to language-level `ptr<T, S>` states by sign. Negative versions represent `ptr<T, null>`, zero represents `ptr<T, unknown>`, and positive versions represent `ptr<T, valid>`. The magnitude is the MVCC generation; the sign is the pointer-state proof. Kernel helpers such as `page_version_state(version)` must be the single mapping authority.

| PageHeader `version` sign | Language state | Meaning |
|---------------------------|----------------|---------|
| `< 0` | `ptr<T, null>` | Known not to reference a live page version |
| `0` | `ptr<T, unknown>` | Unproven or transitional page state |
| `> 0` | `ptr<T, valid>` | Proven live MVCC generation |

4. **D4. Write-Ahead Log (WAL) Engine:** ACID Bedrock.
Implement the kernel-managed transaction log as a structural ring buffer tied directly to the underlying block storage device. Every transactional modification must serialize a log entry containing `(block_address, old_data, new_data, transaction_id)` before flushing dirty data frames to storage. Expose four structural system primitives: `log_write`, `log_commit`, `log_abort`, and `log_checkpoint`.

5. **D5. Relational State Store Initialization:** State Migration.
Construct fixed-size relational tables (Process Table, File Descriptor Table, Quotas Table) inside B-trees backed by the Buffer Pool and the WAL. **Critical Migration Trigger:** Once the relational store is online, loop through the initial Phase D2a bootstrap bitmap state, insert corresponding allocation rows into the new formal transactional memory map relation, and permanently deprecate the raw bootstrap allocator.

**Incremental Migration Protocol:** D5 must not depend on an unrecoverable stop-the-world migration. Once the WAL is online, migration advances through the bootstrap bitmap with a persistent cursor. For each allocated bootstrap page, the kernel logs an `allocations` row upsert before publishing the row. If power fails mid-scan, `log_recover()` replays committed rows, aborts pending rows, and resumes from the last durable cursor. The raw bootstrap allocator is disabled only after `REL_MIGRATION_DONE` is durably published.

The migration may temporarily throttle user-space allocation, but it must not leave the kernel in a state where interruption means corruption. All D5 state transitions must be either replayable through `log_commit` or discardable through `log_abort`.

6. **D6. Two-Tiered Scheduler: O(1) Micro-Engine:** Tier-1 Scheduling Micro-Mechanics.
To prevent relational lookup overhead from destroying context-switch times, split scheduling into two isolated tiers. Tier-1 is the micro-scheduler: implemented as a highly optimized, flat, non-relational O(1) run-queue array. The core timer-interrupt handler (D1) only interacts with this flat Tier-1 layer, popping the next runnable task thread in microsecond time horizons without touching any relational B-trees.

Tier-1 run-queue slots, head/tail counters, and queue counts are shared kernel state. Producers and consumers must use `TLDR` followed by `TSTR` with acquire-release ordering for slot claims and publications. A `TSTR` without a preceding reservation-setting `TLDR` on the same address is invalid.

7. **D7. Two-Tiered Scheduler: Macro-Reconciliation Loop:** Tier-2 Scheduling Policy.
Implement Tier-2 of the scheduler as a background macro-horizon reconciliation loop executing strictly every 10–50 ms. This loop queries the formal `desired_processes` and `quotas` relations, evaluates priority shifts and resource usage balances, and flushes the resulting top-tier runnable threads into the flat Tier-1 O(1) run-queue array.

The Tier-2 to Tier-1 handoff is an explicit synchronization boundary: the macro loop stages runnable thread IDs in a shadow queue, publishes them into Tier-1 with acquire-release `TLDR`/`TSTR`, then increments a scheduler epoch with release ordering. The timer path acquires the epoch before consuming newly published work.

8. **D8. VFS Isolation: Relational Control vs. Opaque Linear Data Plane.**

D8 delivers the native filesystem and file-descriptor syscall surface. After D8, a user process can open, close, read, write, stat, and enumerate files through syscall ids `12` through `17`; `exec()` in D9 can then consume ordinary executable files instead of static kernel images. D8 depends on the D3 Buffer Pool, D4 WAL, D5 relational state store, and D6/D7 scheduler. D8 must not depend on D9 COW/fork semantics.

To prevent transactional locking and metadata update overhead from throttling raw throughput, the Virtual File System is split into two isolated operational planes that share the underlying Buffer Pool and Write-Ahead Log (WAL) substrate. D8 defines only the filesystem contract. The VFS must not transform, pack, tile, reinterpret, compress, encrypt, or vector-align arbitrary payload bytes or words.

### D8a. Required Kernel Data Model

The VFS is backed by fixed-size relational rows stored in D5 B-trees. Persistent `vfs_*` row mutations are D4 WAL transactions. The volatile `process_fds` relation is synchronized but not replayed after reboot. All persistent VFS tables include `namespace_id`; D8 may run with only namespace `0`, but the column exists from the start so D9 namespace isolation is not retrofitted.

| Relation | Key | Required fields |
|----------|-----|-----------------|
| `vfs_mounts` | `(namespace_id, device_id)` | `root_inode`, `fs_version`, `block_words`, `flags` |
| `vfs_inodes` | `(namespace_id, inode_id)` | `kind`, `mode`, `size_words`, `link_count`, `version`, `ctime`, `mtime`, `exec_header_ptr`, `flags` |
| `vfs_dirents` | `(namespace_id, parent_inode, name_hash, name_words)` | `child_inode`, `entry_version` |
| `vfs_extents` | `(namespace_id, inode_id, logical_start_word)` | `length_words`, `ppn_start`, `extent_flags`, `extent_version` |
| `process_fds` | `(pid, fd)` | `namespace_id`, `inode_id`, `offset_words`, `open_flags`, `ref_count`, `fd_version` |

`kind` must at minimum encode `free`, `file`, `directory`, and `executable`. Directory names are metadata, not payload: path components are stored as T40 character words in `vfs_dirents`, are compared exactly, and are never locale-folded or normalized beyond the explicit path rules below. File payload extents are opaque data.

The block/page granularity for D8 is `MMU_PAGE_WORDS` words. Current VM systems use 27-word pages; the VFS must refer to the architecture constant rather than hardcoding `27` in algorithms.

### D8b. Mount, Format, and Root Rules

At boot, D8 mounts `block0` from the device tree into namespace `0`. If the device is empty and the boot policy permits formatting, the kernel creates a deterministic root filesystem with a root directory inode, a free-space relation, and a WAL checkpoint before user-space starts. If the device contains an incompatible magic, version, or page-word size, mount fails cleanly before `shell_main()` is entered.

The root directory is inode `0` within each namespace. Paths exposed to user-space are absolute, T40 null-terminated character-word strings. D8 must support `/`, ordinary component lookup, `.`, and `..`; `..` at namespace root resolves back to root and must not escape the namespace. D8 does not require symbolic links, hard links beyond `.` and `..`, rename, unlink, chmod, or mount stacking.

The kernel-internal VFS API must include:

```
vfs_format(device_id: T40) -> T1
vfs_mount(namespace_id: T40, device_id: T40) -> T1
vfs_create(namespace_id: T40, path: ptr<T40, user, valid>, kind: T40) -> T40
vfs_lookup(namespace_id: T40, path: ptr<T40, user, valid>) -> T40
vfs_open(pid: T40, path: ptr<T40, user, valid>, mode: T40) -> T40
vfs_close(pid: T40, fd: T40) -> T1
vfs_read(pid: T40, fd: T40, dst: ptr<T40, user, valid>, count_words: T40) -> T40
vfs_write(pid: T40, fd: T40, src: ptr<T40, user, valid>, count_words: T40) -> T40
vfs_stat(namespace_id: T40, path: ptr<T40, user, valid>, out: ptr<T40, user, valid>) -> T1
vfs_readdir(namespace_id: T40, path: ptr<T40, user, valid>, out: ptr<T40, user, valid>, max_words: T40) -> T40
vfs_fsync(pid: T40, fd: T40) -> T1
```

`vfs_create`, `vfs_format`, `vfs_mount`, and `vfs_fsync` are kernel-internal in D8 unless a later syscall ABI explicitly exposes them. They exist so tests, init image construction, and D9 `exec()` do not bypass the real VFS path.

### D8c. Syscall ABI Contract

D8 owns syscall ids `12` through `17`:

| ID | Name | Arguments in `r13-r18` | Success payload |
|----|------|-------------------------|-----------------|
| `12` | `sys_open` | `r13=path_ptr`, `r14=mode` | `fd` |
| `13` | `sys_close` | `r13=fd` | `0` |
| `14` | `sys_read` | `r13=fd`, `r14=buf_ptr`, `r15=count_words` | words read |
| `15` | `sys_write` | `r13=fd`, `r14=buf_ptr`, `r15=count_words` | words written |
| `16` | `sys_stat` | `r13=path_ptr`, `r14=out_ptr_or_zero` | file size in words |
| `17` | `sys_readdir` | `r13=path_ptr`, `r14=out_ptr`, `r15=max_words` | words written |

All D8 syscalls use the kernel `T1` status convention: `r13 = -1/0/+1`, `r14 = payload`, and `r15 = errno/detail`. `+1` means success, `0` means a non-fatal boundary condition such as EOF or would-block, and `-1` means failure. Existing one-register compatibility wrappers may collapse this triple into a single return value, but the routed kernel ABI must preserve all three registers.

`sys_open` mode is a small integer flag word:

| Mode | Meaning |
|------|---------|
| `0` | read-only, path must exist |
| `1` | read-write, path must exist |
| `2` | read-write, create file if missing |
| `3` | read-write, create if missing and truncate to zero words |

`sys_stat(path, 0)` returns the file size in `r14`. If `out_ptr_or_zero` is nonzero, it must validate as a writable user pointer and the kernel also writes a fixed stat record: `[inode_id, kind, size_words, extent_count, flags, version]`. The D8 `ulib.trit` compatibility wrapper for `stat(path)` must explicitly pass `0` as the second syscall argument; no D8 handler may read an omitted optional argument from stale register state. `sys_readdir` writes repeated directory records into `out_ptr`: `[child_inode, kind, name_len, name_word_0, ...]` until `max_words` would be exceeded.

### D8d. Pointer, Permission, and Error Rules

Every user pointer crossing the VFS syscall boundary must be validated as a `ptr<T40, user, valid>` span before any metadata or extent mutation occurs. Validation includes non-null state, user privilege, page presence, read/write permission appropriate to the syscall, namespace compatibility, and `count_words` bounds. A failed validation returns `ERR_BAD_PTR` and leaves all VFS state unchanged.

The minimum D8 errno/detail values are:

| Name | Meaning |
|------|---------|
| `ERR_NOT_FOUND` | path, inode, or extent missing |
| `ERR_EXISTS` | kernel-internal exclusive create requested an already existing path |
| `ERR_NO_SPACE` | extent allocation or relation row allocation failed |
| `ERR_BAD_FD` | fd is not open in the calling process |
| `ERR_BAD_PTR` | user pointer validation failed |
| `ERR_NOT_DIR` | directory operation encountered a non-directory |
| `ERR_IS_DIR` | file read/write attempted on a directory |
| `ERR_INVALID` | malformed path, mode, count, or filesystem state |
| `ERR_EOF` | read reached end of file |

Errors must be fail-stop. `sys_write` must not publish a shorter partial write unless it returns success with the exact shorter count for a documented boundary such as device EOF. For ordinary block files, D8 writes are all-or-error.

### D8e. Read Path Algorithm

`sys_read` must follow this order:

1. Validate `fd`, `buf_ptr`, and `count_words`.
2. Acquire the fd row with `TLDR/TSTR` acquire-release semantics and read `offset_words`.
3. Resolve a contiguous extent window from `vfs_extents` using `(namespace_id, inode_id, offset_words)`.
4. Pin every Buffer Pool page in the window; pages with `pin_count > 0` remain non-evictable under the D3 rule.
5. Copy payload words directly from physical extents into the validated user buffer without interpreting the payload.
6. Release pins.
7. Advance the fd offset with a `TSTR` compare-and-swap on `fd_version`.

The hot copy loop must not perform B-tree lookups per page once the extent window is known. If the requested range crosses an extent boundary, the kernel resolves the next extent window and repeats. EOF returns status `0`, payload `0`, detail `ERR_EOF`.

### D8f. Write Path Algorithm

`sys_write` must follow this order:

1. Validate `fd`, `buf_ptr`, `count_words`, write permission, quotas, and maximum file size before allocating pages.
2. Acquire the fd row with `TLDR/TSTR` acquire-release semantics and snapshot `offset_words`.
3. Resolve existing extents for overwrite ranges and reserve new extents for growth. New extent rows begin with an in-flight `extent_version` state and are not visible to readers.
4. Start a WAL transaction and log old/new images for every touched inode, extent, and free-space row before dirty frames can be flushed.
5. Copy payload words from the validated user buffer into Buffer Pool pages. The VFS must preserve word order exactly.
6. Publish extent rows and inode `size_words` at the commit boundary, then `log_commit`.
7. Advance fd offset and `fd_version` only after the write transaction commits.

For append mode, the offset snapshot is the committed inode size read inside the transaction. For truncate mode, the old extents are removed only after replacement metadata has been WAL-logged. Readers must see either the old committed version or the new committed version; they must never observe in-flight extent rows.

### D8g. Metadata Transactions and Recovery

All metadata operations are WAL-backed transactions. The WAL record set for a filesystem mutation must include enough old and new row images to make `log_abort` and replay deterministic. D8 recovery is complete only when these cases are handled:

* Crash before `log_commit`: discard in-flight rows and restore old metadata.
* Crash after `log_commit` but before checkpoint: replay committed inode, dirent, extent, and free-space row updates.
* Crash during root filesystem format: detect incomplete format by mount epoch and restart format or fail before user-space starts.
* Crash during truncate or overwrite: expose either the pre-transaction file or the committed replacement, never a mixed extent list.

The Buffer Pool dirty bit is not a commit signal. The authoritative visibility signal is the relation row version published by the WAL transaction.

### D8h. Concurrency and Scheduler Interaction

VFS metadata relation rows are shared kernel state and must be accessed as `shared<T, ACQ_REL>` or under locks implemented with `TLDR/TSTR` and `FENCE.0`. The minimum synchronization rules are:

* Directory and inode metadata mutations serialize per `(namespace_id, inode_id)`.
* Independent reads of committed file extents may proceed concurrently.
* Writes to the same inode serialize through the inode row version.
* Multiple tasks sharing an fd serialize offset changes through `fd_version`.
* Blocking disk I/O must park the task on a scheduler wait channel; it must never spin inside the D1 trap path or the Tier-1 scheduler hot path.
* Completion of a disk or WAL operation wakes blocked tasks by publishing the wait-channel state with release ordering; resumed tasks acquire it before continuing.

Single-core interrupt-disable sections may protect very short critical regions in the first implementation, but every D8 shared-state contract must already be expressible with `TLDR/TSTR` so the design remains valid for later multicore work.

### D8i. VFS Neutrality Contract

The VFS is a neutral router between file descriptors, namespace-filtered metadata, physical extents, and user buffers. This neutrality is a hard invariant:

* Reads return the same logical stream that prior writes committed.
* Writes commit the caller-provided stream without reinterpretation.
* File type, extension, metadata flags, executable status, or compiler annotations may not cause the VFS to change payload layout.
* Specialized runtimes may request aligned extents or larger sequential windows for performance, but the returned data remains opaque to the VFS.
* Any format-aware transformation belongs above the syscall boundary in user-space libraries, compiler-generated code, or explicit runtime services.

### D8j. Execution Boundary

The VFS responsibility terminates at delivering validated, namespace-authorized, cache-line-friendly physical extents to or from the Buffer Pool. The compiler and runtime own all decisions about sub-word layouts, vector-register loading, width-parametric functions, and `#[parallel]` execution paths. Kernel VFS code must remain correct when the payload is text, executable code, serialized relations, media data, model weights, or an unknown application-defined format.

### D8k. Required Acceptance Tests

D8 is not complete until the native kernel test suite proves all of the following:

* `format` + `mount` creates and remounts a deterministic root filesystem on `block0`.
* `vfs_create`, path lookup, `.`, `..`, and namespace-root clamping behave deterministically.
* `sys_open`, `sys_close`, `sys_read`, `sys_write`, `sys_stat`, and `sys_readdir` use ids `12-17` and the `r13/r14/r15` status triple.
* `read` after `write` returns exactly the committed word stream, including zeros, negative words, and 3-element packed data treated as opaque payload.
* Large files crossing at least three extent windows read back exactly and do not perform per-page B-tree metadata updates in the hot copy loop.
* Bad fd, invalid path, null pointer, unknown pointer, out-of-range span, directory-as-file, and EOF cases return the required status/detail without mutating metadata.
* A simulated crash before and after WAL commit proves recovery exposes only old or new committed metadata.
* Concurrent readers and serialized writers preserve fd offsets and inode versions under `TLDR/TSTR` acquire-release ordering.
* D9 `exec()` can load an executable file through the public VFS read/stat path without using any filesystem test bypass.

### D8l. IPC Primitive

Phase D includes a minimal IPC mechanism because native processes, drivers, and the judge/server split cannot be useful as isolated islands. The required primitive is a namespace-authorized shared ring buffer. Channel metadata stores the two allowed namespaces, buffer address, read cursor, write cursor, count, capacity, and version. `ipc_send` and `ipc_recv` publish cursor/count changes with acquire-release `TLDR`/`TSTR`, and failed namespace checks must leave the channel unchanged.

This IPC layer is deliberately small: it is sufficient for subprocess result delivery, driver messages, and future user-space protocols without entangling the scheduler, VFS, or compiler runtime with payload formats.

### Technical Guardrail for the Implementing Agent

> **Implementation Rule:** In `kernel.trit`, do not allow raw sequential read/write system calls (`sys_read`/`sys_write`) to invoke individual B-tree inserts or metadata updates per block.
> The user-space buffer pointer must be validated as a `ptr<T40, valid>` span and mapped to raw physical memory extents via the Buffer Pool. The VFS must deliver data as a transparent stream without enforcing arbitrary layout transformations on the payload. All relational index changes must be deferred and batched into a single WAL-backed atomic transaction at the operation boundary.

---

### Non-Normative Optimization Annex: 3-Element Sub-Word Layout

This annex is not part of the VFS contract. It is guidance for compiler, runtime, and application authors building specialized vector workloads on top of ordinary opaque file streams.

For data structures that naturally decompose into three coordinated components, such as spatial coordinates, color channels, or tensor slices, a runtime may choose to store three 9-trit short values inside each 27-trit machine word:

```
[ Word Layout: 27 trits ] -> [ 9-trit Short A | 9-trit Short B | 9-trit Short C ]
```

The Triton-27 CPU core defines a native hardware vector length of 27 lanes. A compiler/runtime pipeline using this layout can read 9 sequential words and present 27 logical sub-word elements to a vectorized compute path. This can improve cache behavior for specialized workloads, but it remains an application/runtime layout choice. The VFS must neither require nor infer this representation.

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

**Valid field zero state:** The `zero` state is a hard FAULT encoding in the hardware model, not a software guideline. On cold boot, DMEM initialises to zero, meaning all PTEs start as faulting entries. RTL page walking and software page walking must both reject `zero` exactly as they reject `neg`; only `pos` is present. No path may treat a zeroed PTE as a valid structure.

**MMIO trit behaviour:** Pages with the MMIO trit set to `pos` must be excluded from COW duplication, fork propagation, and snapshot inclusion. The COW fault handler, the fork path, and any snapshotting mechanism must check this trit before operating on a page. The `pin_count` constraint from D3 also applies to MMIO-mapped pages — an MMIO page is implicitly pinned for the lifetime of the MMIO mapping and must not be evicted.

Implement `fork()` as an O(1) metadata operation that duplicates PTEs and marks non-MMIO pages as COW, postponing physical duplication until write-fault entry. Enforce namespace filters automatically during table queries. To bypass system call traps for reading static data, maintain a process-local memory cache of observed state; mutations invalidate this cache via a fast hardware `FENCE.0` sequence. Conclude by initializing the user-space `shell_main()`.

---

## Phase E — Persistent Memory and Disk-Backed System Substrate

Status: completed for the current persistent-substrate acceptance scope.
Validated by the OS platform and process-handoff tests, with persistence
coverage for block geometry, root image construction, mount/recovery, `fsync`,
WAL replay, and disk-backed executable launch.

Phase E makes the Phase D kernel state durable. Phase D proves the native kernel can schedule, isolate, transact, and route file/process syscalls. Phase E proves that the same system can survive reboot, boot from a filesystem image, persist user-visible writes, replay committed WAL records, discard uncommitted WAL records, and load executable process images from disk rather than from static assembly placement.

Phase E is intentionally not a new VFS contract. D8 owns the VFS semantics. Phase E is the persistence substrate underneath and around that VFS: block-device geometry, root image construction, WAL-on-disk layout, native mount/recovery, `fsync`, and disk-backed `exec` handoff.

### E1. Block Device and Device-Tree Geometry

The VM must expose a block device through CSRs with explicit geometry:

| CSR | Meaning |
|-----|---------|
| `block_count` | number of addressable blocks |
| `block_words` | words per block, matching `MMU_PAGE_WORDS` in the initial implementation |
| `block_index` | selected block number |
| `block_addr` | DMEM/IMEM transfer address |
| `block_cmd` | read, write, or IMEM-read command |
| `block_status` | last command result |

The device tree must advertise `block0` with `block_words` and `block_count`. Kernel boot must reject incompatible geometry before mounting the root filesystem. Tests must prove that block writes mark dirty state, block reads recover the same payload, and serialized block images survive VM reboot.

### E2. Native Disk Layout

The native disk image is a deterministic block layout containing:

| Region | Required content |
|--------|------------------|
| superblock | native VFS magic, version, block size, row capacities, next allocation cursors |
| inode blocks | fixed-width inode rows |
| dirent blocks | directory edge rows |
| dirent-name blocks | T40 name-word storage |
| extent blocks | file extent rows |
| payload blocks | ordinary file payload words |
| WAL meta block | persisted WAL cursor and checkpoint state |
| WAL record blocks | persisted pending/committed transaction records |
| executable text blocks | app text images beyond the fixed root metadata region |

The initial implementation may use fixed metadata capacities, but the disk format must make those capacities explicit in the superblock so Phase F can scale them without silent image incompatibility.

### E3. Format, Mount, Sync, and Recovery

The kernel must provide durable helpers:

```
vfs_sync_superblock() -> T1
vfs_sync_all_to_disk() -> T1
vfs_load_all_from_disk() -> T1
wal_sync_to_disk() -> T1
wal_load_from_disk() -> T1
vfs_fsync(pid: T40, fd: T40) -> T1
```

`kernel_init()` mounts the disk image if a valid superblock exists. If no valid image exists and boot policy allows formatting, it creates the deterministic root layout and checkpoints it before user processes begin.

Recovery rules:

* A pending WAL record without commit is rolled back or ignored.
* A committed WAL record not yet checkpointed is replayed into the VFS image.
* `vfs_fsync` flushes the touched persistent VFS ranges and WAL state to the block device.
* The kernel must never claim a write is durable until both metadata and payload ranges required by that write can be recovered after reboot.

Phase E exposes `sys_fsync` as syscall id `47`, with SDK wrappers in `os_sdk.trit` and `ulib.trit`, so user programs can explicitly publish data durability.

### E4. Root Filesystem Image Builder

The host-side image builder must construct native root images without bypassing the real disk layout. Required builder features:

* Install deterministic base directories such as `/bin`, `/apps`, `/etc`, `/home`, `/tmp`, and `/var`.
* Add ordinary files and directories through the same inode/dirent/extent model used by the kernel.
* Add executable descriptors and executable text blocks.
* Serialize the final image into VM block-device words.
* Support reboot tests by loading the serialized image into a fresh VM.

This builder exists to seed boot images, not to define alternate filesystem semantics. Anything it creates must be mountable and readable through the native kernel VFS.

### E5. Disk-Backed Process Image Handoff

`exec()` and app launch must consume ordinary executable files from `/bin`. Executable file metadata includes an executable header plus disk text-block location. Launch must:

1. Resolve the app path through VFS lookup.
2. Validate the executable descriptor and ABI version.
3. Load text pages from disk into per-process IMEM using the block device.
4. Map data, scratch, stack, window buffers, and user-visible DMEM through per-process page tables.
5. Install the task context and return through `ERET` into the loaded user image.

The desktop and launcher tests must prove `/bin/desktop`, `/bin/calculator`, and windowed app probes are loaded from the disk image into IMEM, not appended into the boot assembly through `.org`.

### E6. Required Acceptance Tests

Phase E is complete only when tests prove:

* device-tree block geometry validation;
* block read/write/dirty state and reboot image reload;
* root filesystem image creation and native kernel mount;
* file write + `fsync` + reboot + readback;
* uncommitted WAL record recovery leaves the old committed data visible;
* committed WAL record recovery replays the new data;
* executable root image builder installs `/bin/*` images;
* desktop-triggered process handoff loads text pages from disk into IMEM;
* `sys_fsync` is exported through VM constants, compiler runtime ids, `os_sdk.trit`, and `ulib.trit`.

---

## Phase F — Production OS Surface and Consumer Experience

Status: completed for the current production-surface acceptance scope.
Validated by `ci_production`, including production layers, production
hardening, OS platform, scaling profile, and consumer shell productization.

Phase F turns the durable native OS into a consumer-product-shaped system. It includes the six-layer GUI work because Layers 5 and 6 are explicitly widget toolkit and consumer shell work, and Layers 1 through 4 are prerequisites for those to feel real. If the GUI work later grows large enough to require its own schedule, split it into a sub-track under F rather than moving self-hosting forward; self-hosting remains the final phase.

### F1. Scaling Pass

Target minimum profile:

```
2 virtual cores
4 GiB RAM-equivalent addressable memory
64 GiB sparse disk image
production-scale process, file, window, IPC, socket, and framebuffer limits
```

Required work:

* Define a production profile shared by VM, kernel, image builder, and tests.
* Replace flat always-allocated IMEM/DMEM with sparse page backing.
* Add larger OS allocation clusters above the 27-word hardware page.
* Replace in-memory block vectors with sparse file-backed storage.
* Move native kernel fixed limits (`PROCESS_MAX`, `VFS_MAX_INODES`, `WINDOW_MAX`, etc.) to profile-derived or dynamically allocated tables.
* Add two-core VM execution with complete per-core architectural state.
* Add per-core run queues and cross-core load balancing.
* Add stress tests for at least 100 processes, 1000 files, 100 windows, high-address RAM access, and high-block disk access.

### F2. Blocking IPC, Futexes, and Event Wait

Required work:

* Add wait queues keyed by wait channel.
* Add `sys_futex_wait(addr, expected, timeout)` and `sys_futex_wake(addr, count)`.
* Key futex wait channels by namespace/process/physical page identity, not raw virtual address alone.
* Add blocking IPC receive with timeout.
* Add blocking window/input event waits with timeout.
* Integrate wait completion with the scheduler so blocked tasks leave the runnable queues.
* Wake blocked tasks on IPC send, window event delivery, input arrival, signal delivery, timeout, and resource destruction.
* Convert GUI apps away from busy polling.

### F3. Signals and Process Control

Required work:

* Add process states for stopped, zombie, killing, crashed, and exited processes.
* Add pending signal masks per process.
* Add `sys_kill`, `sys_suspend`, `sys_resume`, and `sys_getproc`.
* Enforce ownership/capability checks so ordinary apps cannot kill arbitrary processes.
* Deliver close-request events to window owners before forced termination.
* Force-kill cleanup must release file descriptors, windows, IPC channels, sockets, wait queues, quotas, and memory mappings.
* Parent/child wait must reap zombies deterministically.
* Task Manager must use these syscalls for kill/suspend/resume controls.

### F4. Six-Layer GUI Stack and Widget Toolkit Completion

The six GUI layers belong here:

```
Layer 1: Framebuffer and display driver
Layer 2: 2D graphics primitives
Layer 3: Font and text rendering
Layer 4: Compositor, z-order, window buffers, input routing
Layer 5: Widget toolkit
Layer 6: Consumer shell
```

Required work:

* Framebuffer syscalls allocate, pin, flip, and expose DMA-safe buffers.
* `libgfx.trit` implements plot, fill, line, circle, blit, and blend primitives.
* Bitmap text rendering supports labels, titles, and text fields before vector fonts.
* The compositor owns the master framebuffer, window table, z-order, dirty flags, close events, and focused input routing.
* `libwidget.trit` completes Label, Button, TextField, PasswordField, ListBox, ScrollBar, Panel, focus, dirty rectangles, and event routing.
* Calculator, Paint, Task Manager, File Manager, Settings, Terminal, and Desktop use the shared widget toolkit instead of one-off drawing code.

### F5. Consumer Shell Productization

Required work:

* Boot splash and first-run setup.
* Login backed by persistent user records.
* Desktop with taskbar, launcher, clock, window switching, and logout/shutdown.
* File manager backed by the VFS.
* Settings app for display, storage, users, and system information.
* Terminal/shell as a windowed app.
* App registry under `/apps` and executable images under `/bin`.
* Persistent user preferences.
* Crash dialog and not-responding force-close flow.
* Reboot tests proving users, preferences, files, and app registry survive shutdown.

### F6. Production Hardening

Required work:

* Capability checks for process control, files, windows, IPC, sockets, and device access.
* Syscall fuzzing for invalid pointers, invalid states, and boundary sizes.
* Crash-consistency tests for power loss during filesystem and WAL phases.
* Memory-pressure and quota-pressure tests.
* Scheduler fairness and starvation tests.
* Filesystem consistency checker and recovery mode.
* Signed executable metadata and package/update format.
* Performance counters for scheduler, disk, compositor, and syscall paths.
* Release image builder that produces a bootable disk image from source artifacts.

---

## Phase G — Distribution, Host Runtime, and Bootstrapping

Status: in progress for Path A desktop distribution. Path B UEFI/live USB
bootstrapping remains planned.

Phase G packages the completed native OS into bootable, distributable artifacts.
This phase is not a new guest OS feature phase. It defines the boundary between
the ternary guest world and the binary host world: image formats, host runtime
contracts, release packaging, and the first bare-metal UEFI host runtime.

Phase G is split into two tracks:

* **Path A — Desktop Host Runtime:** package the `run_gui_console`/VM path into
  a polished binary-host desktop app with prebuilt boot images, framebuffer
  presentation, sparse `.tdisk` mounting, app launcher handoff, diagnostics,
  and Windows-first packaging. This is the active shippable path.
* **Path B — UEFI / Live USB Host Runtime:** boot a binary-host ternary VM from
  firmware using GOP framebuffer output and FAT-loaded release images. This
  remains future work and is still emulation on binary hardware.

Completed Path A implementation work includes:

* profiling foundation for VM/kernel/app hot-path discovery;
* prebuilt boot images and no compile-at-launch product path;
* VM decode cache and basic block cache;
* guest TLB and memory fast paths;
* compiler branch reduction, including `TSEL` branch-elimination targets;
* graphics dirty rendering for framebuffer/texture updates;
* disk/cache optimization for sparse disk and persistence behavior;
* optional trace JIT implemented behind a fallback-safe execution backend.

### G1. Boot and Disk Image ABI

Define stable release artifacts:

| Artifact | Role |
|----------|------|
| `.tboot` / `.tiso` | immutable boot/package image containing manifest, kernel, app images, root filesystem seed, ABI/profile metadata, checksums, and signatures |
| `.tdisk` | mutable sparse user disk containing persistent user state |
| `.tsnap` | optional VM snapshot/checkpoint for debugging and fast resume |

The immutable boot image and mutable user disk must remain separate so updates,
factory reset, user backup, and reproducible tests do not overwrite each other.

### G2. Release Image Builder

The release builder must compile the native kernel and selected applications,
install `/bin`, `/apps`, `/etc`, `/home`, `/tmp`, and `/var`, write the app
registry, generate the image manifest, sign or checksum every image component,
and emit a versioned `.tboot/.tiso` plus initial `.tdisk`.

The builder must use the same executable image and VFS layout exercised by
Phase E. It may automate packaging, but it must not define a second filesystem
or alternate app loading path.

### G3. Desktop Host Runtime

Build a user-facing binary-host runtime around the ternary VM. Required host
runtime features:

* framebuffer-to-window rendering;
* keyboard and mouse input routing into the guest device model;
* sparse `.tdisk` mounting;
* pause, reset, shutdown, and restart controls;
* guest crash capture and diagnostic bundle export;
* deterministic replay seed or trace capture for reproducible bugs;
* VM profile selection for compact and production configurations.

This is the first distribution target because it can ship on ordinary Windows,
Linux, and macOS systems without replacing the host boot chain.

Completed Path A optimization passes:

* execution profiles are collected before major optimization passes;
* prebuilt release images boot by default instead of compiling guest `.trit`
  sources at startup;
* decoded instructions and basic blocks are cached while preserving interpreter
  fallback behavior;
* guest address translations and validated syscall spans are cached with
  precise invalidation on page-table, `exec`, `fork`, COW, and privilege
  changes;
* guest branches are reduced through compiler if-conversion and `TSEL` where
  arms are side-effect-free and cheap;
* host framebuffer textures update from dirty regions instead of repainting the
  entire display when possible;
* sparse disk/cache writes are batched without weakening WAL or `fsync`
  durability;
* trace JIT remains optional and fallback-safe while matching interpreter
  behavior on golden programs.

### G4. Desktop Installer Packaging

Package the desktop host runtime and release images:

* Windows installer first, using an ordinary setup executable or MSI pipeline.
* Linux AppImage/deb/rpm packaging after the Windows path is stable.
* macOS `.app`/`.dmg` packaging once the runtime has a stable graphics/input
  abstraction.

Installers must include a signed release manifest, default `.tboot/.tiso`, and
an initialized or lazily-created `.tdisk`.

### G5. UEFI Bare-Metal Host Runtime

Prototype an x86-64 UEFI host runtime that runs the ternary VM directly after
firmware boot. Required minimum:

* load the release image from a FAT filesystem;
* query the UEFI GOP framebuffer and expose it to the guest display path;
* route keyboard input into the guest input device;
* provide a timer source for guest scheduling;
* expose a block-device bridge backed by the boot medium or an attached image;
* enter the same ternary VM execution loop used by the desktop host runtime.

This is still emulation on binary hardware, not native ternary execution. Its
value is that it removes the dependency on a host OS and makes a live USB
experience possible.

### G6. Live USB and Dual-Boot Acceptance

Live USB comes after the UEFI runtime can boot the graphical desktop reliably.
Dual-boot integration comes last because it modifies existing host boot
configuration.

Phase G is complete only when tests or documented manual runs prove:

* release images are built reproducibly from source;
* the desktop host runtime boots the release image and persists user state in
  `.tdisk`;
* crash diagnostics can be exported from the host runtime;
* at least one desktop installer or reproducible package is generated;
* the UEFI host runtime loads the same image format and reaches a visible
  framebuffer;
* live USB creation is documented and tested on a development machine or
  emulator target.

---

## Phase H — Self-Hosting

Self-hosting remains last. The native compiler should not become the maintenance authority until the persistent disk substrate, scaled OS profile, blocking waits, process control, GUI stack, consumer shell, production hardening, and distribution host runtime have stable acceptance tests. Otherwise the project risks debugging compiler self-hosting failures and OS product/distribution failures at the same time.

* **H1. Bootstrapping Evaluation:** Compile `tcl_frontend.trit`, `tcl_backend.trit`, and `ulib.trit` using the legacy Phase A C++ compiler. Run the compiled native binary inside the VM environment to compile a test program. The output must be bit-identical to the C++ compiler's native output.
* **H2. Secondary Self-Hosting Loop:** Compile `tcl_frontend.trit` and `tcl_backend.trit` using the native compiler binary generated in step H1. If the compiler binaries from H1 and H2 match identically down to the bit level, the compiler toolchain is officially self-hosting.
* **H3. Demolition:** Decommission the legacy C++ compiler source. All subsequent alterations to the compiler toolchain, language specifications, transactional operating system kernel, consumer OS, and distribution system are written and maintained exclusively in native `.trit` source code.

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
                                                                                  E
                                                                                  |
                                                                                  F
                                                                                  |
                                                                             G1 -> G2 -> G3 -> G4 -> G5 -> G6
                                                                                                      |
                                                                                                H1 -> H2 -> H3
```

> **Critical Guardrail for Implementing Agent:** Do not attempt to optimize early phases by building high-level relational paradigms prematurely. Phase D must follow the structural sequence exactly: the raw bootstrap page array (`D2`) and basic buffer pages (`D3`) must be completely stable before the transactional relational store engine (`D5`) is initialized.
