# Native compositor foundation contract

Stage 1 validates the existing native kernel, SDK and VFS before a separate
compositor session is introduced. Normal desktop startup and the in-process
`service_host` graph remain the current implementation. ISA v2, executable and
function ABI v3, syscall ABI v2, `.tboot` v3 and `.tdisk` v2 are preserved.

## Current coordinate and surface conventions

Window geometry uses display coordinates: `(x,y)` is the window origin; width
and height are positive dimensions. Drawing helpers use client-local coordinates
and clip rectangles to `[0,width) × [0,height)`. Existing mouse event records
contain display coordinates; clients translate by their window origin. Keyboard
records carry a key value rather than a coordinate. Window composition clips
against the display. RGB surfaces use three consecutive integral channel words
per pixel, in red, green, blue order, each in `[0,255]`; row stride is `width*3`.
The planned span convention is `(y,x_begin,x_end_exclusive)` in client-local
coordinates. Stage 1 does not add a span API or line rasterizer.

`os_window_get_buffer(window_id)` maps the owner's fixed ten-page window storage
into that process. Its pointer is usable only during that window/process
lifetime. Current successful resize validates dimensions, changes geometry and
clears the same fixed storage; it does not allocate a new surface or provide
a pointer generation. Failed resize preserves geometry and storage. Destroy
and process cleanup clear storage; process cleanup also removes process page
mappings. Existing signatures remain unchanged.

## Current event and lifetime conventions

The bounded FIFO stores four words: `[kind,x_or_key,y,modifiers]`, with key=1,
mouse=2, close=3 and timeout=4. Its capacity is eight records; exhaustion reports
an error instead of silently overwriting a queued event. Mouse press focuses
the hit window. Capture starts at press and routes movement through release
to the captured window. Keyboard input follows focus, with the current
top-visible-window fallback when no valid focus exists. The native driver keeps
window sampling independent from global input consumers.

Close requests queue a close event and signal the owning process. Event reads
and window waits are owner-only: a peer process cannot wait on another process's
window. A pending window wait belongs to its owner's process lifetime and must
be removed on termination, including its deadline and queue links. Process
cleanup must also release descriptors, IPC ownership, windows, focus, capture
and page mappings. Reaping must be idempotent and a reused slot must obtain
the newly registered application capability mask.

Blocking ABI results use status/payload/detail. Ordinary IPC delivery returns
status1/payload1/detail0 and writes the received word to the supplied pointer.
Timeout returns status0/payload0/detail-10, which the existing SDK interprets
as zero progress. Cancellation must return status-1/payload0/detail-12, which
the SDK interprets as -12. `waitpid` returns the child's exit status; forced
kill uses the existing signal value2 as exit status. Scheduler enqueue
generations are queue-membership protection, not stable process identity.

## Pending Stage 2 and later contracts

Authenticated requests, session identity, request sequence, transport queues,
display ownership, immutable committed snapshots and surface generations are
pending Stage 2. The planned successful-resize contract invalidates the old
pointer; failed resize preserves it. That is a future protected-session contract,
not a claim about current fixed window storage. Compositor-owned stacking,
focus, capture, input routing, presentation and guest composition are pending
Stages 2–6. No compositor is activated in normal desktop sessions at Stage 1.

Assets, persistence, fonts, widgets, desktop migration and the broader
`trit-b8b.2` scope remain open. Stage 1 evidence alone does not establish them.

## Repeatable Stage 1 evidence

Build the existing `trit_current_conformance` target and run
`build_current_cleanup/trit_current_conformance.exe --compositor-prerequisites-only`.
Each invocation creates a new `compositor_workflow_artifact/stage1/run_N`
directory, preserving earlier failures. Evidence contains embedded compiled
guest sources, link output, current boot/disk images, saved VM state, lifecycle
transitions, raw ABI returns, resource/reuse observations and execution-derived
assertions. A partial or failed report does not establish prerequisite acceptance.

## Frozen Stage 2 transport subgate

The transport subgate uses syscall IDs 61–66: register, join, submit, receive,
reply and leave. Registration is limited to a trusted immutable compositor
executable role, distinct from the ordinary capability mask 63. Namespace 0
production policy recognizes the exact paths `/bin/compositor` and
`/bin/comp_supervisor`; private compiled fixtures use a trusted kernel pin helper.
Roles bind a registered app ID, inode allocation generation and exact bounded
path. Fork does not inherit the role or graphics join. Admitted exec replaces
graphics identity even if the load fails; an invalid user pathname span does
not admit exec.

The frozen records are eight-word registration/join info
`[1,session,PID,process_generation,2,16,10,1]`, an eight-word submitted request
`[operation,args0..6]`, a sixteen-word received record
`[1,kind,session,PID,process_generation,sequence,handle,record_generation,operation,args0..6]`,
and a ten-word reply
`[1,session,PID,process_generation,sequence,handle,record_generation,status,payload,detail]`.
Kinds 1, 2 and 3 mean request, client closed and request canceled. The kernel
retains client reply pointers privately. Numeric arguments retain their exact
signed or fractional T40 values; tokens, operations, counts and tuple headers
are integral. Registration state 1 means active transport; it does not claim
the later compositor READY state.

Limits are two queued requests, eight retained request records and eight joined
lifetimes with distinct reserved closure rows. One synchronous request per
caller blocks until authenticated reply or cancellation. Replies match every
issued identity and token once. Pool/counter exhaustion is -3, bad span -5,
invalid/stale/unauthorized -8, queue full/already pending -11 and cancellation
or peer death -12. A revoked retained reply span completes once with -5 and
cannot receive a partial reply. Cancellation and closure controls preserve the
original lifetime and tokens; compositor death fails the session without
automatic restart.

Metadata reserves twenty pages at PPN 20200–20219, word range
`[14725800,14740380)`, above process backing and separate from V3 contexts
`[700002,727902)`. A global monotonic bounded process counter supplies fresh
lifetimes on create and admitted exec and survives slot reap. Spawn reserves
two remaining values before replacing a terminal child; fork needs one.

Run `build_current_cleanup/trit_current_conformance.exe --compositor-transport-only`.
The additive `compositor_transport` completion group preserves append-only
`compositor_workflow_artifact/stage2_transport/run_N` evidence. A complete PASS
requires all ten compiled behavior variants and their actual dynamic lifetime
assertions. RED or fixture-INCOMPLETE evidence establishes no implementation
acceptance. Surface/display protection, window routing and Stages 3–6 remain
pending; normal desktop activation is unchanged by this selector.

The native V3 prerequisite uses metadata offsets8–15 as a private scalar trap
mailbox and offsets164–563 as four100-word generation-bound readiness and
pending-clone arrays. These ranges remain reserved from transport records.
Trap entry saves the interrupted live state with VCTXSTORE and marks its
current issued generation ready. A fork child remains internally STOPPED
until the return boundary loads the validated saved parent and stores the
exact279-word V3 state into the child, then marks that lifetime ready and
enqueues it. All pending clones finish before any ERET. Source invalidation
retains a dependent marker so validation fails closed instead of dropping an
already-successful clone obligation.

An unready selected lifetime receives zero eight vector registers and zero
accumulator, with canonical fault records generated by the existing vector
operations and VCTXSTORE; selected state is then loaded unconditionally.
Only readiness0 or the exact current issued generation is valid. Boot's
first task captures its actual initial live vector state at its first trap;
other never-run tasks receive zero state. Existing PID1/slot0 kernel idle
selection remains supported. Invalid lifetime, owned pointer, clone token,
or readiness metadata halts in Kernel mode before user return.

Scalar registers and scalar STORE use canonical T40 physical values. A
source declaration of T5 does not make scalar STORE preserve a T5 memory
tag; the earlier fork fixture expectation was invalid. Exact V3 tags remain
required by VCTXLOAD. Private typed-load equivalence and wide-word semantics
remain separate native E2E prerequisites; this context slice does not accept
full fork or compositor transport.

Native fork admits a hardware DMEM mapping only when its VPN/PPN pair is
canonical private data (`VPN < PROCESS_DATA_PAGES_MAX`, `PPN = parent_ppn +
VPN`) or canonical scratch (`USER_SCRATCH_VPN_BASE <= VPN < base + pages`,
`PPN = parent_ppn + PROCESS_SCRATCH_PPN_OFFSET + VPN - base`). Either relation
is allowed where heap growth replaces scratch with ordinary private data.
The child uses its corresponding backing offset and retains hardware flags;
other aliases are omitted even when their physical page lies inside the
parent backing range. Parent mappings and shared immutable IMEM text remain
unchanged. The local window lifetime probe records an actual child User LOAD
and the parent's real waitpid return; this mapping prerequisite still does
not accept private wide-word semantics, full fork, or compositor transport.

The focused `--compositor-fork-fatal-user-only` native prerequisite treats
numeric overflow as a defined special value, distinct from invalid positional
encodings. A labeled test-only assembly extension of compiled guests executes
finite accumulator arithmetic, native syscall serialization, and a separate
current-ISA VMAC instruction with an out-of-range vector operand. Generated
assembly and final assembly plus the installed instruction word are retained.
Only the independent illegal operation is expected to cause User fault1;
its parent wait, reap, private guards and scheduled survivor are observed.
Production numeric semantics and strict V3 validation remain unchanged.
The aggregate fork prerequisite also requires this phase; full fork still
needs private typed/wide semantics and malformed lifetime validation.

The focused `--compositor-fork-memory-only` prerequisite uses real User numeric
vector stores and typed loads across fork for T1/T5/T10/T20/T40/T50. Nine
27-lane guarded rows include two finite T50 fractions with observed nonzero
high bits and a row spanning private pages. A two-way IPC protocol checks
child mutations, parent preservation, parent mutations and child preservation,
then orderly exit/wait/reap with a compiled survivor. Physical narrow storage
tags may normalize when actual typed-load results remain identical. Scalar
views do not prove full T50 preservation. L50 remains separately unvalidated.
The fifth aggregate prerequisite phase retains full-fork/transport false while
private-memory and malformed lifetime acceptance remain incomplete.

Native fork retains the existing scalar eager copy for narrower numeric typed
loads. Before the stopped child becomes ready, the raw trap shim validates each
canonical private hardware page and corrects actual numeric T50 words with
existing zero-index gather/scatter instructions. A following-register sentinel
and actual TSCAN distinguish full wide words from canonical scalar writes.
The generation-bound mailbox and exact source/child PTE relation are checked
before physical addresses are published; external aliases remain omitted.
Temporary vectors are restored from the selected owned V3 context before ERET.
This correction does not establish L50 coverage or malformed lifetime acceptance.

The focused `--compositor-fork-malformed-v3-only` fixture uses a compiled
parent/supervisor IPC rendezvous and native numeric work before one explicitly
logged privileged metadata-cell injection. Fifteen variants target validation
before clone, private-page admission, completion after genuine copy, or selected
readiness after entry save. Actual helper rejection, fault branch and Kernel HALT
must precede any later User return or readiness publication; owned contexts and
guards are compared through halt apart from the injected cell. A no-injection
control proves real fork, typed isolation, child exit and pending parent wait.
This sixth aggregate phase proves validation at the tested helper boundaries;
it does not establish tamper resistance after validation, L50 or transport authority.


### Stage 4 drawing gate (not yet accepted)

The drawing-only selector is a separate coupled workflow. Native correctness
requires compiled User/kernel execution through `vm::step`; missing
`apps/span_draw.trit` is an INCOMPLETE compile prerequisite. No host expected
pixel/result injection is permitted. Directed inclusive Bresenham clips emitted
pixels, using the same old doubled error for both inclusive tie comparisons.
Copy clipping couples source and destination offsets; same-base/equal-dimension
self-copy has source-snapshot semantics. Other overlapping virtual intervals
return -8 before drawing writes. Mapping and physical ownership remain the
caller and protected Stage 2 contract; this gate confers no library authority.
Special comparisons may fault the offending User child; there is no universal
ordinary -8 classifier promise. Benchmarks select only future measured winning
ranges, under identical warmed preallocated geometry and AB/BA six pairs.
The drawing gate does not accept protected surface, snapshot, display, or full
Stage 4 integration. The paused malformed-v3 scenario is unchanged.

Valid test cases declare destination width and height independently of guest
argument text. The directed/snapshot host oracle clips to those dimensions and
uses the declared width as its flat RGB pixel stride. A full80x60 backing pattern
is still compared, including every untouched word and guard. The nine1x1 cases
include original in-bounds points plus six outside/crossing span and directed
reference/hybrid lines, so ignoring dimensions produces a visible mismatch.
Guest query/grow/query integrality predicates use narrow unsafe blocks because
the current compiler classifies `tmod` as an unsafe intrinsic. All predicates
still execute before the first heap dereference.


The reusable APIs in `apps/span_draw.trit` are:

- `draw_rgb_span(buf,w,h,y,xb,xe,r,g,b)`, with an exclusive span end.
- `draw_rgb_copy(dst,dw,dh,src,sw,sh,dx,dy,sx,sy,cw,ch)`.
- `draw_rgb_line_reference(buf,w,h,x0,y0,x1,y1,r,g,b)`.
- `draw_rgb_line_hybrid(buf,w,h,x0,y0,x1,y1,r,g,b)`.

All inputs are T40. Dimensions must be integral1..80 and1..60, RGB channels
integral0..255, coordinates integral[-4096,4096], and copy extents0..8192.
Pointers must be positive integral with exclusive RGB extent at most512*729.
Bounds and integrality are checked before coordinate differences, products or
stores. Unsupported finite/fractional inputs and reversed spans return-8;
valid empty or completely clipped operations return0; other results count pixels.
Special comparisons can fault only the offending User process before drawing
stores. This does not promise a universal nonthrowing special classifier.

Copies clip one shared offset range against both declared surfaces. Disjoint
virtual intervals and exact same-base/equal-dimension self-copy are supported;
other virtual overlap is rejected before stores. Self-copy chooses safe row and
column directions without allocation, and loads each source RGB triple before
storing its destination. Valid mapping and integral source RGB remain caller
invariants; physical aliases and protected ownership are Stage2 responsibilities.

Reference lines globally validate then store directed inclusive Bresenham points
with the same old doubled error for both tie decisions. Hybrid is an explicit
candidate: horizontal lines use one internally prevalidated clipped run;
horizontal-major lines group exact directed row runs with an integral quotient
and remainder; diagonal/steep lines fall back to reference. Internal clipping
accepts the run-exclusive4097 boundary before any pointer arithmetic. Neither API
allocates, uses global scratch, invokes syscalls, or changes the VM line command.
The candidate has no automatic selection threshold. Native measurements and
root acceptance are required before selecting any measured range. Native drawing
validation and protected integration remain pending until their actual gates pass.


### Focused native drawing lifecycle evidence

`--compositor-drawing-lifecycle-only` runs the same compiled drawing guest and
per-step observer with four existing nonbenchmark cases, in this order:
`span_left`, `overlap_diagonal`, `line_0_forward_reference`, and
`line_0_forward_hybrid`. The full `--compositor-drawing-only` selector retains
its ordered189 cases, warmed benchmarks, assertions, and execution budgets.
Both selectors retain the99-row process observer,750000000-step budget, and
unchanged2000000000-cycle cap.

The focused run checks both actors' owned query/grow/query allocations, actual
CALL/RET regions, independent pixels/counts and guards, all18 live caller values
(checksum3789), both valid-special children created through1200 real multiply3
operations, before-write fault or-8 rejection, one canonical wait/EPC+1 and reap
per child, survivor IPC/progress/exact fraction/guards, and both actors' exits.
It creates four PNGs and an intentionally empty `benchmark_raw.jsonl`;
`benchmark_settings.json` records `executed:false` and `warm:false`.

Evidence is append-only under
`compositor_workflow_artifact/stage4/lifecycle/run_N`, with a separate
`lifecycle/report.json` summary and coupled `compositor_drawing_lifecycle`
completion in focused scope. Acceptance requires the actual closed native PASS,
source SHA256 freeze, complete inventory seal, and Python completion inspector.
The report uses `native_drawing_lifecycle_and_owned_artifacts`, with
`raster_matrix_complete:false`, `benchmark_executed:false`,
`Stage4_acceptance:false`, and `protected_resource_integration:false`.
Only API pressure, special span/line children, survivor progress, and caller
liveness criteria are exercised. This evidence does not settle the full raster
matrix, benchmarks, protected integration, or Stage4 acceptance. Interrupted
full drawing artifacts and their pending completion remain preserved.


Both drawing selectors complete only after each actor's actual User zeroexit
syscall and the final drawing/liveness marker. Painter202 is reaped: its nine
process-row words, capabilities, exit status, cleaned flag, waits, and heap
metadata are cleared, with parent-1. Survivor201 remains a cleaned ZOMBIE7 for
live bootstrap parentPID1, with exit status0, cleaned flag1, cleared heap/waits,
and its original namespace/priority/quota/context/vector-context/capabilities.
Those immutable fields and the real generation are captured at its owned
query/grow/query completion; the mutable process-row version is not fixed.

The earlier both-FREE claim was a fixture criterion mistake corrected by actual
native evidence. Bootstrap app_spawn uses parentPID1; fixture setup overrides
painter202's parent to201 and preserves survivor201's parent. Survivor exit
reparents painter202 to-1, so painter202 later reaps while survivor201 remains
available for its real parent to wait. Completion waits until every required
cleanup field matches, then records one actual_actor_cleanup_complete event
with both tuples, PROC_CLEANED, live parent identity, captured zeroexit
generations, and survivor_retained_for_parent_wait:true. No host reaping or
fixture-parent change is involved.

After the final drawing marker, checked and flushed special.jsonl diagnostics
record both actor and IPC/wait-queue states immediately and every1000000 native
steps until cleanup, including idle WAIT records. Typed Kernel words accompany
integer summaries; invalid or unavailable summaries remain null. Final IPC
entries, actual matched returns, and zeroexits are flushed without changing
completion criteria.


### Generic Kernel memory-word classification prerequisite

CSR 53 `dmem_class` is a Kernel-read-only memory descriptor, with writes disabled.
It inspects the physical DMEM word selected by the existing `scratch` CSR without
loading or converting that word into a scalar register. Classes are 0 unreadable
or out of range, 1 non-T40 (including narrow, wide and logical modes), 2 invalid
raw T40 (including nonzero high bits), 3 defined T40 special, 4 finite fractional
T40, and 5 finite integral T40 (including huge integers and noncanonical zeros).
Ownership and User-pointer translation are separate Kernel responsibilities.
The test-only helper saves scratch, selects the validated physical address, reads
CSR53 and restores scratch before returning. Existing checkpoint serialization
already retains scratch; this primitive introduces no additional state or opcode.

`--kernel-word-classification-only` is an independent focused E2E selector. Its
`kernel_word_classification` completion and append-only `word_classification/run_N`
artifacts record actual compiled User/Kernel instructions, independent class
literals, typed source and guard preservation, real getpid tuples, three child CSR
denials, wait/reap, caller liveness and native checkpoint replay. T5 and T50
catalog entries use actual User VBCAST/VSTORE into disjoint 27-word extents.
Other unusual raw and logical inputs are explicitly trusted typed INPUT seeding
at compiled per-query markers; no legal L50 guest instruction support is claimed.
Malformed inputs are deferred until after the safe first-query checkpoint reload,
and the guest clears each queried source only after its unchanged-word proof.

This pre-code phase activates only the descriptor and fixtures. The unchanged VM
has no CSR53 read semantics, so actual Kernel execution must report RED/INCOMPLETE,
with failed coupled completion, until separately reviewed implementation follows.
Unreached scenarios remain unobserved. Full transport acceptance and Stage 2
acceptance remain false; syscalls 61�66 are unimplemented and untested here.
