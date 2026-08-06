# Image Format

Ternary OS release images are split into two host-side artifacts:

- `.tboot`: immutable boot metadata, kernel/trap text, app section metadata, and initial data words.
- `.tdisk`: mutable sparse block backing for the guest root filesystem.

The source of truth is `ternary_host_runtime.h`. `IMAGE_FORMAT_MANIFEST.json`
summarizes the contract for tools, while `build_tos_image.cpp` is the release
builder that emits the current artifacts.

## `.tboot` Boot Image

`.tboot` files are little-endian binary files with a fixed 24-byte file header
followed by a checksummed payload.

| Offset | Field | Type | Notes |
| --- | --- | --- | --- |
| 0 | `magic` | `uint64` | `0x31544f4f424f5354` (`TOS_BOOT_MAGIC`) |
| 8 | `fnv1a_checksum` | `uint64` | FNV-1a over the payload bytes |
| 16 | `payload_size` | `uint64` | Payload length in bytes |
| 24 | `payload` | bytes | Serialized `TosBootImage` payload |

The current payload format version is 3. Production readers accept only v3;
v1 and v2 payloads are rejected with guidance to use the standalone offline
migrator. The writer always emits v3.

Version 3 payload fields are serialized in this order:

| Field | Type | Notes |
| --- | --- | --- |
| `format_version` | `uint32` | Always `3` for newly written images |
| `boot_entry` | `int32` | Initial VM PC, relative to the text segment |
| `framebuffer_width` | `int32` | Runtime framebuffer width |
| `framebuffer_height` | `int32` | Runtime framebuffer height |
| `profile_name` | string | `minimum` or `compact` |
| `image_version` | string | Release/build version string |
| `isa_version` | `int32` | Must be ISA v2 |
| `required_features` | `uint64` | Required v2 feature bits |
| `scalar_word_trits` | `int32` | Must be 40 |
| `base_page_words` | `int32` | Must be 729 |
| `function_abi_version` | `int32` | Must be 2 |
| `syscall_abi_version` | `int32` | Must be 2 |
| `section_count` | `uint32` | Number of section table entries |
| `sections` | `TosImageSection[]` | Kernel and app section metadata |
| `app_count` | `uint32` | Number of app registry entries |
| `apps` | `TosAppManifestEntry[]` | Guest executable registry metadata |
| `program_word_count` | `uint32` | Number of instruction words |
| `program` | `uint64[]` | `TritWord27.bits` for each instruction word |
| `data_word_count` | `uint32` | Number of initial data words |
| `data_words` | `uint64[]` | Canonical raw T40 words |

Strings are encoded as `uint32 byte_length` followed by raw bytes. The host
tooling decodes them as UTF-8 for inspection, but the C++ runtime stores them as
plain `std::string` byte sequences.

`TosImageSection` entries have this layout:

| Field | Type | Meaning |
| --- | --- | --- |
| `name` | string | Logical section name, such as `kernel` or an app id |
| `path` | string | Guest path, such as `/kernel` or `/bin/desktop` |
| `kind` | string | Current builder uses `kernel` and `app` |
| `load_address` | `int32` | Word address for the section |
| `entry_pc` | `int32` | Entry PC for executable sections |
| `word_count` | `int32` | Text word count |
| `page_count` | `int32` | Text page count in 729-word v2 base pages |
| `flags` | `int32` | Executable/kernel/app bit flags |

`TosAppManifestEntry` entries have this layout:

| Field | Type | Meaning |
| --- | --- | --- |
| `name` | string | App id |
| `path` | string | Guest executable path |
| `text_ppn` | `int32` | Guest text physical page number |
| `entry_pc` | `int32` | App entry virtual PC |
| `text_pages` | `int32` | App text pages |
| `data_pages` | `int32` | App data pages |
| `stack_words` | `int32` | Stack hint chosen by the builder/linker |
| `isa_version` | `int32` | Must be ISA v2 |
| `required_features` | `uint64` | Required v2 feature bits |
| `function_abi_version` | `int32` | Must be 2 |
| `syscall_abi_version` | `int32` | Must be 2 |

Version 3 images do not embed mutable root filesystem state. That data is
written as the companion `.tdisk` artifact. Legacy v1/v2 images may contain an
embedded rootfs word array, but only the offline migrator reads that field.

## `.tdisk` Sparse Disk

`.tdisk` files store only nonzero 27-word VM block-device pages. The format is
little-endian and compact: each record represents one live block, and an
all-zero block is omitted from the canonical image.

| Offset | Field | Type | Notes |
| --- | --- | --- | --- |
| 0 | `magic` | `uint64` | `0x54524954535032` (`TOS_SPARSE_DISK_MAGIC`) |
| 8 | `version` | `uint32` | Always `2` |
| 12 | `block_words` | `uint32` | Always `27` |
| 16 | `generation` | `uint64` | Monotonic compact-image generation |
| 24 | `fnv1a_checksum` | `uint64` | FNV-1a over block indexes and raw T40 words |
| 32 | `nonzero_block_count` | `int32` | Number of complete records following the header |

Each block record is:

| Field | Type | Notes |
| --- | --- | --- |
| `block_index` | `int32` | Guest block number |
| `words` | `uint64[27]` | One block of canonical raw T40 words |

`writeSparseDiskFile()` writes a compact seed image by scanning a full rootfs
word vector and emitting only blocks that contain at least one nonzero word.
Runtime writes rewrite the canonical v2 image atomically; compaction and
diagnostics use the same v2 format. A legacy `TRITSP1` image is rejected by the
live runtime and must be converted offline.

## Release Builder Flow

`build_tos_image.cpp` builds the release pair in one pass:

1. Compile `kernel.trit`.
2. Read `native_kernel_trap_stub.tasm`.
3. Compile every bundled app from `apps/os_sdk.trit`, `apps/libwidget.trit`,
   and the app source.
4. Link each app with a stack hint: GUI apps use 1024 words, session/service
   apps use 256 words, and CLI utilities use 128 words.
5. Assign app text pages starting at physical page 8100, aligned to 16-page
   boundaries with 8 guard pages after each executable.
6. Build a native VFS root image with `/bin/*`, `/apps/registry`,
   `/system/bin.manifest`, service markers, `/etc/*`, `/dev/*`, and `/home/root`.
7. Assemble boot code that starts `/bin/desktop`, then append the trap stub and
   compiled kernel assembly.
8. Emit `.tboot` with the kernel section and one section/app manifest entry per
   bundled app.
9. Emit `.tdisk` from the native VFS root image.

The `.tboot` section table is metadata for inspection and runtime diagnostics;
the actual app executable bytes live in the guest filesystem stored by `.tdisk`.

## Loader Behavior

`readBootImageFile()` validates the file header, verifies the FNV-1a checksum,
deserializes the payload, and then runs `validateBootImage()`.

`loadBootImageIntoVm()` then:

1. Validates the image again.
2. Rejects text or data segments that exceed VM instruction/data memory.
3. Cold-resets the VM.
4. Loads `program` into IMEM at address 0.
5. Stores `data_words` into DMEM starting at address 0.
6. Sets `pc` to `boot_entry`.
7. Advances `standalone_heap_break` beyond the data segment plus 16 words.
8. Attaches the configured `.tdisk` file if one is supplied.

If a disk path is supplied and the file is missing, the runtime can create it
from an embedded seed only when the image object explicitly carries one. Current
v3 release images leave `rootfs_words` empty, so a companion v2 `.tdisk` is
required. Existing legacy or corrupt `.tdisk` files are rejected with offline
migration guidance; they are never mounted live.

## Validation Rules

The C++ runtime rejects a boot image when:

- The format version is not 3.
- The text segment is empty.
- `boot_entry` is negative or outside the text segment.
- `profile_name` is empty or not `minimum`/`compact`.
- The framebuffer geometry is not positive.
- An embedded rootfs seed is present but not 27-word aligned.
- The image has no section table entries.
- Any section has an empty name/kind or negative geometry.
- The file magic, payload length, or checksum does not match on disk.

`tools/trit_tool.py inspect-image` is an offline inspector and can still report
preserved v1/v2 fixtures for migration analysis; it is not the production
loader. The Python `inspect_sparse_disk()` helper performs matching v2 sparse-
disk validation for compaction and diagnostics export. Use
`migrate_tos_artifacts` to convert preserved legacy inputs into fresh v2/v3
artifacts before booting.

## Useful Commands

```powershell
tools/trit-build-image.ps1
tools/trit-inspect-image.ps1 build/release/TernaryOS/ternary-os.tboot
tools/trit-compact-disk.ps1 build/release/TernaryOS/ternary-os.tdisk
tools/trit-export-diagnostics.ps1
```

Focused validation targets:

```powershell
cmake --build build --target test_host_runtime
cmake --build build --target test_os_platform
cmake --build build --target test_process_handoff
cmake --build build --target stage_tos_release
cmake --build build --target smoke_tos_release
```

`test_host_runtime` covers v3 `.tboot` round-tripping, checksum rejection, and
separate mutable `.tdisk` boot persistence. `test_migration_v2` proves that
legacy boot/disk fixtures are rejected by production loading and remain usable
through the offline migrator. `test_os_platform` and `test_process_handoff`
cover the native VFS image and guest executable handoff that make the release
disk meaningful.
