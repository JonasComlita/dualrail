# 08 Filesystem VFS

Focused storage and VFS tests.

Required areas:

- format and mount;
- path resolution including `.`, `..`, and root clamping;
- open/read/write/close/stat/readdir;
- mkdir/unlink/fsync;
- WAL replay before and after commit;
- corrupt image handling;
- large files crossing extents;
- disk-backed executable loading.
