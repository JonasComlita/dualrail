# Syscall Table

Source of truth: `SYSCALL_MANIFEST.json`, `ternary_compiler_ir.h` (runtime namespace)

---

## Convention

```
CSR syscall_id = <id>   ← set BEFORE SYSCALL instruction
r13 = arg0
r14 = arg1
r15 = arg2
r16 = arg3
SYSCALL

→ r13 = status  (0=OK, 2=blocked, negative=error)
→ r14 = payload
→ r15 = detail
```

---

## Complete Syscall Table (57 services)

| ID | Name | Args | Returns | Description |
|----|------|------|---------|-------------|
| 1  | `sys_write_int` | r13=int | — | Print integer to console |
| 2  | `sys_newline` | — | — | Print newline to console |
| 3  | `sys_clear` | — | — | Clear console |
| 4  | `sys_yield` | — | — | Yield CPU to scheduler |
| 5  | `sys_sleep_until_tick` | r13=tick | — | Sleep until absolute tick |
| 6  | *(reserved)* | | | |
| 7  | `sys_getpid` | — | r14=pid | Get current process ID |
| 8  | `sys_uptime` | — | r14=ticks | Get system uptime in ticks |
| 9  | `sys_read_console_word` | — | r14=word | Read one word from console input |
| 10 | `sys_spawn_static` | r13=name_ptr, r14=name_len | r14=pid | Spawn a statically-named process |
| 11 | *(reserved)* | | | |
| 12 | `sys_open` | r13=path_ptr, r14=path_len, r15=flags | r14=fd | Open file |
| 13 | `sys_close` | r13=fd | — | Close file descriptor |
| 14 | `sys_read` | r13=fd, r14=buf_ptr, r15=len | r14=bytes_read | Read from fd |
| 15 | `sys_write` | r13=fd, r14=buf_ptr, r15=len | r14=bytes_written | Write to fd |
| 16 | `sys_stat` | r13=path_ptr, r14=path_len, r15=stat_ptr | r14=0 or error | Get file metadata |
| 17 | `sys_readdir` | r13=path_ptr, r14=path_len, r15=out_ptr, r16=max | r14=count | List directory |
| 18 | `sys_brk` | r13=new_break | r14=actual_break | Set heap break (absolute) |
| 19 | `sys_sbrk` | r13=increment | r14=old_break | Increment heap break |
| 20 | `sys_fork` | — | r14=child_pid (0 in child) | Fork current process |
| 21 | `sys_exec` | r13=path_ptr, r14=path_len, r15=args_ptr, r16=args_len | r14=0 or error | Exec new program |
| 22 | `sys_write_char` | r13=char | — | Write one character to console |
| 23 | `sys_ipc_send` | r13=pid, r14=word | — | Send IPC message to process |
| 24 | `sys_ipc_recv` | — | r14=word (0 if none) | Non-blocking IPC receive |
| 25 | `sys_fb_init` | r13=width, r14=height | — | Initialize framebuffer |
| 26 | `sys_fb_flip` | — | — | Flip framebuffer pages |
| 27 | `sys_window_create` | r13=x, r14=y, r15=w, r16=h | r14=win_id | Create window |
| 28 | `sys_window_get_buffer` | r13=win_id | r14=buf_ptr | Get window framebuffer pointer |
| 29 | `sys_window_present` | r13=win_id | — | Present/flip window |
| 30 | `sys_window_move` | r13=win_id, r14=x, r15=y | — | Move window |
| 31 | `sys_window_set_z` | r13=win_id, r14=z | — | Set window Z-order |
| 32 | `sys_window_destroy` | r13=win_id | — | Destroy window |
| 33 | `sys_window_read_event` | r13=win_id | r14=event_type, r15=x, r16=y | Poll window event |
| 34 | `sys_window_resize` | r13=win_id, r14=w, r15=h | — | Resize window |
| 35 | `sys_window_request_close` | r13=win_id | — | Request close event |
| 36 | `sys_socket` | r13=domain, r14=type | r14=sock_fd | Create socket |
| 37 | `sys_bind` | r13=sock_fd, r14=addr_ptr, r15=addr_len | — | Bind socket |
| 38 | `sys_connect` | r13=sock_fd, r14=addr_ptr, r15=addr_len | — | Connect socket |
| 39 | `sys_send` | r13=sock_fd, r14=buf_ptr, r15=len | r14=bytes_sent | Send on socket |
| 40 | `sys_recv` | r13=sock_fd, r14=buf_ptr, r15=len | r14=bytes_recv | Receive on socket |
| 41 | `sys_mkdir` | r13=path_ptr, r14=path_len | — | Create directory |
| 42 | `sys_unlink` | r13=path_ptr, r14=path_len | — | Delete file |
| 43 | `sys_waitpid` | r13=pid | r14=exit_status | Wait for process |
| 44 | `sys_exit` | r13=exit_code | — | Exit current process |
| 45 | `sys_sleep` | r13=ticks | — | Sleep for N ticks |
| 46 | `sys_ps` | r13=out_ptr, r14=max_entries | r14=count | List processes |
| 47 | `sys_fsync` | r13=fd | — | Flush file to storage |
| 48 | `sys_kill` | r13=pid, r14=signal | — | Send signal |
| 49 | `sys_suspend` | r13=pid | — | Suspend process |
| 50 | `sys_resume` | r13=pid | — | Resume process |
| 51 | `sys_getproc` | r13=pid, r14=info_ptr | r14=0 or error | Get process info |
| 52 | `sys_futex_wait` | r13=addr, r14=expected | r14=0 or EAGAIN | Wait on futex |
| 53 | `sys_futex_wake` | r13=addr, r14=count | r14=woken | Wake futex waiters |
| 54 | `sys_ipc_recv_blocking` | — | r14=word | Blocking IPC receive |
| 55 | `sys_wait_event` | — | r14=event_type | Block for any IPC or window event |
| 56 | `sys_sleep_ms` | r13=milliseconds | — | Sleep for N milliseconds |
| 57 | `sys_app_spawn` | r13=name_ptr, r14=name_len | r14=pid | Spawn app by registered name |

---

## Notes

- **Pointers** (`*_ptr`) are word addresses in the process's DMEM.
- **Lengths** are in words (not bytes).
- Syscall IDs 6 and 11 are currently reserved.
- Status 2 = **blocked**: the kernel has descheduled the process. It will be rescheduled when the blocking condition resolves (e.g., IPC message arrives, timer fires).
- All pointer arguments must be within the caller's DMEM range; the kernel validates bounds.

## Adding a New Syscall

When adding a syscall, you MUST update ALL of:
1. `kernel.trit` — dispatch table + implementation
2. `ternary_compiler_ir.h` — `runtime::sys_*` constant
3. `SYSCALL_MANIFEST.json` — manifest entry
4. `apps/os_sdk.trit` — SDK wrapper function
5. Test in `test_os_platform.cpp` or `test_phase_d_kernel.cpp`

See `AGENTS.md` for the full checklist.
