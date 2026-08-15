# Build System, Image Format, and Host Tools

Source of truth: `CMakeLists.txt`, `IMAGE_FORMAT_MANIFEST.json`, `AGENTS.md`, `DEBUGGING.md`

---

## Build System

The project uses **CMake**. Default build directory: `build/` (or `$TRIT_BUILD_DIR`).

### First-time setup

```powershell
cmake -S . -B build
cmake --build build
```

### Key CMake Targets

| Target | Description |
|--------|-------------|
| `build_tos_image` | Build `.tboot` release image (compiles all apps, links, bundles rootfs) |
| `test_host_runtime` | Run host runtime tests |
| `stage_tos_release` | Stage release image for smoke testing |
| `smoke_tos_release` | Run release image smoke test (no SDL) |
| `ci_production` | Full production gate: tests, architecture contract, system workloads, and staged release when SDL is available |

---

## Test Runner

```powershell
# Quick health check
tools/trit-test.ps1 smoke

# OS and kernel tests
tools/trit-test.ps1 os

# Full production gate
tools/trit-test.ps1 production

# Or via Python:
python tools/trit_tool.py test smoke
python tools/trit_tool.py test production
```

Test suites are defined in `TEST_MANIFEST.json`.

---

### Linux and macOS shell wrappers

Each PowerShell host-tool entry point also has a Bash wrapper for Linux,
macOS, and Git Bash. The wrappers resolve the repository from their own path,
select `python3` (falling back to `python`), and forward arguments unchanged
to `tools/trit_tool.py`. Set `TRIT_PYTHON` when Python is not on `PATH`:

```bash
TRIT_PYTHON=/opt/python/bin/python3 tools/trit-doctor.sh
```

The wrappers preserve `TRIT_BUILD_DIR`; commands that accept `--build-dir`
use that environment variable whenever the option is omitted. For example:

```bash
export TRIT_BUILD_DIR="$PWD/build-linux"
tools/trit-doctor.sh
tools/trit-test.sh smoke
tools/trit-run.sh --smoke-test --frames 10 --export-diagnostics build/diag
```

The available wrappers are `trit-bench.sh`, `trit-build-image.sh`,
`trit-compact-disk.sh`, `trit-doctor.sh`, `trit-export-diagnostics.sh`,
`trit-fuzz.sh`, `trit-inspect-image.sh`, `trit-knowledge.sh`,
`trit-replay.sh`, `trit-run.sh`, and `trit-test.sh`. They accept the same
arguments as their `.ps1` counterparts; use `bash tools/<name>.sh --help` when
the filesystem does not preserve executable bits.

## Agent Workflow

```powershell
# 1. Check environment
tools/trit-doctor.ps1

# 2. Run smoke tests
tools/trit-test.ps1 smoke

# 3. Make changes

# 4. Run focused test
cmake --build build --target test_os_platform

# 5. Verify production health
tools/trit-test.ps1 production

# 6. If failures persist, export diagnostics
tools/trit-export-diagnostics.ps1 build/diagnostics
```

---

## .tboot Image Format

Source: `IMAGE_FORMAT_MANIFEST.json`

Magic: `0x31544f4f424f5354` (ASCII "1TOOBoST")

```
.tboot header:
  magic               u64      ← 0x31544f4f424f5354
  version             u32      ← 1
  flags               u32
  entry_point         u32      ← PC at boot (index into program[])
  program_word_count  u32      ← number of TritWord27 instructions
  data_word_count     u32      ← number of T40 data words
  rootfs_word_count   u32      ← number of words in rootfs blob
  framebuffer_width   u32      ← display width in pixels
  framebuffer_height  u32      ← display height in pixels
  app_count           u32      ← number of embedded apps

  program[program_word_count]  ← TritWord27 instruction words (2 bits/trit, uint64_t each)
  data_words[data_word_count]  ← T40 static data (uint64_t each)
  rootfs_words[rootfs_word_count] ← VFS root filesystem blob
  apps[app_count]              ← embedded app headers + binaries
```

**rootfs_word_count must be a multiple of 27** (block size = 27 words).

---

## .tdisk Disk Format

Source: `IMAGE_FORMAT_MANIFEST.json`

Magic: `0x54524954535031` (ASCII "TRITSP1")

```
.tdisk header:
  magic               u64      ← 0x54524954535031
  version             u32      ← 1
  block_count         u32      ← total blocks in disk
  block_size_words    u32      ← always 27 (words per block)
  flags               u32

  blocks[block_count × 27]     ← T40 words (uint64_t each), packed
```

Sparse blocks are zero-filled. Block 0 contains the disk superblock.

---

## Image Builder (`build_tos_image.cpp`)

The image builder:
1. Compiles each app in `apps/` using the host C++ compiler + TCL compiler pipeline
2. Links them to the guest ABI
3. Packs them into an `apps[]` section of the `.tboot` image
4. Writes the rootfs blob from `rootfs/` directory content
5. Writes the kernel binary as the `program[]` section

Verify the output:
```powershell
tools/trit-inspect-image.ps1 build/release/TernaryOS/ternary-os.tboot
```

---

## SDL Runner (`run_tos_sdl.cpp`)

Boots a `.tboot` image in an SDL window:
```powershell
tools/trit-run.ps1 build/release/TernaryOS/ternary-os.tboot
tools/trit-run.ps1 --smoke-test --frames 10 --export-diagnostics build/diag
```

Without SDL, smoke tests run headlessly (console-only mode).

---

## Diagnostics Export

```powershell
tools/trit-export-diagnostics.ps1 [output_dir]
python tools/trit_tool.py export-diagnostics
```

Exports (from `DEBUGGING.md`):
- `vmstate.json` — final VM state
- `regfile.json` — register dump
- `dmem_snapshot.bin` — raw data memory
- `imem_disasm.txt` — disassembly of instruction memory
- `framebuffer_snapshot.txt` — framebuffer as ASCII art
- `syscall_trace.jsonl` — schema-versioned syscall events when tracing is
  enabled (`--export-diagnostics` enables capture); `tools/trit-replay.ps1`
  validates and compares captures
- `input_journal.jsonl` — cycle-stamped keyboard, text, and mouse input
- `checkpoint.json` — metadata for the last VM checkpoint. The host API can
  export a self-contained bundle (`vm_state.bin`, `disk.tdisk`, `boot.tboot`,
  binary/JSONL journals and trace) and restore it into a fresh runtime before
  replay.
- `process_table.json` — OS process table
- `diagnostics.json` — summary

---

The input journal keeps its v1 event schema for existing readers and adds a
deterministic `provenance` object (`source`, `channel`, and `external`) for
each host event. Every diagnostics export also writes a self-contained
`checkpoint/` directory containing `boot.tboot`, `vm_state.bin`, `disk.tdisk`,
`checkpoint.bin`, binary/JSONL journals, and the syscall trace. Root manifest
and checkpoint metadata use paths relative to the diagnostics directory, and
the nested directory can be restored into a fresh runtime before replay.

Replay accepts versioned schema identifiers in the form
`trit.<family>.v<major>[.<minor>]`. Current v1 syscall traces and input
journals (and checkpoint metadata v1/v2) are dispatched to their named
adapters. Compatible future minors are accepted after the existing required
fields and value shapes validate; unknown fields are ignored for comparison
and reported by `replay --json` under `schema_capabilities`. Unsupported
majors fail closed with an explicit error instead of being interpreted as v1.

## Benchmarks

```powershell
tools/trit-bench.ps1
```

Runs `test_benchmark.cpp` targets. Measures:
- Boot time (cycles from reset to first app launch)
- App launch time
- Frame time (desktop compositor)
- Disk I/O throughput
- Context switch overhead

---

## Image Inspector

```powershell
tools/trit-inspect-image.ps1 <path.tboot>
```

Prints:
- Magic and version
- Entry point
- Section counts (program / data / rootfs / apps)
- App inventory (name, entry, size)
- Rootfs block count and layout
- Any format violations
