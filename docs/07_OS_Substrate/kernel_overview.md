# Kernel Architecture Overview

Source of truth: `kernel.trit`, `kernel/`, `ternary_os.h`, `SYSCALL_MANIFEST.json`

---

## Structure

The kernel is a **monolithic kernel** written entirely in TCL (`.trit`).
Source: `kernel.trit` (~5700 lines) + subsystem files in `kernel/`.

```
kernel.trit              Main kernel: syscall dispatcher, scheduler, init
kernel/process.trit      Process Control Blocks, fork/exec, scheduling
kernel/vfs.trit          Virtual Filesystem: inode, open, read, write
kernel/bio.trit          Block I/O: WAL, sector read/write
kernel/hal.trit          Hardware Abstraction Layer: timer, interrupts
kernel/net.trit          Networking: socket, bind, connect, send, recv
```

The active storage contract is documented in
[[redo_wal_v2|Redo WAL v2]]. The implementation remains consolidated in
`kernel.trit` until the source split preserves generated architecture
constants and focused test coverage.

---

## Boot Sequence

```
1. .tboot image loaded by host runtime (build_tos_image.cpp / ternary_host_runtime.h)
2. VM initialized, PC = boot_entry (= address of init in kernel.trit)
3. Kernel init:
   a. Unmarshal rootfs from .tboot into in-memory VFS
   b. Set up process table
   c. Spawn PID 1 (init.trit / /sbin/init)
4. init.trit:
   a. Mount filesystems
   b. Start service daemons (sessiond, logd, window_server)
   c. Spawn desktop.trit (PID 2+)
5. Kernel event loop: TVEC handler dispatches syscalls + timer interrupts
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
| `sys_spawn_static` (10) | Spawn a statically-linked process by name |
| `sys_app_spawn` (57) | Spawn a registered app by name |

### Scheduler
- Round-robin with priority queues.
- Timer interrupt (`OS_CAUSE_TIMER_IRQ`) triggers preemption.
- `sys_yield` (4), `sys_sleep` (45), `sys_sleep_ms` (56), `sys_sleep_until_tick` (5) for voluntary yield.
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
├── sbin/
│   └── init          ← PID 1
├── bin/              ← POSIX utilities (50+ binaries)
│   ├── shell, ls, cat, cp, mv, rm, mkdir, ...
│   └── doctor, test, sysinfo, ...  (some missing; see KNOWN_GAPS.md)
├── apps/             ← GUI apps
│   ├── desktop, terminal, calculator, paint, ...
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
| `sys_uptime` | 8  | Get system uptime in ticks |
| `sys_ps` | 46 | List running processes |
| `sys_getproc` | 51 | Get process info by PID |
| `sys_kill` | 48 | Send signal to process |

---

## Syscall Error Codes

| Return (r13) | Meaning |
|-------------|---------|
| 0 or positive | Success (service-specific) |
| 2            | Blocked (will be resumed) |
| negative     | Error |

See `SYSCALL_MANIFEST.json` for per-syscall error codes.
