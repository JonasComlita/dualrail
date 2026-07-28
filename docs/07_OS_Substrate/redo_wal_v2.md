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
and checksum. Recovery stops at the first invalid or torn record.

## Commit and flush ordering

`log_write` buffers after-images in the in-memory ring. `log_commit` appends a
commit record and makes the transaction visible in the cache; it does not
issue a stable-storage barrier. Abort discards unpublished records and has no
undo phase.

A group is flushed when explicitly requested, at 27 transactions, at 27 WAL
blocks, or on the first timer tick after a commit becomes pending. Home pages
cannot be written while their required LSN is newer than the durable LSN.

`fsync(fd)` orders durability as:

1. Append and flush WAL through the inode's required LSN.
2. Write that inode's dirty data and metadata.
3. Issue one data barrier.

Thus ordinary writes issue no stable flush, while a targeted write plus
`fsync` needs at most one WAL barrier and one data barrier.

## Executable recovery oracle

`ternary_redo_wal.h` is the dependency-free host recovery oracle for the same
record contract. `test_redo_wal_v2` verifies logical-versus-durable commit,
group/timer flushing, abort without undo, torn commits, alternating
superblocks, redo replay, and the two-barrier `fsync` gate.

