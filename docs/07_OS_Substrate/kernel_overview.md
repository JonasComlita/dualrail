# Kernel Architecture Overview

Source of truth: `kernel.trit`, `ternary_os.h`, `ternary_host_runtime.h`,
`SYSCALL_MANIFEST.json`

---

## Structure

The kernel is a **monolithic kernel** written entirely in TCL (`.trit`).
The active source is the root `kernel.trit`, compiled directly into the current
image path. The former modular `kernel/` fragments are retired and are not
compiled or loaded by the current platform.

```
kernel.trit              Current kernel: syscall dispatcher, scheduler, VFS, init
ternary_os.h             Current OS-facing runtime and syscall interfaces
ternary_host_runtime.h   Host image, VM, and runtime boundary
```

The active storage contract is documented in
[[redo_wal_v2|Redo WAL v2]]. The implementation is intentionally consolidated
in `kernel.trit`; a future source split is out of the current platform scope
and would require preserving the root architecture contract and conformance
coverage before it could be considered.

---

## Boot Sequence

```
1. Host loads current .tboot v3/.tdisk v2 and enters the builder boot assembly.
2. Boot assembly calls kernel_init, reconstructs VFS/process/boot state, then execs /bin/init.
3. Init attempts os_spawn_app(/bin/service_host, slot7, PID107) once.
4. Spawn failure is printed; init still execs /bin/desktop. A post-launch host
   startup failure does not block desktop. Stub daemons are not started here.
5. Kernel trap/scheduler/input/window queues and ownership remain authoritative.
```

---

## Privilege and Trap Entry

The kernel runs in Kernel privilege mode (T_NEG). User processes run in User mode (T_POS).

On any trap:
1. VM saves PC → `CSR epc`
2. VM saves privilege → `CSR status`
3. VM jumps to `CSR tvec` (kernel trap handler) in Kernel mode
4. Kernel reads `CSR cause` to dispatch:
   - `OS_CAUSE_SYSCALL` → syscall dispatch table
   - `OS_CAUSE_TIMER_IRQ` → scheduler tick
   - Page faults, protection violations → kill process or handle
5. Kernel returns via `ERET` (restores mode + PC)

---

## Process Model

### Process Control Block (PCB)
Each process has:
- PID (ternary integer)
- State: running, ready, blocked, sleeping, zombie
- Register snapshot (saved on context switch)
- IMEM + DMEM bounds (for MMU programming)
- File descriptor table
- Parent PID
- IPC message queue

### Lifecycle

| Syscall | Operation |
|---------|-----------|
| `sys_fork` (20)      | Clone current process (COW semantics) |
| `sys_exec` (21)      | Replace current process image from file |
| `sys_exit` (44)      | Terminate current process |
| `sys_waitpid` (43)   | Wait for child process to exit |
| `sys_spawn_static` (10) | Runtime mapping retained; current kernel has no dispatch branch |
| `sys_app_spawn` (57) | Spawn a registered app by name |

### Scheduler
- Round-robin with priority queues.
- Timer interrupt (`OS_CAUSE_TIMER_IRQ`) triggers preemption.
- `sys_yield`, `sys_sleep` and `sys_sleep_until_tick` (all45), plus `sys_sleep_ms` (56) for voluntary yield.
- `sys_suspend` (49) / `sys_resume` (50) for explicit process control.

---

## Memory Management

- Each process has its own IMEM and DMEM regions.
- The kernel programs MMU CSRs on each context switch.
- `sys_brk` (18) / `sys_sbrk` (19) for heap growth.
- Kernel memory is identity-mapped (no translation); user memory is translated.

---

## Virtual Filesystem (VFS)

### Structure
- Inode-based filesystem.
- Root filesystem (`/`) is loaded from the `.tboot` image's `rootfs_words[]` blob.
- No dynamic mounting of external block devices yet (see `KNOWN_GAPS.md`).

### Key Syscalls

| Syscall | ID | Operation |
|---------|----|-----------|
| `sys_open` | 12 | Open file, return fd |
| `sys_close` | 13 | Close fd |
| `sys_read` | 14 | Read bytes from fd |
| `sys_write` | 15 | Write bytes to fd |
| `sys_stat` | 16 | Get file metadata |
| `sys_readdir` | 17 | List directory entries |
| `sys_mkdir` | 41 | Create directory |
| `sys_unlink` | 42 | Delete file |
| `sys_fsync` | 47 | Flush file to disk |

### Standard Filesystem Layout
```
/
├── bin/              ← Current APP_MANIFEST registrations, including init/desktop/service_host
│   ├── shell, ls, cat, cp, mv, rm, mkdir, ...
│   └── doctor, test, sysinfo, ...  (some missing; see KNOWN_GAPS.md)
│   ├── desktop, terminal, calculator, paint, service_host, ...
└── etc/, tmp/, proc/, dev/
```

---

## IPC

Two-way message passing (synchronous and blocking):

| Syscall | ID | Description |
|---------|----|-------------|
| `sys_ipc_send` | 23 | Send message (word) to PID |
| `sys_ipc_recv` | 24 | Try to receive (non-blocking) |
| `sys_ipc_recv_blocking` | 54 | Block until a message arrives |
| `sys_wait_event` | 55 | Block until any IPC or window event |

---

## Window Manager

The kernel includes an integrated window manager:

| Syscall | ID | Description |
|---------|----|-------------|
| `sys_window_create` | 27 | Create window, return window ID |
| `sys_window_get_buffer` | 28 | Get framebuffer pointer |
| `sys_window_present` | 29 | Flip/present framebuffer |
| `sys_window_move` | 30 | Move window |
| `sys_window_set_z` | 31 | Set window Z-order |
| `sys_window_destroy` | 32 | Destroy window |
| `sys_window_read_event` | 33 | Poll for window event (mouse/keyboard) |
| `sys_window_resize` | 34 | Resize window |
| `sys_window_request_close` | 35 | Request window close |
| `sys_fb_init` | 25 | Initialize framebuffer |
| `sys_fb_flip` | 26 | Flip framebuffer pages |

---

## Networking

Partial implementation (see `KNOWN_GAPS.md`):

| Syscall | ID | Description |
|---------|----|-------------|
| `sys_socket` | 36 | Create socket |
| `sys_bind` | 37 | Bind socket to address |
| `sys_connect` | 38 | Connect socket |
| `sys_send` | 39 | Send data |
| `sys_recv` | 40 | Receive data |

---

## Futex / Blocking Primitives

| Syscall | ID | Description |
|---------|----|-------------|
| `sys_futex_wait` | 52 | Block on a futex (with expected value check) |
| `sys_futex_wake` | 53 | Wake waiters on a futex |

---

## Process Introspection

| Syscall | ID | Description |
|---------|----|-------------|
| `sys_getpid` | 7  | Get current PID |
| `sys_uptime` | 8  | Get monotonic virtual CYCLE nanoseconds |
| `sys_ps` | 46 | List running processes |
| `sys_getproc` | 51 | Get process info by PID |
| `sys_kill` | 48 | Send signal to process |

---

## Syscall Error Codes

| Return (r13) | Meaning |
|-------------|---------|
| 1 | Success; r14 is the service payload, r15 detail |
| 0 | No result/poll/EOF where specified; payload is preserved |
| 2            | Blocked (will be resumed) |
| negative     | Error |

See `SYSCALL_MANIFEST.json` for per-syscall error codes.


### Monotonic virtual timing

The OS clock maps one user-readable CYCLE to one virtual nanosecond (1,000,000
cycles per millisecond). The desktop timer quantum is 1,000,000 cycles: one virtual
ms (1,000 Hz). Delivered kernel IRQ ticks, including metrics field 2, count delivered interrupts
separately from elapsed milliseconds. Timer-wheel expiry catches up from elapsed CYCLE quanta
with at most 243 buckets per scan, including coalesced interrupts.

Positive blocking durations for futex wait (52), IPC receive (54), event wait
(55), and sleep-ms (56) are integral milliseconds through 2,147,483,647; invalid
positive durations return ERR_INVALID before queue effects. Existing ready and
pointer/error checks retain priority. Nonpositive 52/54 waits remain indefinite,
55 polls, and 56 returns immediately. Sleep (45) still waits one timer quantum.
Finite deadlines round current time upward and cannot complete early; scheduling
may complete them later. Service sampling uses absolute CYCLE deadlines and
sleeps only the remaining interval outside callbacks.

This deterministic virtual clock does not guarantee host real-time pacing.
Measure host wall intervals and jitter separately from virtual elapsed time;
report slow-emulation factor rather than interpreting IRQ ticks as wall time.


### Accepted user-space observability and WAL bounds

[Service host](service_host.md) documents syscall60's fixed7-word namespace
record and one bounded private host. The service does not own kernel input,
interrupt/scheduler queues, window ownership or wakeups. Stub-backed daemon
registrations remain placeholders. Shared channels and GUI latency improvement
remain deferred.

WALv2 retains729 records, two alternating superblocks and publication reserve.
Bounded transaction-boundary maintenance keeps WAL/barrier before homes/barrier,
checkpoint publication before durable tail advancement, and targeted fsync.
Unresolved/colliding owners, snapshot pins, unsafe targets and I/O failure block
reclamation; active-frame guards do not relocate the `trit-cx1` static storage
alias. Boot attributes are finite volatile state reconstructed on boot; the
narrow PID1 retirement exception is not durable arbitrary configuration.
