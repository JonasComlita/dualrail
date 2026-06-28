# Bundled Apps

Source of truth: `build_tos_image.cpp`, `APP_MANIFEST.json`, and `apps/*.trit`.

---

## Packaging Pipeline

`build_tos_image.cpp` owns the release app bundle. For each `BundledApp` entry it:

1. Reads `apps/os_sdk.trit`, `apps/libwidget.trit`, and the app source.
2. Compiles the concatenated source with `compileSource()`.
3. Links with a stack hint and `dead_strip_functions = true`.
4. Assigns a text physical page number, starting at PPN 8100, aligned to 16 pages, with 8 guard pages between apps.
5. Installs the executable image into the root VFS at its guest path.
6. Adds section and app-manifest metadata to the `.tboot` image.
7. Writes GUI launcher records to `/apps/registry` and app metadata files under `/apps/*.app`.
8. Writes `/system/bin.manifest` with `guest_path id source_name` rows.

The builder currently uses these stack classes:

| Class | Stack words | Examples |
|-------|-------------|----------|
| GUI app | 1024 | desktop, terminal, files, settings, tasks, calculator, paint, text editor, about, help |
| Service/session app | 256 | init, shell, sh, top/tasks aliases |
| CLI utility | 128 | ls, bin_core aliases, clear, date, sleep, ps, kill, mount, fsck, sync, reboot, shutdown, login, passwd, service stubs |

`APP_MANIFEST.json` mirrors the bundle inventory and root layout. If it conflicts with `build_tos_image.cpp`, the builder is the executable source of truth.

---

## Root Layout

The image builder starts from `NativeVfsImageBuilder::installBaseLayout()` and then ensures these directories and metadata exist:

| Path | Purpose |
|------|---------|
| `/bin` | Guest executables |
| `/apps/registry` | GUI launcher records only |
| `/apps/<id>.app` | GUI app metadata files |
| `/system/bin.manifest` | All bundled guest paths and source names |
| `/system/services/manifest` | Service role descriptions |
| `/system/services/<name>` | Enabled markers for service stubs |
| `/etc/os-release`, `/etc/issue`, `/etc/motd`, `/etc/profile` | User-visible system metadata |
| `/dev/*` | Seed device nodes such as `console`, `keyboard`, `mouse`, `fb0`, and `disk0` |
| `/var/log`, `/var/crash`, `/var/packages` | Logs, crash landing area, package status |
| `/home/root` | Default root user home |

The default root user is installed with home `/home/root` and shell `/bin/desktop`.

---

## Boot And Session Apps

| ID | Guest path | Source | Notes |
|----|------------|--------|-------|
| `init` | `/bin/init` | `apps/init.trit` | First user process. Seeds `/bin/desktop` into scratch memory and calls `sys_exec()` in a loop. |
| `desktop` | `/bin/desktop` | `apps/desktop.trit` | Main session surface. In standalone test mode it prints/draws markers; in OS mode it checks `scratch`, shows login if flag `62000` is not set, draws the desktop HUD/launcher, and spawns apps with `os_spawn_app()`. |
| `shell` | `/bin/shell` | `apps/shell.trit` | Readline shell with built-ins and a `/bin/<cmd>` fallback through `sys_exec()`. |
| `sh` | `/bin/sh` | `apps/shell.trit` | Alias build of the shell source. |
| `terminal` | `/bin/terminal` | `apps/terminal.trit` | Windowed terminal surface. In OS mode it tries to exec `/bin/shell`; if not replaced, it draws a terminal frame and waits for close/key events. |

Desktop launcher hotkeys map to calculator, task manager, paint, file manager, settings, and terminal. The desktop also has an in-app login dialog; the current passcode check in source accepts ASCII `777`.

---

## GUI Registry Apps

These apps are included in `/apps/registry` and get `/apps/<id>.app` metadata.

| ID | Guest path | Source | Architecture notes |
|----|------------|--------|--------------------|
| `desktop` | `/bin/desktop` | `desktop.trit` | Text-mode desktop, login, launcher, and child app spawning. |
| `terminal` | `/bin/terminal` | `terminal.trit` | Shell launcher plus terminal frame/event loop. |
| `files` | `/bin/file_manager` | `file_manager.trit` | Draws a file-manager frame and navigates back to desktop; current UI is mostly static shell around VFS affordances. |
| `settings` | `/bin/settings` | `settings.trit` | Tabbed settings UI for display, audio, users, and system pages; uses widget sliders and text fields. |
| `tasks` | `/bin/task_manager` | `task_manager.trit` | Process table UI backed by process snapshot syscalls; draws state labels and process controls. |
| `calculator` | `/bin/calculator` | `calculator.trit` | Button-grid calculator with widget routing, arithmetic state, and a small window-surface path. |
| `paint` | `/bin/paint` | `paint.trit` | Text/GPU drawing app with toolbar buttons, color selection, clear/exit actions, and pointer/key handling. |
| `text_editor` | `/bin/text_editor` | `text_editor.trit` | Text frame and note-editing surface; also built as CLI alias `/bin/edit`. |
| `about` | `/bin/about` | `about.trit` | Static about panel. |
| `help` | `/bin/help` | `help.trit` | Static help panel. |

Most GUI apps use a direct text framebuffer path (`os_text_cell`, `os_clear_text`, GPU helpers) plus selected `libwidget.trit` routines for buttons, fields, list boxes, and sliders. A smaller subset uses `os_window_*` APIs to exercise the native window service path.

---

## Shell And CLI Utilities

The shell has built-ins for:

| Command | Behavior |
|---------|----------|
| `help` | Prints command list. |
| `calc`, `paint`, `tasks`, `files`, `settings` | Seeds a guest path and `sys_exec()`s the corresponding app. |
| `ls` | Reads `/` through `os_readdir()` and prints entries. |
| `ps` | Calls `os_ps()` and prints process rows. |
| `exit` | Leaves the shell loop. |
| Other input | Builds `/bin/<input>` and tries `sys_exec()`. |

Bundled CLI sources:

| Source | Guest paths | Notes |
|--------|-------------|-------|
| `apps/ls.trit` | `/bin/ls` | Dedicated directory listing app. |
| `apps/bin_core.trit` | `/bin/cat`, `/bin/echo`, `/bin/pwd`, `/bin/cp`, `/bin/mv`, `/bin/rm`, `/bin/mkdir`, `/bin/rmdir`, `/bin/touch`, `/bin/stat`, `/bin/find`, `/bin/grep` | Shared minimal implementation currently prints `CORE UTIL`. |
| `apps/clear.trit` | `/bin/clear` | Clears terminal text output. |
| `apps/date.trit` | `/bin/date` | Prints uptime through `sys_uptime()`. |
| `apps/sleep.trit` | `/bin/sleep` | Sleeps/yields through the SDK wrapper. |
| `apps/ps.trit` | `/bin/ps` | Process listing command. |
| `apps/kill.trit` | `/bin/kill` | Process signal command. |
| `apps/mount.trit` | `/bin/mount` | Mount/status command surface. |
| `apps/fsck.trit` | `/bin/fsck` | Filesystem check/status command surface. |
| `apps/sync.trit` | `/bin/sync` | Filesystem sync command. |
| `apps/reboot.trit`, `apps/shutdown.trit` | `/bin/reboot`, `/bin/shutdown` | Power-control commands. |
| `apps/login.trit`, `apps/passwd.trit` | `/bin/login`, `/bin/passwd` | Account/session commands. |

---

## Service Stubs

`apps/service_stub.trit` is compiled repeatedly under service names:

```text
sessiond window_server compositor inputd mountd logd crashd updated packaged devd timed authd powerd
```

The current implementation prints `SERVICE READY` and exits. The service manifest in `/system/services/manifest` documents intended ownership, but the daemons are not full long-running service managers yet.

---

## Validation

Relevant tests and targets:

- `cmake --build build --target build_tos_image`
- `tools/trit-inspect-image.ps1 build/release/TernaryOS/ternary-os.tboot`
- `tools/trit-test.ps1 apps`
- `test_native_apps`
- `test_process_handoff`
- `test_consumer_shell_productization`
- `test_host_runtime`
