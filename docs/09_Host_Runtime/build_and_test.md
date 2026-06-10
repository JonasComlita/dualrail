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
| `ci_production` | Full CI production build (build + all tests) |

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
- `syscall_trace.jsonl` — syscall trace (placeholder; not yet implemented)
- `process_table.json` — OS process table
- `diagnostics.json` — summary

---

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
