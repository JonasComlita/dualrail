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

The current payload format version is 2. The reader still accepts legacy
version 1 images, but the writer always emits version 2.

Version 2 payload fields are serialized in this order:

| Field | Type | Notes |
| --- | --- | --- |
| `format_version` | `uint32` | Always `2` for newly written images |
| `boot_entry` | `int32` | Initial VM PC, relative to the text segment |
| `framebuffer_width` | `int32` | Runtime framebuffer width |
| `framebuffer_height` | `int32` | Runtime framebuffer height |
| `profile_name` | string | `minimum` or `compact` |
| `image_version` | string | Release/build version string |
| `section_count` | `uint32` | Number of section table entries |
| `sections` | `TosImageSection[]` | Kernel and app section metadata |
| `app_count` | `uint32` | Number of app registry entries |
| `apps` | `TosAppManifestEntry[]` | Guest executable registry metadata |
| `program_word_count` | `uint32` | Number of instruction words |
| `program` | `uint64[]` | `TritWord27.bits` for each instruction word |
| `data_word_count` | `uint32` | Number of initial data words |
| `data_words` | `int64[]` | Initial DMEM words |

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
| `page_count` | `int32` | Text page count in 27-word VM pages |
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

Version 2 images do not embed the mutable root filesystem seed. That data is
written as the companion `.tdisk` artifact. Legacy version 1 images may contain
an embedded rootfs word array after `data_words`; the reader can still load it
and seed a missing disk from it.

## `.tdisk` Sparse Disk

`.tdisk` files store only nonzero 27-word VM block-device pages. The format is
little-endian and append-friendly: later records for the same block replace
earlier live contents, and an all-zero record deletes the live block in the
Python inspector/compactor.

| Offset | Field | Type | Notes |
| --- | --- | --- | --- |
| 0 | `magic` | `uint64` | `0x54524954535031` (`TOS_SPARSE_DISK_MAGIC`) |
| 8 | `nonzero_block_count` | `int32` | Number of complete records following the header |

Each block record is:

| Field | Type | Notes |
| --- | --- | --- |
| `block_index` | `int32` | Guest block number |
| `words` | `int64[27]` | One VM block, `MMU_PAGE_WORDS` words |

`writeSparseDiskFile()` writes a compact seed image by scanning a full rootfs
word vector and emitting only blocks that contain at least one nonzero word.
Runtime block writes can append newer records and are compacted by
`tools/trit-compact-disk.ps1` / `python tools/trit_tool.py compact-disk`.

## Release Builder Flow

`build_tos_image.cpp` builds the release pair in one pass:

1. Compile `kernel.trit`.
2. Read `OS3/native_kernel_trap_stub.tasm`.
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
from legacy embedded `rootfs_words`. For current v2 release images,
`rootfs_words` is empty, so a companion `.tdisk` is required.

## Validation Rules

The C++ runtime rejects a boot image when:

- The format version is neither 1 nor 2.
- The text segment is empty.
- `boot_entry` is negative or outside the text segment.
- `profile_name` is empty or not `minimum`/`compact`.
- The framebuffer geometry is not positive.
- Legacy embedded rootfs words are present but not 27-word aligned.
- A version 2 image has no section table entries.
- Any section has an empty name/kind or negative geometry.
- The file magic, payload length, or checksum does not match on disk.

`tools/trit_tool.py inspect-image` mirrors the binary reader closely and reports
the parsed image version, profile, segment sizes, sections, app entries, and
rootfs legacy status. The Python `inspect_sparse_disk()` helper performs the
matching sparse-disk validation used by `compact-disk` and diagnostics export.

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

`test_host_runtime` covers `.tboot` round-tripping, checksum rejection, legacy
rootfs seeding, and separate mutable `.tdisk` boot persistence. `test_os_platform`
and `test_process_handoff` cover the native VFS image and guest executable handoff
that make the release disk meaningful.
