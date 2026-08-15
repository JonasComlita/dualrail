# Encrypted volumes (host boundary and ternary-native proposal)

Encrypted storage is intentionally split into two tracks.  The compatibility
track below is a host-side envelope around an existing image; it is not a new
guest filesystem and it is not accepted directly by the VM block-device
loader.  The ternary-native track is a versioned design proposal only.

## Host-compatible envelope (implemented, opt-in)

`encrypted_volume_host.h` owns a small, little-endian envelope for opaque image
bytes.  A canonical tDisk v2 file can be encrypted as a whole, then decrypted
to a normal `.tdisk` path before `attachBlockBackingFile` is called.  The
module does not parse, rewrite, or mount the tDisk payload, so sparse block
indices and the tDisk checksum remain the responsibility of the existing
image implementation.

The envelope header is 64 bytes:

| field | type | meaning |
|---|---|---|
| magic | 8 bytes | `TRITENC1` |
| version | u32 | `1` |
| algorithm | u32 | `1` = AES-256-GCM; test-only IDs are never production formats |
| header bytes | u32 | `64` |
| flags | u32 | reserved; must be zero |
| chunk bytes | u32 | 4 KiB..1 MiB, default 64 KiB |
| logical block words | u32 | optional metadata (`27` for current tDisk), not parsed |
| logical size | u64 | plaintext byte count |
| logical block count | u64 | derived when block metadata is present |
| chunk count | u64 | bounded by the 32-bit chunk nonce counter |
| nonce prefix | 8 bytes | fresh per volume/key pair |

Each chunk stores `chunk_index`, plaintext/ciphertext byte counts, a 16-byte
GCM tag, and ciphertext.  The authenticated data is the exact header plus the
chunk metadata.  A 12-byte nonce is `nonce_prefix || little_endian(index)`;
the caller must never reuse a prefix with the same key.  Sizes, counts,
metadata, truncation, trailing bytes, and provider/algorithm mismatches are
rejected before plaintext is returned.  Failed decryptions clear staged
plaintext and do not log or echo key material.

Production encryption uses only the optional `OpenSslAes256GcmProvider` in
`encrypted_volume_openssl.cpp`, which calls OpenSSL EVP AES-256-GCM and
`RAND_bytes`.  The base provider is fail-closed.  Current development hosts
have OpenSSL 3.6 under `C:/msys64/ucrt64` (`pkg-config --cflags --libs
openssl`), but the repository's CMake graph does not yet declare or link that
dependency.  Until a release build explicitly links OpenSSL (or an equivalent
OS AEAD provider) and defines `TRIT_ENCRYPTED_VOLUME_ENABLE_OPENSSL`, encrypted
volume creation must be treated as unavailable rather than falling back to a
home-grown cipher.

Focused validation is kept outside the current CMake/manifest wiring while
the dependency boundary is reviewed:

```powershell
C:/msys64/ucrt64/bin/g++.exe -std=c++17 -I. `
  -DTRIT_ENCRYPTED_VOLUME_ENABLE_OPENSSL `
  tests/test_encrypted_volume.cpp encrypted_volume_openssl.cpp `
  -lcrypto -o build/test_encrypted_volume.exe
build/test_encrypted_volume.exe
```

The test covers OpenSSL and a clearly marked deterministic mock (test-only):
round-trip, wrong-key rejection, ciphertext/header tamper, truncation,
trailing bytes, provider-generated prefixes, file helpers, and fail-closed
behavior when OpenSSL is not linked.  The mock is not a production provider.

## Ternary-native format (proposal; not selected)

The guest storage contract remains unencrypted tDisk v2.  A future native
format should not reuse the host envelope or the current `ternary_montgomery.h`
candidate as an implicit cipher.  Before selection, cryptographic review must
freeze a new format version and a provider backed by reviewed primitives.

The proposed `TENC-TN/1` layout is:

1. A fixed little-endian header with magic, version, profile/algorithm ID,
   logical block geometry, key-slot/KDF descriptor, random volume nonce, and a
   header hash.  Unknown versions and algorithms fail closed.
2. A domain-separated metadata authentication record (`"trit.tenc.tn1.meta"`)
   followed by one authenticated record per logical block.  The record AAD
   includes version, volume UUID, block index, plaintext length, and generation;
   no record may be accepted under a different domain.
3. Per-block nonces are derived from a unique volume nonce and the block index
   (with an explicit generation counter for rewrites).  Nonce reuse, counter
   wrap, and rollback are hard errors.  Tags are fixed-size and verified before
   a block is exposed to VFS/WAL recovery.
4. Key slots carry only wrapped data keys and KDF parameters; raw keys never
   appear in logs, manifests, diagnostics, or guest-visible metadata.  Key
   rotation writes a new version and re-authenticates all metadata before the
   old slot is retired.

This design is deliberately a review boundary: it has no guest mount hook,
no kernel ABI, and no release-image selection.  A cryptographic review must
decide the AEAD/KDF, nonce derivation, rollback policy, recovery ordering, and
constant-time implementation before a `TENC-TN/1` record can become a guest
storage contract.
