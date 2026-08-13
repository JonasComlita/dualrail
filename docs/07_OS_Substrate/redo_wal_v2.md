# Redo WAL v2

The v2 write-ahead log is a control-plane redo log. It protects durable VFS
metadata, namespace and policy configuration, snapshots, and reconciliation
state. Scheduler queues, process lifecycle state, quota usage counters, and
physical-page allocation fast paths are reconstructed after boot and do not
generate WAL traffic.

## Physical layout

- Storage transfer block: 27 T40 words.
- Ring: 729 blocks.
- Superblocks: two alternating checksummed blocks.
- Record header: 12 words.
- After-image payload: up to 15 words.
- Record types: data, commit, and checkpoint.

The header fields are magic, version, generation, LSN, transaction ID,
previous LSN, record type, target kind, target ID, target offset, word count,
and checksum. Recovery stops at the first invalid or torn record. A commit is
replayed only when its data records form a complete previous-LSN chain ending
at the commit; a stale valid block cannot supply a suffix of another
transaction.

## Commit and flush ordering

`log_write` buffers after-images in the in-memory ring. `log_commit` appends a
commit record and makes the transaction visible in the cache; it does not
issue a stable-storage barrier. Abort discards unpublished records and has no
undo phase.

A group is flushed when explicitly requested, at 27 transactions, at 27 WAL
blocks, or on the first timer tick after a commit becomes pending. Home pages
cannot be written while their required LSN is newer than the durable LSN.

Kernel buffer frames carry their owning inode and page LSN; dirty frames cannot
be written or evicted while their page LSN is newer than the durable LSN. Each
inode also tracks the greatest commit LSN required by its dirty data and metadata.

`fsync(fd)` orders durability as:

1. Append and flush WAL through the inode's required LSN.
2. Write that inode's dirty data and metadata.
3. Issue one data barrier.

Thus ordinary writes issue no stable flush, while a targeted write plus
`fsync` needs at most one WAL barrier and one data barrier. The kernel's
`vfs_fsync` writes only the fd inode's inode row, directory entry/name, extent
rows, and extent payloads; unrelated dirty inode frames remain dirty.

Appending into a file that needs a new extent logs the complete extent row and
allocator cursors in the same transaction as payload, inode-size, and mtime
after-images. The standalone allocation and growth helpers use that same
transactional reservation path; the allocator is not advanced in the cache
until the logical commit, so a durable redo commit can recreate the extent
before a home-page checkpoint. A write that runs out of WAL space reports the
number of committed words and advances the descriptor by exactly that amount.

Create/mkdir now use one WAL transaction for the inode row, inode cursor,
directory row/name, and directory cursor; create timestamps are part of that
after-image. Tombstoned inode, dirent, and extent rows are first-fit reusable;
extent tombstones retain their `DATA_ADDR`/`LENGTH` span, while legacy v2
length-zero tombstones remain valid but provide no reusable data hole.
Truncate and unlink log inode size/link state, directory tombstones, extent
version tombstones, and mtime before committing. Unlink leaves a links=0
orphan readable through matching open descriptors; close, process cleanup, and
mount perform bounded reclaim passes only after the generation-checked FD set
is empty. `vfs_sync_to_disk`
also includes the greatest outstanding inode dependency when enforcing the
no-steal durable-LSN rule. `quota_set_limit` and `namespace_create` log all
fields in their control-plane rows rather than mutating unlogged companion
fields after commit. Native VFS layout version 2 gives namespace rows ten
dedicated home-page blocks and quota policy fifteen compact home-page blocks.
Quota ID, configured limit, and version survive checkpoints; instantaneous
usage is reset and reconstructed after boot. The image builder seeds namespace
zero and all default quota policies, and executable payload allocation begins
after block 8042 so it cannot overlap the control-plane homes.

`sys_rename(old_path, new_path)` (service 59) is the bounded atomic rename
slice. It operates in the caller's process namespace and accepts only an
existing regular file. Both user strings are copied, with independent
64-word bounds, into non-overlapping kernel scratch before any VFS lookup.
Source and destination parents must resolve to directories; the root,
directories, malformed pointers, and an existing destination are rejected
without a metadata mutation. An identical old/new path is a no-op.

The source directory-entry slot is reused, preserving the dirent and extent
cursors and the inode generation used by open descriptors. One redo
transaction carries the parent, hash, name length, complete bounded name
payload (including zeroed trailing words), and dirent version after-images,
plus the inode parent flag, ctime, and mtime. The transaction reserves all
data records and its commit record before starting, aborts unpublished records
on write failure, and stamps the committed LSN onto the inode dependency
tracker. Recovery therefore replays a committed rename as a unit and leaves
the old name visible when a data/commit chain is torn or incomplete.

The following paths remain intentionally outside this completed atomic slice:
high-water allocator cursors are monotonic (there is no global compaction);
destination replacement and directory renames are not supported; and quota
usage/physical-page allocation are volatile reconciliation state.

Checkpointing first makes committed WAL durable, then writes home pages and
issues their data barrier, appends and durably flushes a checkpoint record, and
only then advances the WAL tail in a new superblock.

## Executable recovery oracle

`ternary_redo_wal.h` is the dependency-free host recovery oracle for the same
record contract. `test_redo_wal_v2` verifies logical-versus-durable commit,
group/timer flushing, abort without undo, torn commits, alternating
superblocks, redo replay, and the two-barrier `fsync` gate.

