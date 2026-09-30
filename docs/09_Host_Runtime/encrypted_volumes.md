# Encrypted volumes (deferred host work)

Encrypted volumes are not part of the current platform gate. The supported
guest storage boundary remains the checksummed tDisk v2 format loaded by
`ternary_host_runtime.h`; the current platform does not expose an encrypted
disk adapter or encryption command.

`encrypted_volume_host.h` and `encrypted_volume_openssl.cpp` are retained as
an explicitly deferred host-only design/reference while the gap in
[`KNOWN_GAPS.md`](../../KNOWN_GAPS.md) remains open. They are not included in
the current CMake graph, and the deleted test/tool trees are not replaced by
ad-hoc commands here.

Any future encryption work must preserve these boundaries:

- decrypt to a canonical tDisk v2 image before the runtime mounts it;
- authenticate the complete tDisk header and record area before exposure;
- use a reviewed provider and key lifecycle, with no fallback cipher;
- add one current CMake target and one focused conformance check before
  claiming support.

The proposed ternary-native format remains design space, not a guest ABI or a
release-image selection. No current loader accepts it.
