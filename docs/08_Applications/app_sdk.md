# App SDK and Widget Toolkit

Source of truth: `apps/os_sdk.trit`, `apps/libwidget.trit`, `APP_MANIFEST.json`

---

## App SDK (`apps/os_sdk.trit`)

Every bundled app links against `os_sdk.trit`. It provides:

### Syscall Wrappers

Thin TCL wrappers for all 57 kernel syscalls. Apps should always call these wrappers rather than using raw `SYSCALL` instructions.

```tcl
// Examples (TCL syntax):
fn open(path: str, flags: int) -> int { sys_open(...) }
fn write(fd: int, buf: *byte, len: int) -> int { sys_write(...) }
fn fork() -> int { sys_fork() }
fn exit(code: int) { sys_exit(code) }
```

### Memory Allocator

- `malloc(n: int) -> *byte` — allocate N bytes from the heap
- `free(ptr: *byte)` — free heap allocation
- `realloc(ptr: *byte, n: int) -> *byte`

Uses `sys_sbrk` internally to grow the heap.

### String Functions

- `strlen(s: str) -> int`
- `strcpy(dst: *byte, src: *byte)`
- `strcmp(a: str, b: str) -> int`
- `strcat(dst: *byte, src: *byte)`
- `itoa(n: int, buf: *byte)` — integer to string
- `atoi(s: str) -> int` — string to integer

### Console I/O

- `print(s: str)` — write string to fd 1 (stdout)
- `println(s: str)` — print + newline
- `print_int(n: int)` — print integer
- `read_line(buf: *byte, max: int) -> int` — read line from stdin

### Environment

- `getenv(key: str) -> str` — read environment variable
- `getpid() -> int` — get current PID
- `uptime() -> int` — get system uptime

### Process Management

- `spawn(name: str) -> int` — spawn app by name, returns PID
- `waitpid(pid: int) -> int` — wait for process exit

### IPC

- `ipc_send(pid: int, msg: int)` — send message
- `ipc_recv() -> int` — non-blocking receive
- `ipc_recv_blocking() -> int` — blocking receive

---

## Widget Toolkit (`apps/libwidget.trit`)

All GUI apps link against `libwidget.trit`. It builds on top of `os_sdk.trit`.

### Widget Hierarchy

```
Widget (base)
├── Window      — top-level OS window
├── Panel       — container for other widgets
├── Label       — text display
├── Button      — clickable button
├── TextInput   — single-line text entry
├── TextArea    — multi-line text
├── ListBox     — scrollable item list
├── ScrollPane  — scrollable container
├── Canvas      — pixel/drawing surface
├── MenuBar     — menu strip
├── MenuItem    — menu item
├── Toolbar     — icon toolbar
├── StatusBar   — bottom status line
├── Slider      — numeric range control
├── CheckBox    — boolean toggle
├── RadioButton — exclusive selection
├── ProgressBar — progress indicator
└── Splitter    — resizable pane divider
```

### Window Lifecycle

```tcl
let win = Window.new(x, y, width, height, title)
win.show()

loop {
    let evt = win.wait_event()     // sys_wait_event
    win.dispatch(evt)              // routes to widget handlers
    win.draw()                     // calls widget.paint() for dirty regions
    win.present()                  // sys_window_present
}

win.destroy()
```

### Drawing API (Canvas)

```tcl
canvas.clear(color)
canvas.fill_rect(x, y, w, h, color)
canvas.draw_rect(x, y, w, h, color)
canvas.draw_text(x, y, text, color)
canvas.draw_line(x0, y0, x1, y1, color)
canvas.blit(src_canvas, dst_x, dst_y)
```

### Event Types

| Event | Description |
|-------|-------------|
| `EVT_MOUSE_MOVE`  | Mouse moved |
| `EVT_MOUSE_DOWN`  | Mouse button pressed |
| `EVT_MOUSE_UP`    | Mouse button released |
| `EVT_KEY_DOWN`    | Key pressed |
| `EVT_KEY_UP`      | Key released |
| `EVT_CLOSE`       | Window close requested |
| `EVT_RESIZE`      | Window resized |
| `EVT_PAINT`       | Redraw requested |
| `EVT_IPC`         | IPC message received |

---

## Bundled Apps Inventory

From `APP_MANIFEST.json`:

| App | Binary path(s) | Description |
|-----|----------------|-------------|
| `init` | `/sbin/init` | PID 1 supervisor |
| `desktop` | `/apps/desktop` | Desktop compositor + dock |
| `terminal` | `/apps/terminal` | Terminal emulator |
| `shell` | `/bin/shell`, `/bin/sh` | Command interpreter |
| `calculator` | `/apps/calculator` | GUI calculator |
| `paint` | `/apps/paint` | Bitmap paint |
| `text_editor` | `/apps/text_editor`, `/bin/edit` | Text editor |
| `file_manager` | `/apps/file_manager` | File browser GUI |
| `task_manager` | `/apps/task_manager`, `/bin/top` | Process viewer |
| `settings` | `/apps/settings` | System settings panel |
| `about` | `/apps/about` | About dialog |
| `help` | `/apps/help` | Help viewer |
| `tcc` | `/bin/tcc` | In-OS TCL compiler |
| `ls` | `/bin/ls` | List directory |
| `cat` | `/bin/cat` | Concatenate files |
| `echo` | `/bin/echo` | Print arguments |
| `pwd` | `/bin/pwd` | Print working directory |
| `cp` | `/bin/cp` | Copy file |
| `mv` | `/bin/mv` | Move file |
| `rm` | `/bin/rm` | Remove file |
| `mkdir` | `/bin/mkdir` | Create directory |
| `ps` | `/bin/ps` | List processes |
| `kill` | `/bin/kill` | Send signal |
| `clear` | `/bin/clear` | Clear terminal |
| `date` | `/bin/date` | Print date/time |
| `sleep` | `/bin/sleep` | Sleep N seconds |
| `mount` | `/bin/mount` | Mount filesystem |
| `fsck` | `/bin/fsck` | Filesystem check |
| `sync` | `/bin/sync` | Sync filesystem |
| `reboot` | `/bin/reboot` | Reboot system |
| `shutdown` | `/bin/shutdown` | Shutdown system |
| `login` | `/bin/login` | Login manager |
| `passwd` | `/bin/passwd` | Change password |
| `window_probe` | `/bin/window_probe` | Window system diagnostic |
| *(daemons)* | `/bin/sessiond`, `/bin/logd`, `/bin/window_server` | Service stubs |

---

## Adding a New App

See `AGENTS.md` for the full checklist. Summary:

1. Create `apps/<name>.trit`
2. Link against `os_sdk.trit` (always) and `libwidget.trit` (if GUI)
3. Add to `build_tos_image.cpp` build list
4. Add entry to `APP_MANIFEST.json` with `guest_path` and `stack_words`
5. Write a test in `test_native_apps.cpp` or `test_process_handoff.cpp`
6. Rebuild with `cmake --build build --target build_tos_image`
7. Inspect with `tools/trit-inspect-image.ps1`
