# From v1 Bring-Up to v2: A Ternary Stack Architecture Revision Study

## Abstract

TernaryOS v1 was a bring-up architecture. Its first job was to make a complete
silicon-to-software path observable: fetch a 27-trit instruction, represent
ternary values, run a compiler, boot a kernel, mount a root filesystem, and
exercise processes, graphics, storage, and host tooling. That goal favored
flexibility, short implementation paths, and broad experimentation.

TernaryOS v2 is the first architecture intended to remain stable while the
stack grows. It fixes scalar and memory geometry, compacts the direct opcode
space, makes ABI and image metadata explicit, replaces ambiguous memory and
page-table representations, separates durable control state from transient
runtime state, makes scheduling and waiting constant-time, and makes optimized
SSA the compiler's only code-generation source. It also introduces a portable
micro-op executor, a guarded x86-64 native JIT, measured workload gates, and a
clean migration boundary.

The important lesson is not that v1 was irrational. It is that a successful
bring-up exposes the contracts that must become architectural before a system
can scale. This paper records those contracts as study material and turns the
revision into testable engineering questions.

## 1. Scope and provenance

This paper describes two states of the project:

- **v1** means the frozen historical implementation at the Git tag
  trit-v1-final. It includes the original assembler, VM, compiler, kernel,
  image formats, and bootstrap programs such as minimal_kernel_bringup.tasm.
- **v2** means the current architecture described by the manifests and v2
  contract documents, including the later ABI v3/vector boundary work where
  applicable. The authoritative sources are ARCHITECTURE_MANIFEST.json,
  IMAGE_FORMAT_MANIFEST.json, SYSCALL_MANIFEST.json, TEST_MANIFEST.json, and
  the generated pages in this vault.

Some v1 documents are design notes rather than a formal specification. In
particular, the v1 notes disagree about whether a data-memory word is T40 or
T50 and describe page-table flags separately in more than one way. Those
disagreements are themselves useful test cases: an implementation cannot
derive a stable ABI from prose that permits multiple physical layouts.

The reasons attributed to v1 below are engineering inferences from its code,
tests, comments, and the order in which features were added. They are not
claims about the personal intent of every original author.

## 2. The v1 problem statement

V1 tried to prove that a ternary machine could support the whole software
stack, even though the host executing it was binary. The shortest useful
feedback loop was more valuable than a fully frozen architecture. The stack
therefore optimized for:

1. **Visible progress.** A small VM, a compiler, a kernel, and a boot image
   could be exercised together.
2. **Representation freedom.** The runtime could carry several ternary widths
   and lanes without first deciding how every width would map to silicon.
3. **Feature discovery by implementation.** Scalar, lane, vector, reduction,
   accumulator, and AI operations could be tried as direct opcodes.
4. **A complete vertical slice.** The kernel supplied processes, IPC, VFS,
   graphics/input hooks, partial networking, and a disk-backed state model
   before each subsystem had a final durability or scheduling contract.
5. **Host practicality.** Binary-host data structures and helper calls made
   ternary arithmetic and memory translation straightforward to debug.

That is a reasonable strategy for a research bring-up. It becomes expensive
when the same provisional choices cross compiler, executable, VM, JIT, kernel,
disk, and diagnostic boundaries.

## 3. V1 architecture

### 3.1 ISA and machine representation

The instruction was a fixed 27-trit word. Each trit used a two-bit host
encoding, with the unused two-bit pattern treated as invalid. The machine had
27 general registers, a separate trap register, separate instruction and data
memories, balanced signed immediates, and a Harvard-style fetch/execute path.
The register field bias made compact register fields fit naturally in the
instruction word.

The v1 instruction map assigned direct opcode numbers to a wide portfolio:
ordinary scalar operations occupied the low range, while lane, advanced scalar,
vector, reduction, accumulator, and AI operations also received direct
numbers. The map reached opcode 79 before the reserved range beginning at 80.
This was useful for experimentation: adding an operation was conceptually
similar to adding another Opcode enum entry and another decoder case.

The runtime value was a tagged TernaryValue. It could represent T1, T5, T10,
T20, T40, T50 and corresponding lane forms, with raw payloads held in host
integer storage. A memory word could therefore carry both a value and a width
or mode. This made generic arithmetic and early tests convenient, but it also
meant that “what bits are in the word” and “how should those bits be
interpreted” were coupled.

The v1 VM used a large dense guest data memory by default, with a separate
sparse host backing option. The host's sparse page size was an implementation
detail and was not the same as the guest MMU page size. Scalar memory
instructions converted numeric address values through helpers; vector memory
instructions accumulated lane-local fault information. Atomic load/store
forms and a fence were present, but the exact ownership of reservations,
generations, and fault state was still evolving.

### 3.2 Memory management and page tables

V1 used 27 guest words as the MMU page unit, matching the disk/cache transfer
block. Page-table state was represented with a physical page number and
separate flag information in the v1 design notes. Translation caches existed,
and context switching updated MMU-related CSRs, but the later v2 contract for
ASIDs, page sizes, accessed/dirty state, permission priority, and shootdowns
was not yet fixed.

The small page size was easy to reason about in a first implementation and
made a page-table entry fit naturally into early data structures. Its cost was
translation pressure: a large working set required many page translations, and
the page-table representation had more than one possible encoding.

### 3.3 Compiler and object generation

The v1 compiler had a typed AST and several useful optimizations, including
constant folding, strength reduction, peephole transformations, label
generation, and special handling for selected AI kernels. Its normal mental
model was a recursive AST visitor:

1. visit an AST node;
2. emit ternary IR or assembly for that node;
3. release temporary registers as the visitor returns;
4. perform a dry pass or local allocation step;
5. emit the real function.

The approach was productive because diagnostics, lowering, and assembly were
close together. It was not a single authoritative optimization pipeline:
some code could be emitted while the AST was being walked, and the v1
compiler explicitly recorded that spill lowering was not implemented. Global
control-flow properties such as dominance, loop structure, call-clobber
interference, and predecessor-specific phi values were not yet enforced by one
verified IR.

### 3.4 Kernel and operating-system substrate

The v1 kernel was a monolithic TCL program with process, VFS, block-I/O, HAL,
network, and window/input components. The boot path loaded a .tboot image,
unmarshalled an embedded root filesystem, initialized process state, and
spawned the first user process. minimal_kernel_bringup.tasm and related
bootstrap/trap stubs were intentionally small programs for proving that
instruction fetch, traps, and kernel entry worked before the full kernel was
available.

The process model included a PCB, register/context snapshots, per-process
memory bounds, file descriptors, parentage, IPC queues, and timer-driven
preemption. Scheduling used priority/runnable structures and a wait/event
model, but the later v2 guarantees about slot ownership, queue generations,
FIFO waiting, PID indexing, and empty-queue architectural WAIT were not yet
uniformly enforced.

The VFS was inode based. It supported the core file, directory, stat, read,
write, directory, unlink, and fsync operations, with namespaces, quotas, and
partial networking layered around it. The early system favored fixed tables,
monotonic cursors, and predictable memory addresses over reclamation and
fully concurrent allocation.

### 3.5 Storage and WAL

The v1 block-I/O layer used small rows describing a transaction, block, old
value, new value, and state. The kernel WAL had a modest in-memory capacity and
disk metadata for replay. This was enough to demonstrate that a write could be
recorded and recovered, and it provided a foundation for disk-backed tests.

The design still treated too many kinds of state as if they needed the same
transaction machinery. Transient process/scheduler state, allocation cursors,
and durable filesystem metadata did not yet have a crisp policy boundary.
The root filesystem was also closely tied to the boot image and legacy image
readers, which simplified bring-up but increased format coupling.

### 3.6 Host runtime and execution

The interpreter was the reference execution path. Trace and host-helper
facilities were added to reduce the cost of repeated instruction decoding, but
the early acceleration path was not yet a host-neutral micro-op contract with
precise side exits, W^X code pages, complete cache-generation keys, and
wall-time acceptance gates.

The host runtime supplied image loading, SDL-oriented display/input hooks,
diagnostics, and native test drivers. These made the whole stack observable,
which was more important than peak speed during v1 bring-up.

## 4. What v1 got right

V1 proved several high-risk ideas that were prerequisites for v2:

- a 27-trit instruction could be fetched, decoded, trapped, and tested;
- ternary arithmetic, multiple widths, lanes, and vector-like operations could
  be represented on a binary host;
- a compiler could produce programs that booted inside a ternary VM;
- the kernel could create processes, schedule them, exchange IPC, expose a VFS,
  and drive a host display/input loop;
- a boot image could carry enough state to start the system;
- disk state could be logged and recovered well enough to guide the later WAL
  design;
- the bootstrap assembly path could isolate hardware bring-up from the full
  compiler and kernel.

Those accomplishments explain why v1 should be preserved as a historical tag
and fixture source rather than deleted conceptually.

## 5. Why v1 stopped scaling

The problems were primarily cross-layer contract problems.

### 5.1 Representation ambiguity

A tagged value is convenient inside an interpreter, but it is a poor universal
ABI. A register, a memory word, a PTE, a syscall argument, and a host JIT
operand need predictable width and invalid-value behavior. V1's width tags and
the disagreement between v1 documents about T40 versus T50 memory words made
that boundary ambiguous.

### 5.2 Opcode pressure and feature coupling

Putting every experimental operation in direct opcode space made the decoder
and assembler grow with every experiment. It also consumed the space needed
for future operations before workload evidence existed. A direct opcode had
become a permanent architectural promise even when an operation was only
useful to one prototype.

### 5.3 Compiler phase coupling

When AST traversal can emit target code, optimization, diagnostics, register
allocation, and target selection can disagree. Missing spill rewriting and
weak predecessor/phi contracts turn a local compiler change into a runtime
debugging problem. The compiler needed one verifiable intermediate language,
not several partially authoritative paths.

### 5.4 Durability and scheduling policy

A small pointer-style WAL can prove replay, but it does not by itself define
group commit, after-images, no-steal ordering, inode-scoped fsync, or which
state is durable. Likewise, a runnable queue can produce progress without
proving O(1) ordinary scheduling, exactly-once wakeups, fairness, or idle
waiting without a busy loop.

### 5.5 Compatibility surface

Supporting old binaries, old image formats, old PTE encodings, old syscall
semantics, and new semantics simultaneously would require branches in the
assembler, loader, VM, JIT, kernel, and tests. That is a large security and
correctness surface for a young system whose v1 contracts were not yet
stable.

## 6. V2 design: freeze the contracts first

V2 replaced provisional cross-layer behavior with explicit contracts. The
following table summarizes the major revisions.

| Area | V1 bring-up choice | V2 contract | Engineering reason |
|---|---|---|---|
| Scalar word | Tagged multi-width values | Fixed T40 scalar and memory word; T1/T5/T10/T20 are normalized views | Predictable arithmetic, memory, ABI, and native lowering |
| Wide value | T50 as another tagged width | Two consecutive T40 words/registers, low part first | Explicit pair constraints and spill/ABI rules |
| Direct ISA | Many operations assigned 0–79 | Direct 0–37, reserved 38–79, EXT at 80 | Preserve decode space and make extensions discoverable |
| Feature support | Decoder knowledge implied support | Feature word and read-only discovery CSRs | Reject unsupported code before execution |
| Function ABI | Evolving register conventions | Fixed r1–r12 callee-saved, r13–r24 caller-saved, r25 link, r26 stack; explicit T50 return | Compiler, SDK, kernel, and tests share one ABI |
| Images | Legacy-friendly headers and embedded assumptions | Metadata-bearing tboot v3 and tDisk v2 | Loader can validate architecture before running |
| MMU page | 27 words | 729-word base page and 19,683-word superpage | Lower translation pressure while retaining 27-word transfer blocks |
| PTE | Separate/ambiguous fields | One numeric T40 with defined flag and PPN trits | Canonical serialization and simple validation |
| TLB | Early translation caches | I/D 27-entry L1, unified 243-entry L2, ASID/global/superpage state | Measurable hit behavior and precise invalidation |
| WAL | Small old/new pointer records | 729-block redo ring, checksummed superblocks, 12-word header plus 15 after-images | Crash recovery, grouping, and no-steal ordering |
| Durability policy | Broad transaction ambition | WAL only for durable control-plane state | Keep scheduler/process/usage fast and reconstructible |
| fsync | General disk synchronization | WAL through inode LSN, target inode data/metadata, one barrier | Correct inode-scoped durability without global flushes |
| Allocation | Monotonic rows and early tombstones | WAL-atomic reservations, reusable tombstones, inode generations, open-unlink reclaim | Avoid leaks, ABA reuse, and stale buffer data |
| Scheduling | Runnable/wait structures still evolving | Slot-owned O(1) tier-one queue, PID index, FIFO waits, architectural WAIT | Stable latency and no ordinary process-table scan |
| Compiler | AST-oriented emission and local allocation | CFG IR, verified SSA, global optimization, graph coloring/coalescing, spill rewrite | One source of truth and global correctness |
| Portable execution | Interpreter plus ad hoc tracing | Host-neutral decoded micro-op executor with guards and side exits | Same semantics on every host |
| Native execution | Helper-heavy acceleration | W^X x86-64 JIT with direct hot operations and precise fallback | Speed without losing trap/PC/count fidelity |
| Compatibility | Live v1 execution and image reading | Clean v2 runtime; source migration and offline image conversion | Remove pervasive legacy branches while preserving history |

### 6.1 Fixed scalar and ABI geometry

Every architectural scalar word is T40. Narrow operations operate on low-trit
views and clear unused upper trits. T50 is a pair of adjacent T40 words or
registers. The function ABI assigns general arguments to r13–r18, syscall
arguments to r13–r16, returns to r13 or r13:r14, and uses a nine-word stack
alignment. The later ABI v3 work adds an explicit caller-owned aggregate
return pointer and a defined vector boundary rather than silently treating
vectors as scalar arguments.

The width is deliberate: v2's explicit design goal is a **ternary equivalent
of a 64-bit computer**. T40 is the largest whole-trit word whose valid ternary
state space fits within 64 bits (`3^40 < 2^64 < 3^41`). This is an equivalence
of native scalar capacity and general-purpose software role, not an assertion
of binary compatibility or the physical size of every host/wire encoding. The
T27 instruction format, 27-lane vector geometry, and paired T50 extended values
remain separate architectural axes.

This makes a compiler spill, a syscall marshal, a VM register write, and a
native JIT store agree about the same physical object.

### 6.2 Compact ISA and extension groups

V2 keeps the stable scalar core and adds explicit control, CSR, MMU, and WAIT
operations. Advanced scalar, lane, vector, reduction, and accumulator/AI
operations move behind EXT selectors with documented layouts. Old v1 opcode
numbers can remain stable as extension selectors without consuming direct
decode space. An unassigned selector traps as illegal, and optional operations
require a matching feature declaration.

The result is a smaller mandatory decoder and a measurable process for
promoting an operation: it must remove a recurring sequence, be selected by the
compiler, match VM and RTL semantics, improve representative workloads, and
justify its area cost.

### 6.3 MMU, TLB, and precise memory faults

The base page is 729 words and the superpage is 19,683 words; the disk/cache
transfer block remains 27 words. A PTE is one numeric T40 word with explicit
present, user, read, write, execute, global, accessed, dirty, and superpage
trits followed by the physical base-page number.

Separate instruction/data L1 TLBs and a unified L2 TLB are keyed by ASID,
virtual page, page size, and global state. Page walks update accessed/dirty
state, permission faults have defined priority, and TLBINV carries address,
ASID, and scope. Vector and gather/scatter operations preflight all active
lanes before changing architectural state, recording the first failing lane
and complete fault mask.

### 6.4 Storage as a control-plane redo system

The v2 WAL is a 729-block ring with two checksummed superblocks. Each 27-word
record has a 12-word header and up to 15 after-image words. Data, commit, and
checkpoint records are replayed in LSN order. Incomplete transactions are
ignored, and recovery stops at the first torn or invalid block.

The log is durable where durability matters: VFS metadata, namespace policy,
quota limits, snapshots, and reconciliation state. Scheduler queues, process
state, quota usage, and physical allocation truth are reconstructed or
reconciled. A dirty frame whose LSN is newer than the durable log cannot be
written. fsync(fd) flushes the WAL through that inode's required LSN and then
only the inode's dirty data/metadata.

The VFS reservations, create/mkdir, write growth, truncate/unlink reclaim,
open-unlink lifetime, and bounded rename paths are now tested as transactions.
Inode generations and buffer invalidation prevent a recycled inode or data
span from exposing stale state.

### 6.5 Compiler as an IR-first system

The v2 compiler pipeline is:

1. typed AST to address-based CFG IR;
2. predecessor/successor construction;
3. dominators and dominance frontiers;
4. mem2reg and explicit predecessor/value phi inputs;
5. SSA, type, effect, and dominance verification;
6. global optimization;
7. phi, T50, aggregate, and vector lowering;
8. CFG-wide liveness;
9. graph coloring, coalescing, and iterative spill rewriting;
10. target selection and object emission.

The optimized SSA module is the sole source for object generation. Unsupported
constructs fail closed rather than silently replaying the AST. The deterministic
compiler corpus gate measures correctness, repeated instruction counts, and
optimization benefit; the current bounded corpus demonstrates a median dynamic
instruction reduction above the v2 acceptance target without a workload
regression.

### 6.6 Portable executor and x86-64 JIT

The portable executor translates instructions to a host-neutral micro-op form
with architectural PC, instruction accounting, guards, memory effects, trap
points, and side exits. It remains the semantic fallback.

The x86-64 backend uses RW allocation followed by RX finalization, never
write-plus-execute pages. It directly emits the proven hot subset—moves,
copies, integral T40 arithmetic and comparisons, raw T40 unary operations,
guarded dense physical loads/stores, internal branches, and selected calls.
MMU paths, sparse memory, tagged/fractional values, unsupported operations,
non-local control, and faults exit to the portable executor with precise
state. Cache keys include ISA/features, ASID, PC, instruction-memory and
mapping generations, and relevant MMU state. Acceptance is based on
wall-clock speed, correctness, parity, and timing stability, not decode counts.

### 6.7 Clean v2 cutover

The transition deliberately distinguishes four kinds of compatibility:

- **Binary compatibility:** dropped in the production v2 runtime.
- **Live storage compatibility:** dropped; legacy disks are not mounted.
- **Source compatibility:** retained by recompiling old programs for v2.
- **Historical reproducibility:** retained through the v1 tag, fixtures, and
  offline tools.

The v2 loader rejects v1 executables and old boot formats with a clear error.
The standalone migrator reads legacy artifacts, validates them, and atomically
creates v2/v3 replacements. Bundled programs are rebuilt from source. This
removes legacy opcode decoders, tagged-register compatibility branches,
raw-versus-numeric PTE fallbacks, v1 syscall dispatch, and mixed image-header
logic from the production hot path.

## 7. Why v2 replaced v1

V2 was not a rewrite for aesthetic reasons. It was a response to measurable
coupling:

1. **A fixed physical contract was needed.** T40/T50 rules allow the compiler,
   VM, kernel, JIT, linker, and hardware description to agree.
2. **The ISA needed room to evolve.** Extensions and feature discovery prevent
   every experiment from becoming a permanent direct opcode.
3. **The MMU needed predictable performance.** Larger pages, real TLB
   hierarchy, ASIDs, and precise invalidation turn translation into a measured
   subsystem.
4. **The storage model needed crash semantics, not just logging.** Redo
   after-images, LSN ordering, no-steal rules, and inode-scoped fsync define
   what a crash may expose.
5. **The compiler needed global correctness.** SSA, dominance, alias-aware
   promotion, graph coloring, and spill rewriting eliminate the AST/target
   disagreement that made optimization fragile.
6. **The scheduler needed bounded hot paths.** Queue ownership and
   architectural WAIT make latency measurable instead of dependent on table
   scans or timer busy loops.
7. **The compatibility matrix was too large.** Clean v2 reduces the number of
   semantic modes in every subsystem while preserving migration and history.

The price is real: v2 requires rebuilding software and converting persistent
images, and it rejects old binaries in production. That price is paid once at
the architecture boundary instead of repeatedly in every future subsystem.

## 8. Test cases for software engineers

The following are study cases, not merely a list of regression commands. Each
case asks what invariant the engineer is proving and which v1 assumption it
replaces.

### TC-01: Instruction and extension contract

Encode and decode every v2 direct opcode. Verify round trips, illegal
selectors, reserved direct opcodes, malformed trits, and feature rejection.
Attempt to assemble a v1-only instruction and confirm an explicit error.

**Lesson:** an opcode map is a compatibility contract, not just an enum.

### TC-02: Width normalization and T50 pairs

Write T1, T5, T10, and T20 results into registers and memory. Confirm unused
upper trits are cleared, invalid dual-rail values remain invalid, and T50
values use adjacent low-then-high T40 words. Exercise spills, calls, returns,
and wide destinations.

**Lesson:** width views must not become hidden runtime tags.

### TC-03: ABI, executable headers, and feature discovery

Load valid v2 and v3 headers; reject v1 headers, missing required features,
bad checksums, invalid scalar width, and impossible page geometry. Verify
callee/caller preservation, T50 returns, syscall argument registers, and the
reserved syscall IDs.

**Lesson:** loaders should reject ambiguity before executing user code.

### TC-04: PTE, TLB, ASID, and superpage behavior

Run sequential and random virtual-memory workloads, measure L1/L2 hits and
walks, switch ASIDs, reuse an ASID, change permissions, map a superpage, and
issue scoped TLBINV. Confirm instruction/data permission priority and
accessed/dirty updates.

**Lesson:** translation correctness and translation performance need separate
observable counters.

### TC-05: Precise vector memory faults

Use a vector load/store/gather/scatter whose active lanes cross a valid/invalid
boundary. Confirm a load leaves its destination unchanged, a store writes no
lane until preflight succeeds, and the trap records the first failing lane and
complete fault mask.

**Lesson:** vectorization must preserve scalar fault atomicity.

### TC-06: SSA as the sole code-generation source

Compile loops with phis, critical edges, calls with live values, alias-blocked
locals, aggregates, atomics, and address-taken variables. Inspect the metadata
that AST replay is disabled. Compare optimized and unoptimized execution,
dynamic counts, spill slots, call-clobber handling, and T50 constraints.

**Lesson:** an optimizer is trustworthy only when one verified IR produces the
object code.

### TC-07: Global optimization and allocation

Exercise constant propagation, value numbering, copy coalescing, dead-code
elimination, loop-invariant code motion, induction simplification, branch
folding, costed selection, graph coloring, coalescing, and repeated spill
rewrite. Require correctness equality and a bounded instruction-count target.

**Lesson:** local wins are not a substitute for a globally colorable program.

### TC-08: Portable/native execution parity

Run randomized programs through the interpreter, decoded executor, and native
x86-64 JIT. Compare result state, PC, trap cause, instruction count,
destination nonmutation, and memory effects. Verify W^X pages, IMEM
invalidation, MMU side exits, sparse fallback, and unsupported-operation
fallback.

**Lesson:** a JIT is an alternate implementation of the VM contract, not a
second semantics.

### TC-09: Scheduler, FIFO waits, and architectural WAIT

Enqueue and dequeue 100 runnable slots while checking that ordinary scheduling
performs no process-table scan. Wake each wait source exactly once, verify FIFO
order and fairness, and empty the queue. Confirm the VM reports WAITING and
resumes only on a timer, device, interprocessor, or interrupt event.

**Lesson:** idle behavior and queue ownership are part of the ISA/OS contract.

### TC-10: WAL crash and inode-specific fsync

Inject failures before and after data append, commit, page write, barrier,
checkpoint, and superblock switch. Recover and require either the complete old
transaction or the complete new transaction. Write two inodes, fsync one,
and verify that the other remains dirty. Test torn records and LSN chains.

**Lesson:** “has a WAL” is not a durability specification; ordering is.

### TC-11: VFS lifetime, reclamation, and rename

Unlink a file while it is open, read it through the open descriptor, close the
last descriptor, and verify that inode/dirent/extent holes can be reused
without stale buffer data. Test inode generations, truncate, malformed paths,
WAL-full nonmutation, atomic rename, collision behavior, and recovery after a
torn rename record.

**Lesson:** namespace state, object lifetime, cache identity, and persistence
must agree.

### TC-12: Clean cutover and offline migration

Feed the migrator v1 boot and disk fixtures, validate the resulting v2/v3
images, and inspect checksums and architecture metadata. Then attempt to boot
or mount the legacy artifacts in the v2 runtime and require a clear rejection.
Rebuild a v1 source program and compare its intended behavior under v2.

**Lesson:** migration is safer when conversion is explicit and legacy
execution is not hidden in the production kernel.

### TC-13: Workload and release gates

Run the bounded Doom-style graphics/input/timer/I/O workload and the
BitNet-style vector/memory-pressure workload. Record source identity, build
profile, correctness hashes, instruction mix, TLB counters, scheduler
behavior, WAL counters, medians, dispersion, and wall time. Inspect the staged
release image and export diagnostics.

**Lesson:** architectural claims become credible only when they are tied to
repeatable workloads and release artifacts.

## 9. Engineering conclusions

The v1-to-v2 transition suggests a practical order for future architecture
work:

1. Freeze physical widths, invalid encodings, register ownership, and image
   metadata before optimizing.
2. Keep optionality in extension groups and feature words, not in ambiguous
   direct semantics.
3. Generate compiler, assembler, kernel, SDK, tests, and documentation from
   shared contracts where possible.
4. Separate durable control-plane state from transient state that can be
   reconstructed.
5. Require precise fallback paths before introducing native acceleration.
6. Treat workload measurements, crash injection, and release-image inspection
   as architecture tests rather than late validation.
7. Preserve old implementations as tags, fixtures, and offline converters;
   do not make the production runtime carry every historical semantic mode.

## Conclusion

V1 was the system that made the idea concrete. It chose broad representation
freedom and fast vertical progress so that the project could learn where the
hard boundaries were. V2 is the system that turns those lessons into durable
contracts: fixed T40 geometry, explicit T50 pairs, a compact feature-aware ISA,
canonical MMU state, measured scheduling, redo durability, SSA-first
compilation, portable/native execution parity, and a clean compatibility
boundary.

The correct historical interpretation is therefore not “v1 was bad and v2 was
good.” It is “v1 optimized for discovery; v2 optimized for composition.” The
test cases above are the bridge between those phases: each one identifies a
place where an initially convenient decision became a long-term systems
contract, and shows how the v2 architecture makes that contract explicit.
