# User-space service host

Sources: [service host](../../apps/service_host.trit),
[service runtime](../../apps/service_runtime.trit), [SDK](../../apps/os_sdk.trit),
[kernel](../../kernel.trit), [builder](../../build_tos_image.cpp), and
[current conformance](../../tests/current_only_conformance.cpp).

One `/bin/service_host` runs as PID107 in slot7 with private user mappings and the
standard9-word stack alignment (2052-word service stack). `/bin/init` attempts
its launch once before exec of `/bin/desktop`. A failed launch is printed and
still reaches desktop; a later host startup failure does not block the desktop.
`sessiond`, `logd`, `window_server` and other stub-backed registrations remain
placeholders. The original59 app registrations plus this host are retained.

The kernel owns input queues, scheduler/interrupt routing, process/window
ownership and wakeups. User-space service graphs observe and process bounded
records; they do not replace those kernel mechanisms. Shared-memory channels
and a demonstrated GUI latency improvement remain deferred.

## Records, scheduling and bounds

Service60 (`os_service_metrics(out_addr, out_words)`) writes a fixed7-word,
version1 namespace record: version, caller PID, delivered kernel IRQ tick,
namespace process count, runnable/running count, window queued events and window
event capacity. It validates the complete advertised writable span before copying,
requires at least7 integral words and a valid integral user pointer, and returns
raw status1/payload7/detail0 on success. On failure raw status is -1 with negative
error in payload and detail. This is bounded observability, not arbitrary kernel
memory exposure. SDK negative-result selection leaves those raw fields unchanged.

The service samples at absolute CYCLE deadlines250,000,000 apart: one CYCLE is
one virtual nanosecond, so this is250 virtual milliseconds. Delivered IRQ ticks
are a separate count and host wall time is measured separately. When downstream
capacity or pool credits are absent, the source does not query metrics or advance
its deadline. Callbacks never allocate or sleep; the outer loop sleeps only the
remaining deadline rounded upward to milliseconds and otherwise yields.

Construction reserves private state, a bounded32-record pool (minimum5), channels,
startup and processing graphs, and a bounded8-job table. Dispatch handles at most32
callback attempts per dispatch. A sample expansion reserves three credits before side effects;
rollback retains capacity for progress and does not strand a full upstream head.
Pressure is high at >=80%; the monitor alerts after three consecutive high samples
and recovers after three consecutive low samples. Aggregation emits complete
eight-sample windows plus partial windows at EOS. Alerts/recoveries and
control/EOS/STOP are immutable records with explicit ownership.
Both graphs validate before effects and all five startup stages complete before
READY. Logger/metrics failure closes initialized resources in reverse order.

Job admission permits eight outstanding jobs; a ninth has no effects. Bounded
steps allow unrelated jobs to progress during a stalled logger. Process reports
use real five-word `ps` snapshots and rechecked eight-word `getproc` records,
including namespace checks; report rows are16 words, not arbitrary pointer data.
Cancellation drains accepted/partial/EOS/prefix work and jobs, closes descriptors,
and resets ownership before restart. Destroying a running service retains exact
pending work rather than pretending it was durable.

## Persistence and limits

The logger uses real VFS writes, handles fd0, positive short prefixes and zero
progress, and releases records only after successful targeted fsync. Two log files
hold at most64 records each; retirement is explicit and only follows successful
rotation. Fatal write/fsync errors retain an exact uncommitted count. Generation
markers must complete before READY and restart rejects old-generation tokens.
Writer PIDs are1..255. Generations are1..1023; an existing1023 generation cannot advance. Tokens use a PID stride of2^30 and a generation stride of2^20,
and record sequence encoding is bounded to the explicit positive
integer wire domain capped by `SVC_WIRE_MAX` (actual conformance value
2,779,530,283,277,763). Sequence values start at1 and are capped at1,048,575; overflow drains
instead of reusing tokens. Foreign, fractional, stale and reused tokens are rejected.
Forced process stop does not promise replay of volatile pool/jobs/uncommitted
records or loss-free recovery of pending work.

WAL maintenance occurs at bounded transaction boundaries. WALv2 keeps729 records,
two alternating superblocks and a publication reserve: WAL write/barrier precedes
home writes/barrier, checkpoint publication and durable tail advancement.
Unresolved owners, snapshot pins, unsafe/corrupt targets and I/O failures block
reclamation. Finite collision ownership and exact negative errno preservation
are tested; targeted fsync still isolates the requested inode. Boot attributes
are a finite volatile relation reconstructed during boot. The narrowly checked
PID1 boot-record retirement exception does not make arbitrary attributes durable
configuration. Broader etcd-style reconciliation remains direction.

## Evidence and remaining gates

The original33 on-demand scenarios, full WAL workloads and original GUI assertions
are preserved. Automatic three-mode proof has a separate path and completion.
Focused2 service-error or focused4 WAL admission reports never count as full proof.
Completion wrappers fingerprint required real histories, reconciliations, state,
pool/job and GUI evidence after writers close; empty no-work logs are still hashed.
See [build and test](../09_Host_Runtime/build_and_test.md) for current-source
completion states and independent full gates. Owner accepted the rollout on2026-10-11(local time) with remaining validation waived
to save usage. Ten final-source default workflows and independent CLI94, desktop and
automatic-three checks passed. The full suite was intentionally stopped at982,001,000
steps in history_cap1, with zero guest failures and3,421 words of arena use.
Remaining full/CI/SDL/fresh-review validation is waived, without PASS claims.
Acceptance and preserved results: `build_current_cleanup/service_workflow_artifact/final_gate/parent-full-test-final94-20261011/user_acceptance_result.json`.

The static kernel buffer/capability address overlap tracked as `trit-cx1` remains;
the active-frame guard does not relocate storage or claim a general layout repair.
