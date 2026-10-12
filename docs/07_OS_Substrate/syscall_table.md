# Current syscall table

Sources: `kernel.trit`, `ternary_compiler_ir.h`, `ternary_compiler_codegen.h`,
`apps/os_sdk.trit`, and `SYSCALL_MANIFEST.json`.

Syscall ABI2 places the ID in CSR syscall_id and arguments in r13..r16.
Pointers are caller DMEM word addresses; paths are bounded NUL-terminated word
strings, not host byte strings. On raw return r13=status, r14=payload, r15=detail:
status1 succeeds, status0 is service-specific empty/poll/EOF, status2 blocks,
negative status errors. Most errors use status-1/payload0/negative errno in detail.
Service60 uses status-1 and the negative error in both payload/detail. Reserved
or unmapped IDs fail; there is no retired dispatch compatibility.

The compiler/SDK payload-return expressions select negative detail when status
is negative and detail is negative, otherwise negative status; nonnegative status
preserves payload, including fd0, EOF0 and a negative child exit status. This
selection does not alter raw ABI fields. Status-return calls use r13. `sys_app_spawn`
has an existing positive-detail failure form, so the SDK returns its negative
status fallback; it does not invent a negative errno from positive detail.

| ID | Current name | Ordered arguments (r13 onward) | Raw payload / behavior |
|---|---|---|---|
| 1 | `sys_write_int` | value | echo value |
| 2 | `sys_newline` | — | 0 |
| 3 | `sys_clear` | — | 0 |
| 4 | reserved | — | rejected |
| 5 | reserved | — | rejected |
| 6 | reserved | — | rejected |
| 7 | `sys_getpid` | — | caller PID |
| 8 | `sys_uptime` | — | CYCLE virtual nanoseconds |
| 9 | `sys_read_console_word` | — | console word; 0 if unavailable |
| 10 | `sys_spawn_static` | not dispatched | retained runtime name; current kernel rejects |
| 11 | reserved | — | rejected |
| 12 | `sys_open` | path_addr, mode | fd (including0) |
| 13 | `sys_close` | fd | 0 (SDK returns status) |
| 14 | `sys_read` | fd, out_addr, count_words | read words; 0 at EOF |
| 15 | `sys_write` | fd, src_addr, count_words | written words, possibly short/0 |
| 16 | `sys_stat` | path_addr, out_addr | stat result; writes output record |
| 17 | `sys_readdir` | path_addr, out_addr, max_words | output words |
| 18 | `sys_brk` | new_break | actual break |
| 19 | `sys_sbrk` | increment_words | previous break |
| 20 | `sys_fork` | child_slot, child_pid | child PID |
| 21 | `sys_exec` | path_addr | installed image words |
| 22 | `sys_write_char` | character | echo character |
| 23 | `sys_ipc_send` | channel, word | sent count |
| 24 | `sys_ipc_recv` | channel, out_addr | received count; writes word at out_addr |
| 25 | `sys_fb_init` | width, height | framebuffer base |
| 26 | `sys_fb_flip` | — | flip result |
| 27 | `sys_window_create` | x, y, width, height | window ID |
| 28 | `sys_window_get_buffer` | window_id | buffer address |
| 29 | `sys_window_present` | window_id | present result |
| 30 | `sys_window_move` | window_id, x, y | move result (SDK status) |
| 31 | `sys_window_set_z` | window_id, z | z-order result (SDK status) |
| 32 | `sys_window_destroy` | window_id | destroy result (SDK status) |
| 33 | `sys_window_read_event` | window_id, out_addr | event result; writes event record |
| 34 | `sys_window_resize` | window_id, width, height | resize result (SDK status) |
| 35 | `sys_window_request_close` | window_id | close-request result |
| 36 | `sys_socket` | kind | socket ID |
| 37 | `sys_bind` | socket_id, port | bind result |
| 38 | `sys_connect` | socket_id, remote_port | connect result |
| 39 | `sys_send` | socket_id, word | sent word result |
| 40 | `sys_recv` | socket_id, out_addr | received count; writes out_addr |
| 41 | `sys_mkdir` | path_addr | new inode |
| 42 | `sys_unlink` | path_addr | unlink result |
| 43 | `sys_waitpid` | child_pid | child exit status (may be negative) |
| 44 | `sys_exit` | code | exit code; process becomes terminal |
| 45 | `sys_sleep` | — | blocks one timer quantum |
| 46 | `sys_ps` | out_addr, max_entries | entry count; five words/entry |
| 47 | `sys_fsync` | fd | targeted flush result |
| 48 | `sys_kill` | pid, signal | signal result |
| 49 | `sys_suspend` | pid | stop result |
| 50 | `sys_resume` | pid | continue result |
| 51 | `sys_getproc` | pid, out_addr | process result; writes eight words |
| 52 | `sys_futex_wait` | addr, expected, timeout_ms | 0 on mismatch; otherwise blocks |
| 53 | `sys_futex_wake` | addr, count | number woken |
| 54 | `sys_ipc_recv_blocking` | channel, out_addr, timeout_ms | received count or blocks |
| 55 | `sys_wait_event` | window_id, out_addr, timeout_ms | event result or poll/block |
| 56 | `sys_sleep_ms` | ms | 1 for nonpositive duration; otherwise blocks |
| 57 | `sys_app_spawn` | path_addr, child_slot, child_pid | child PID |
| 58 | `sys_reboot` | — | 1; requires system capability |
| 59 | `sys_rename` | old_path_addr, new_path_addr | rename result |
| 60 | `sys_service_metrics` | out_addr, out_words | 7; writes fixed record |

`sys_yield` and `sys_sleep_until_tick` are aliases of45 (`sys_sleep`),
not IDs4/5 and not separate absolute-tick APIs. A sleep45 waits one timer quantum.
IPC channels and sockets are namespace-local word queues, not byte-stream networking.
`os_getproc` writes `[pid,state,parent_pid,exit_status,pending_signals,priority,quota_remaining,namespace_id]`;
`os_ps` rows are `[pid,state,priority,quota_remaining,namespace_id]`.

Service60 requires out_words>=7 and an integral pointer/count; it validates the
entire advertised writable span before copying. Its version1 record is
`[version,caller_pid,kernel_tick,namespace_process_count,namespace_runnable_count,namespace_window_event_count,namespace_window_event_capacity]`.
The runnable count includes running processes. Success is raw `(1,7,0)`. Invalid
length returns ERR_INVALID(-8); bad pointer/span returns ERR_BAD_PTR(-5).

Positive finite timeouts52/54/55/56 are integral virtual milliseconds through
2,147,483,647. Nonpositive52/54 waits are indefinite;55 polls;56 returns immediately.
Existing ready/pointer/error checks retain priority. One CYCLE is one virtual
nanosecond; one millisecond is1,000,000 cycles. Delivered kernel IRQ ticks (metrics
field2) are counted separately. Timer-wheel catchup examines at most243 buckets
per scan. Deadlines round upward and may resume late. Service sampling uses an
absolute250,000,000-cycle period, not250 delivered ticks or host wall milliseconds.
Host wall time and emulation factor are measured separately.

Current errno names are defined at the top of `kernel.trit`: NOT_FOUND-1, EXISTS-2,
NO_SPACE-3, BAD_FD-4, BAD_PTR-5, NOT_DIR-6, IS_DIR-7, INVALID-8, EOF-9, TIMEOUT-10,
AGAIN-11, CANCELED-12. Poll/EOF/expected-value mismatch may use nonnegative status
with negative detail; consult the raw triple rather than assuming detail alone
is the SDK scalar error.

Adding a service requires coordinated current kernel/runtime/compiler mapping,
SDK (when exposed), manifest and conformance updates described in `AGENTS.md`.
