#pragma once
#ifndef TRIT_ENCRYPTED_VOLUME_HOST_H
#define TRIT_ENCRYPTED_VOLUME_HOST_H

// Host-side encrypted-volume envelope.
//
// This module deliberately sits outside the guest VFS and block-device
// contract.  It encrypts an opaque image (for example a canonical tDisk v2
// file) before the existing host runtime opens it.  The runtime still mounts
// only its normal authenticated tDisk format after an explicit decrypt step.
// Production encryption is provided by the optional OpenSSL adapter in
// encrypted_volume_openssl.cpp.  The base provider is fail-closed: this
// header never silently falls back to an unauthenticated construction.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sandbox::host::encrypted_volume {

using ByteVector = std::vector<std::uint8_t>;

inline constexpr std::array<char, 8> kEnvelopeMagic =
    {'T', 'R', 'I', 'T', 'E', 'N', 'C', '1'};
inline constexpr std::uint32_t kEnvelopeVersion = 1;
inline constexpr std::uint32_t kAlgorithmAes256Gcm = 1;
// Reserved for deterministic test providers.  It is not a production
// algorithm and must never be accepted by a guest or release image loader.
inline constexpr std::uint32_t kAlgorithmTestOnly = 0xffff0001U;
inline constexpr std::uint32_t kEnvelopeHeaderBytes = 64;
inline constexpr std::size_t kNoncePrefixBytes = 8;
inline constexpr std::size_t kAes256KeyBytes = 32;
inline constexpr std::size_t kAesGcmNonceBytes = 12;
inline constexpr std::size_t kAesGcmTagBytes = 16;
inline constexpr std::uint32_t kDefaultChunkBytes = 64U * 1024U;
inline constexpr std::uint32_t kMinimumChunkBytes = 4U * 1024U;
inline constexpr std::uint32_t kMaximumChunkBytes = 1U * 1024U * 1024U;
inline constexpr std::uint64_t kMaximumPlaintextBytes = 1ULL << 40;
// Keep hostile metadata from turning one envelope into an unbounded parser
// loop.  At the default chunk size this still permits a 1 TiB logical image.
inline constexpr std::uint32_t kMaximumChunkCount = 1U << 24;
inline constexpr std::uint32_t kMaximumLogicalBlockWords = 1U << 20;
inline constexpr std::uint32_t kEnvelopeChunkMetadataBytes = 12;
inline constexpr std::uint64_t kMaximumEnvelopeBytes =
    kMaximumPlaintextBytes + kEnvelopeHeaderBytes +
    static_cast<std::uint64_t>(kMaximumChunkCount) *
        (kEnvelopeChunkMetadataBytes + kAesGcmTagBytes);
inline constexpr std::uint64_t kTdiskV2Magic = 0x54524954535032ULL;
inline constexpr std::uint32_t kTdiskV2Version = 2;
inline constexpr std::uint32_t kTdiskBlockWords = 27;
inline constexpr std::uint32_t kTdiskV2HeaderBytes = 36;
inline constexpr std::uint32_t kTdiskV2RecordBytes =
    4U + kTdiskBlockWords * 8U;
inline constexpr std::uint32_t kMaximumTdiskRecords = 1U << 24;
inline constexpr std::uint64_t kMaximumTdiskBlockCount =
    (64ULL * 1024ULL * 1024ULL * 1024ULL +
     static_cast<std::uint64_t>(kTdiskBlockWords) * 8ULL - 1ULL) /
    (static_cast<std::uint64_t>(kTdiskBlockWords) * 8ULL);
// TernaryScalar<40> reserves the two uint64 sentinel values and accepts only
// positional values in [0, 3^40).  The upper bound is therefore 3^40 - 1.
inline constexpr std::uint64_t kMaximumT40Raw = 12157665459056928800ULL;

struct EncryptOptions {
    std::uint32_t chunk_bytes = kDefaultChunkBytes;
    // Optional metadata for an opaque sparse image.  A non-zero value records
    // the words per logical block (27 for the current host tDisk), but this
    // module does not parse or mount the image.
    std::uint32_t logical_block_words = 0;
    std::array<std::uint8_t, kNoncePrefixBytes> nonce_prefix{};
    bool nonce_prefix_provided = false;
};

class AuthenticatedEncryptionProvider {
public:
    virtual ~AuthenticatedEncryptionProvider() = default;

    [[nodiscard]] virtual std::uint32_t algorithmId() const = 0;
    [[nodiscard]] virtual std::size_t keyBytes() const = 0;
    [[nodiscard]] virtual std::size_t nonceBytes() const = 0;
    [[nodiscard]] virtual std::size_t tagBytes() const = 0;

    // Implementations must authenticate aad and return ciphertext and tag
    // separately.  On failure, output vectors are cleared and error text must
    // not include key material.
    [[nodiscard]] virtual bool encrypt(
        const ByteVector& key,
        const ByteVector& nonce,
        const ByteVector& aad,
        const ByteVector& plaintext,
        ByteVector& ciphertext,
        ByteVector& tag,
        std::string* error = nullptr) const = 0;

    [[nodiscard]] virtual bool decrypt(
        const ByteVector& key,
        const ByteVector& nonce,
        const ByteVector& aad,
        const ByteVector& ciphertext,
        const ByteVector& tag,
        ByteVector& plaintext,
        std::string* error = nullptr) const = 0;

    // The production adapter obtains the prefix from an OS/library CSPRNG.
    // A caller-provided prefix is accepted only by the explicitly test-only
    // algorithm; production TRITENC1 has no durable nonce-reuse registry.
    [[nodiscard]] virtual bool randomBytes(
        std::uint8_t* destination,
        std::size_t size,
        std::string* error = nullptr) const = 0;
};

// Explicit no-provider behavior.  This is the default when the optional
// OpenSSL source is not linked and is intentionally unusable for encryption.
class FailClosedProvider final : public AuthenticatedEncryptionProvider {
public:
    [[nodiscard]] std::uint32_t algorithmId() const override { return 0; }
    [[nodiscard]] std::size_t keyBytes() const override { return 0; }
    [[nodiscard]] std::size_t nonceBytes() const override { return 0; }
    [[nodiscard]] std::size_t tagBytes() const override { return 0; }

    [[nodiscard]] bool encrypt(const ByteVector&, const ByteVector&,
                               const ByteVector&, const ByteVector&,
                               ByteVector& ciphertext, ByteVector& tag,
                               std::string* error = nullptr) const override {
        ciphertext.clear();
        tag.clear();
        setError(error, "authenticated-encryption provider unavailable");
        return false;
    }

    [[nodiscard]] bool decrypt(const ByteVector&, const ByteVector&,
                               const ByteVector&, const ByteVector&,
                               const ByteVector&, ByteVector& plaintext,
                               std::string* error = nullptr) const override {
        plaintext.clear();
        setError(error, "authenticated-encryption provider unavailable");
        return false;
    }

    [[nodiscard]] bool randomBytes(std::uint8_t*, std::size_t,
                                   std::string* error = nullptr) const override {
        setError(error, "authenticated-encryption provider unavailable");
        return false;
    }

private:
    static void setError(std::string* error, const char* text) {
        if (error) *error = text;
    }
};

// Optional adapter implemented in encrypted_volume_openssl.cpp.  Without
// TRIT_ENCRYPTED_VOLUME_ENABLE_OPENSSL that translation unit remains a
// fail-closed provider, so portable builds do not acquire an accidental crypto
// dependency.  When enabled it is AES-256-GCM from OpenSSL EVP.
class OpenSslAes256GcmProvider final : public AuthenticatedEncryptionProvider {
public:
    [[nodiscard]] std::uint32_t algorithmId() const override {
        return kAlgorithmAes256Gcm;
    }
    [[nodiscard]] std::size_t keyBytes() const override {
        return kAes256KeyBytes;
    }
    [[nodiscard]] std::size_t nonceBytes() const override {
        return kAesGcmNonceBytes;
    }
    [[nodiscard]] std::size_t tagBytes() const override {
        return kAesGcmTagBytes;
    }

    [[nodiscard]] bool encrypt(const ByteVector& key,
                               const ByteVector& nonce,
                               const ByteVector& aad,
                               const ByteVector& plaintext,
                               ByteVector& ciphertext,
                               ByteVector& tag,
                               std::string* error = nullptr) const override;

    [[nodiscard]] bool decrypt(const ByteVector& key,
                               const ByteVector& nonce,
                               const ByteVector& aad,
                               const ByteVector& ciphertext,
                               const ByteVector& tag,
                               ByteVector& plaintext,
                               std::string* error = nullptr) const override;

    [[nodiscard]] bool randomBytes(std::uint8_t* destination,
                                   std::size_t size,
                                   std::string* error = nullptr) const override;
};

[[nodiscard]] inline std::shared_ptr<const AuthenticatedEncryptionProvider>
failClosedProvider() {
    static const std::shared_ptr<const AuthenticatedEncryptionProvider> provider =
        std::make_shared<FailClosedProvider>();
    return provider;
}

namespace detail {

inline void clearError(std::string* error) {
    if (error) error->clear();
}

inline void setError(std::string* error, const std::string& text) {
    if (error) *error = text;
}

inline void clearSensitive(ByteVector& value) {
    // Best effort.  The provider owns the actual cryptographic operation; the
    // envelope only clears temporary key/nonce copies before returning.
    volatile std::uint8_t* bytes = value.data();
    for (std::size_t index = 0; index < value.size(); ++index) bytes[index] = 0;
    value.clear();
    value.shrink_to_fit();
}

template <std::size_t Size>
inline void clearSensitive(std::array<std::uint8_t, Size>& value) {
    volatile std::uint8_t* bytes = value.data();
    for (std::size_t index = 0; index < value.size(); ++index) bytes[index] = 0;
}

template <std::size_t Size>
[[nodiscard]] inline bool allZero(
    const std::array<std::uint8_t, Size>& value) {
    for (std::uint8_t byte : value) {
        if (byte != 0) return false;
    }
    return true;
}

inline int hexDigit(std::uint8_t value) {
    if (value >= static_cast<std::uint8_t>('0') &&
        value <= static_cast<std::uint8_t>('9')) {
        return static_cast<int>(value - static_cast<std::uint8_t>('0'));
    }
    if (value >= static_cast<std::uint8_t>('a') &&
        value <= static_cast<std::uint8_t>('f')) {
        return 10 + static_cast<int>(value - static_cast<std::uint8_t>('a'));
    }
    if (value >= static_cast<std::uint8_t>('A') &&
        value <= static_cast<std::uint8_t>('F')) {
        return 10 + static_cast<int>(value - static_cast<std::uint8_t>('A'));
    }
    return -1;
}

inline bool decodeHexKey(const ByteVector& encoded, ByteVector& key) {
    clearSensitive(key);
    if (encoded.size() != kAes256KeyBytes * 2U) return false;
    try {
        key.resize(kAes256KeyBytes);
    } catch (const std::bad_alloc&) {
        clearSensitive(key);
        return false;
    }
    for (std::size_t index = 0; index < key.size(); ++index) {
        const int high = hexDigit(encoded[index * 2U]);
        const int low = hexDigit(encoded[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            clearSensitive(key);
            return false;
        }
        key[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

inline void appendLe32(ByteVector& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

inline void appendLe64(ByteVector& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffULL));
    }
}

inline bool readLe32(const ByteVector& input, std::size_t& offset,
                    std::uint32_t& value) {
    if (offset > input.size() || input.size() - offset < 4) return false;
    value = static_cast<std::uint32_t>(input[offset]) |
            (static_cast<std::uint32_t>(input[offset + 1]) << 8) |
            (static_cast<std::uint32_t>(input[offset + 2]) << 16) |
            (static_cast<std::uint32_t>(input[offset + 3]) << 24);
    offset += 4;
    return true;
}

inline bool readLe64(const ByteVector& input, std::size_t& offset,
                    std::uint64_t& value) {
    if (offset > input.size() || input.size() - offset < 8) return false;
    value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(input[offset++]) << shift;
    }
    return true;
}

inline bool checkedAdd(std::size_t left, std::size_t right,
                       std::size_t& result) {
    if (right > std::numeric_limits<std::size_t>::max() - left) return false;
    result = left + right;
    return true;
}

inline bool checkedMul(std::size_t left, std::size_t right,
                       std::size_t& result) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left)
        return false;
    result = left * right;
    return true;
}

inline bool checkedCeilDiv(std::uint64_t value,
                           std::uint64_t divisor,
                           std::uint64_t& result) {
    if (divisor == 0) return false;
    result = value / divisor;
    if (value % divisor != 0) {
        if (result == std::numeric_limits<std::uint64_t>::max()) return false;
        ++result;
    }
    return true;
}

inline bool supportedAlgorithm(std::uint32_t algorithm) {
    return algorithm == kAlgorithmAes256Gcm || algorithm == kAlgorithmTestOnly;
}

inline bool checkedEnvelopeSize(std::uint64_t logical_size,
                                std::uint64_t chunk_count,
                                std::size_t& result) {
    if (logical_size > kMaximumPlaintextBytes ||
        chunk_count > kMaximumChunkCount) {
        return false;
    }
    const std::uint64_t record_overhead =
        static_cast<std::uint64_t>(kEnvelopeChunkMetadataBytes) +
        static_cast<std::uint64_t>(kAesGcmTagBytes);
    if (chunk_count >
        (std::numeric_limits<std::uint64_t>::max() / record_overhead)) {
        return false;
    }
    const std::uint64_t total_records = chunk_count * record_overhead;
    if (total_records > std::numeric_limits<std::uint64_t>::max() -
                           kEnvelopeHeaderBytes ||
        logical_size > std::numeric_limits<std::uint64_t>::max() -
                           kEnvelopeHeaderBytes - total_records) {
        return false;
    }
    const std::uint64_t total =
        static_cast<std::uint64_t>(kEnvelopeHeaderBytes) + logical_size +
        total_records;
    if (total > kMaximumEnvelopeBytes ||
        total > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    result = static_cast<std::size_t>(total);
    return true;
}

inline bool expectedChunkBytes(std::uint64_t logical_size,
                               std::uint32_t chunk_bytes,
                               std::uint64_t chunk_count,
                               std::uint32_t chunk_index,
                               std::uint32_t& expected) {
    if (chunk_index >= chunk_count || chunk_bytes == 0) return false;
    const std::uint64_t offset =
        static_cast<std::uint64_t>(chunk_index) * chunk_bytes;
    if (offset >= logical_size) return false;
    const std::uint64_t remaining = logical_size - offset;
    expected = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(remaining, chunk_bytes));
    return expected != 0;
}

inline void makeNonce(const std::array<std::uint8_t, kNoncePrefixBytes>& prefix,
                      std::uint32_t chunk_index, ByteVector& nonce) {
    nonce.assign(prefix.begin(), prefix.end());
    appendLe32(nonce, chunk_index);
}

inline void appendHeader(ByteVector& output,
                         std::uint32_t algorithm,
                         std::uint32_t chunk_bytes,
                         std::uint32_t logical_block_words,
                         std::uint64_t logical_size,
                         std::uint64_t logical_block_count,
                         std::uint64_t chunk_count,
                         const std::array<std::uint8_t, kNoncePrefixBytes>& prefix) {
    output.insert(output.end(), kEnvelopeMagic.begin(), kEnvelopeMagic.end());
    appendLe32(output, kEnvelopeVersion);
    appendLe32(output, algorithm);
    appendLe32(output, kEnvelopeHeaderBytes);
    appendLe32(output, 0); // flags; reserved and authenticated
    appendLe32(output, chunk_bytes);
    appendLe32(output, logical_block_words);
    appendLe64(output, logical_size);
    appendLe64(output, logical_block_count);
    appendLe64(output, chunk_count);
    output.insert(output.end(), prefix.begin(), prefix.end());
}

inline bool parseHeader(const ByteVector& envelope,
                        std::uint32_t& algorithm,
                        std::uint32_t& chunk_bytes,
                        std::uint32_t& logical_block_words,
                        std::uint64_t& logical_size,
                        std::uint64_t& logical_block_count,
                        std::uint64_t& chunk_count,
                        std::array<std::uint8_t, kNoncePrefixBytes>& prefix,
                        std::string* error) {
    prefix.fill(0);
    if (envelope.size() < kEnvelopeHeaderBytes ||
        envelope.size() > kMaximumEnvelopeBytes) {
        setError(error, "encrypted-volume envelope header is truncated");
        return false;
    }
    if (!std::equal(kEnvelopeMagic.begin(), kEnvelopeMagic.end(),
                    reinterpret_cast<const char*>(envelope.data()))) {
        setError(error, "encrypted-volume envelope magic is invalid");
        return false;
    }
    std::size_t offset = kEnvelopeMagic.size();
    std::uint32_t version = 0;
    std::uint32_t header_bytes = 0;
    std::uint32_t flags = 0;
    if (!readLe32(envelope, offset, version) ||
        !readLe32(envelope, offset, algorithm) ||
        !readLe32(envelope, offset, header_bytes) ||
        !readLe32(envelope, offset, flags) ||
        !readLe32(envelope, offset, chunk_bytes) ||
        !readLe32(envelope, offset, logical_block_words) ||
        !readLe64(envelope, offset, logical_size) ||
        !readLe64(envelope, offset, logical_block_count) ||
        !readLe64(envelope, offset, chunk_count)) {
        setError(error, "encrypted-volume envelope header is truncated");
        return false;
    }
    if (version != kEnvelopeVersion || header_bytes != kEnvelopeHeaderBytes ||
        flags != 0 || !supportedAlgorithm(algorithm) ||
        envelope.size() - offset < prefix.size()) {
        setError(error, "unsupported encrypted-volume envelope header");
        return false;
    }
    std::copy(envelope.begin() + static_cast<std::ptrdiff_t>(offset),
              envelope.begin() + static_cast<std::ptrdiff_t>(offset + prefix.size()),
              prefix.begin());
    if (chunk_bytes < kMinimumChunkBytes || chunk_bytes > kMaximumChunkBytes) {
        setError(error, "encrypted-volume chunk size is invalid");
        return false;
    }
    if (logical_size > kMaximumPlaintextBytes ||
        chunk_count > kMaximumChunkCount ||
        logical_block_words > kMaximumLogicalBlockWords ||
        allZero(prefix)) {
        setError(error, "encrypted-volume logical size is unreasonable");
        return false;
    }
    // A header-only envelope has no GCM record through which the header can be
    // authenticated.  Canonical tDisk v2 images always contain their 36-byte
    // header, so rejecting a zero-byte opaque payload preserves the selected
    // production policy without inventing a second envelope version.
    if (logical_size == 0) {
        setError(error, "encrypted-volume empty payload is not authenticated");
        return false;
    }
    std::uint64_t expected_chunks = 0;
    if (!checkedCeilDiv(logical_size, chunk_bytes, expected_chunks)) {
        setError(error, "encrypted-volume chunk count is invalid");
        return false;
    }
    if (chunk_count != expected_chunks) {
        setError(error, "encrypted-volume chunk count is inconsistent");
        return false;
    }
    if (logical_block_words != 0) {
        constexpr std::uint64_t kBytesPerTritWord = 8;
        const std::uint64_t block_bytes =
            static_cast<std::uint64_t>(logical_block_words) * kBytesPerTritWord;
        std::uint64_t expected_blocks = 0;
        if (block_bytes == 0 ||
            !checkedCeilDiv(logical_size, block_bytes, expected_blocks) ||
            expected_blocks != logical_block_count) {
            setError(error, "encrypted-volume block metadata is inconsistent");
            return false;
        }
    } else if (logical_block_count != 0) {
        setError(error, "encrypted-volume block metadata is inconsistent");
        return false;
    }
    std::size_t expected_envelope_size = 0;
    if (!checkedEnvelopeSize(logical_size, chunk_count,
                             expected_envelope_size) ||
        expected_envelope_size != envelope.size()) {
        setError(error, "encrypted-volume envelope size is inconsistent");
        return false;
    }
    return true;
}

inline bool buildAad(const ByteVector& header,
                     std::uint32_t chunk_index,
                     std::uint32_t plaintext_bytes,
                     std::uint32_t ciphertext_bytes,
                     ByteVector& aad,
                     std::string* error = nullptr) {
    aad.clear();
    if (header.size() != kEnvelopeHeaderBytes || chunk_index >= kMaximumChunkCount ||
        plaintext_bytes == 0 || plaintext_bytes > kMaximumChunkBytes ||
        ciphertext_bytes != plaintext_bytes) {
        setError(error, "encrypted-volume AAD metadata is invalid");
        return false;
    }
    aad = header;
    appendLe32(aad, chunk_index);
    appendLe32(aad, plaintext_bytes);
    appendLe32(aad, ciphertext_bytes);
    if (aad.size() != kEnvelopeHeaderBytes + kEnvelopeChunkMetadataBytes) {
        aad.clear();
        setError(error, "encrypted-volume AAD is invalid");
        return false;
    }
    return true;
}

inline void fnv1aUpdate(std::uint64_t& hash,
                        const std::uint8_t* bytes,
                        std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
}

inline bool isCanonicalTdiskV2(const ByteVector& bytes,
                               std::string* error = nullptr) {
    if (bytes.size() < kTdiskV2HeaderBytes) {
        setError(error, "encrypted-volume tDisk header is truncated");
        return false;
    }
    std::size_t offset = 0;
    std::uint64_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t block_words = 0;
    std::uint64_t generation = 0;
    std::uint64_t expected_checksum = 0;
    std::uint32_t raw_record_count = 0;
    if (!readLe64(bytes, offset, magic) || !readLe32(bytes, offset, version) ||
        !readLe32(bytes, offset, block_words) ||
        !readLe64(bytes, offset, generation) ||
        !readLe64(bytes, offset, expected_checksum) ||
        !readLe32(bytes, offset, raw_record_count)) {
        setError(error, "encrypted-volume tDisk header is truncated");
        return false;
    }
    if (magic != kTdiskV2Magic || version != kTdiskV2Version ||
        block_words != kTdiskBlockWords || generation == 0 ||
        raw_record_count > static_cast<std::uint32_t>(
                               std::numeric_limits<std::int32_t>::max()) ||
        raw_record_count > kMaximumTdiskRecords) {
        setError(error, "encrypted-volume tDisk header is not canonical v2");
        return false;
    }

    const std::size_t record_count = static_cast<std::size_t>(raw_record_count);
    std::size_t records_bytes = 0;
    std::size_t expected_size = 0;
    if (!checkedMul(record_count, kTdiskV2RecordBytes, records_bytes) ||
        !checkedAdd(kTdiskV2HeaderBytes, records_bytes, expected_size) ||
        expected_size != bytes.size()) {
        setError(error, "encrypted-volume tDisk record area is invalid");
        return false;
    }

    std::uint64_t actual_checksum = 1469598103934665603ULL;
    fnv1aUpdate(actual_checksum, bytes.data() + kTdiskV2HeaderBytes,
                records_bytes);
    if (actual_checksum != expected_checksum) {
        setError(error, "encrypted-volume tDisk checksum validation failed");
        return false;
    }

    bool have_previous_index = false;
    std::uint32_t previous_index = 0;
    for (std::size_t record = 0; record < record_count; ++record) {
        std::uint32_t raw_index = 0;
        if (!readLe32(bytes, offset, raw_index) ||
            raw_index > static_cast<std::uint32_t>(
                             std::numeric_limits<std::int32_t>::max()) ||
            static_cast<std::uint64_t>(raw_index) >= kMaximumTdiskBlockCount ||
            (have_previous_index && raw_index <= previous_index)) {
            setError(error, "encrypted-volume tDisk block index is not canonical");
            return false;
        }
        bool nonzero = false;
        for (std::uint32_t word = 0; word < kTdiskBlockWords; ++word) {
            std::uint64_t raw_word = 0;
            if (!readLe64(bytes, offset, raw_word) || raw_word > kMaximumT40Raw) {
                setError(error, "encrypted-volume tDisk word is invalid");
                return false;
            }
            nonzero = nonzero || raw_word != 0;
        }
        if (!nonzero) {
            setError(error, "encrypted-volume tDisk contains an empty record");
            return false;
        }
        previous_index = raw_index;
        have_previous_index = true;
    }
    if (offset != bytes.size()) {
        setError(error, "encrypted-volume tDisk record area is invalid");
        return false;
    }
    return true;
}

} // namespace detail

[[nodiscard]] inline bool isCanonicalTdiskV2(const ByteVector& bytes,
                                             std::string* error = nullptr) {
    detail::clearError(error);
    return detail::isCanonicalTdiskV2(bytes, error);
}

[[nodiscard]] inline bool providerSupportsAesGcm(
    const AuthenticatedEncryptionProvider& provider) {
    return detail::supportedAlgorithm(provider.algorithmId()) &&
           provider.keyBytes() == kAes256KeyBytes &&
           provider.nonceBytes() == kAesGcmNonceBytes &&
           provider.tagBytes() == kAesGcmTagBytes;
}

[[nodiscard]] inline bool encryptBytes(
    const ByteVector& plaintext,
    const ByteVector& key,
    const AuthenticatedEncryptionProvider& provider,
    ByteVector& envelope,
    const EncryptOptions& options = {},
    std::string* error = nullptr) {
    detail::clearSensitive(envelope);
    detail::clearError(error);
    if (!providerSupportsAesGcm(provider)) {
        detail::setError(error, "authenticated-encryption provider is not configured");
        return false;
    }
    if (key.size() != kAes256KeyBytes) {
        detail::setError(error, "encrypted-volume key length is invalid");
        return false;
    }
    if (plaintext.size() > kMaximumPlaintextBytes ||
        options.chunk_bytes < kMinimumChunkBytes ||
        options.chunk_bytes > kMaximumChunkBytes ||
        options.logical_block_words > kMaximumLogicalBlockWords) {
        detail::setError(error, "encrypted-volume plaintext or chunk size is invalid");
        return false;
    }
    if (plaintext.empty()) {
        detail::setError(error, "encrypted-volume empty payload is not authenticated");
        return false;
    }
    std::uint64_t chunk_count = 0;
    if (!detail::checkedCeilDiv(static_cast<std::uint64_t>(plaintext.size()),
                                options.chunk_bytes, chunk_count) ||
        chunk_count > kMaximumChunkCount) {
        detail::setError(error, "encrypted-volume has too many chunks");
        return false;
    }
    std::size_t expected_envelope_size = 0;
    if (!detail::checkedEnvelopeSize(static_cast<std::uint64_t>(plaintext.size()),
                                     chunk_count, expected_envelope_size)) {
        detail::setError(error, "encrypted-volume envelope size is invalid");
        return false;
    }

    if (provider.algorithmId() == kAlgorithmAes256Gcm &&
        options.nonce_prefix_provided) {
        // TRITENC1 has no durable nonce-reuse registry. Production AES-GCM
        // envelopes therefore always obtain the prefix from the provider's
        // CSPRNG; deterministic prefixes remain available only to the
        // explicitly non-production test provider.
        detail::setError(error,
                         "production encrypted-volume nonce prefixes are provider-generated");
        return false;
    }
    std::array<std::uint8_t, kNoncePrefixBytes> prefix = options.nonce_prefix;
    if (!options.nonce_prefix_provided) {
        if (!provider.randomBytes(prefix.data(), prefix.size(), nullptr)) {
            detail::clearSensitive(prefix);
            detail::setError(error, "encrypted-volume nonce generation failed");
            return false;
        }
    }
    if (detail::allZero(prefix)) {
        detail::clearSensitive(prefix);
        detail::setError(error, "encrypted-volume nonce prefix is invalid");
        return false;
    }
    std::uint64_t logical_block_count = 0;
    if (options.logical_block_words != 0 && !plaintext.empty()) {
        const std::uint64_t block_bytes =
            static_cast<std::uint64_t>(options.logical_block_words) * 8ULL;
        if (!detail::checkedCeilDiv(static_cast<std::uint64_t>(plaintext.size()),
                                    block_bytes, logical_block_count)) {
            detail::clearSensitive(prefix);
            detail::setError(error, "encrypted-volume block metadata is invalid");
            return false;
        }
    }

    ByteVector header;
    header.reserve(kEnvelopeHeaderBytes);
    detail::appendHeader(header, provider.algorithmId(), options.chunk_bytes,
                         options.logical_block_words,
                         static_cast<std::uint64_t>(plaintext.size()),
                         logical_block_count, chunk_count, prefix);
    if (header.size() != kEnvelopeHeaderBytes) {
        detail::clearSensitive(prefix);
        detail::setError(error, "encrypted-volume envelope header is invalid");
        return false;
    }
    envelope = header;
    envelope.reserve(expected_envelope_size);
    ByteVector nonce;
    ByteVector aad;
    ByteVector ciphertext;
    ByteVector tag;
    for (std::uint64_t chunk = 0; chunk < chunk_count; ++chunk) {
        const std::size_t offset =
            static_cast<std::size_t>(chunk) * options.chunk_bytes;
        const std::size_t remaining = plaintext.size() - offset;
        const std::size_t size =
            std::min<std::size_t>(remaining, options.chunk_bytes);
        if (size == 0 || size > std::numeric_limits<std::uint32_t>::max()) {
            detail::setError(error, "encrypted-volume chunk is too large");
            detail::clearSensitive(nonce);
            detail::clearSensitive(aad);
            detail::clearSensitive(ciphertext);
            detail::clearSensitive(tag);
            detail::clearSensitive(prefix);
            detail::clearSensitive(envelope);
            return false;
        }
        const auto chunk_index = static_cast<std::uint32_t>(chunk);
        detail::makeNonce(prefix, chunk_index, nonce);
        const auto* begin = plaintext.data() + static_cast<std::ptrdiff_t>(offset);
        ByteVector chunk_plain(begin, begin + static_cast<std::ptrdiff_t>(size));
        if (!detail::buildAad(header, chunk_index,
                              static_cast<std::uint32_t>(size),
                              static_cast<std::uint32_t>(size), aad, error)) {
            detail::clearSensitive(chunk_plain);
            detail::clearSensitive(nonce);
            detail::clearSensitive(aad);
            detail::clearSensitive(ciphertext);
            detail::clearSensitive(tag);
            detail::clearSensitive(prefix);
            detail::clearSensitive(envelope);
            return false;
        }
        ciphertext.clear();
        tag.clear();
        const bool encrypted = provider.encrypt(
            key, nonce, aad, chunk_plain, ciphertext, tag, nullptr);
        if (!encrypted || ciphertext.size() != size ||
            tag.size() != kAesGcmTagBytes) {
            detail::clearSensitive(chunk_plain);
            detail::clearSensitive(nonce);
            detail::clearSensitive(aad);
            detail::clearSensitive(ciphertext);
            detail::clearSensitive(tag);
            detail::clearSensitive(prefix);
            detail::clearSensitive(envelope);
            detail::setError(error, "encrypted-volume chunk encryption failed");
            return false;
        }
        detail::appendLe32(envelope, chunk_index);
        detail::appendLe32(envelope, static_cast<std::uint32_t>(size));
        detail::appendLe32(envelope, static_cast<std::uint32_t>(ciphertext.size()));
        envelope.insert(envelope.end(), tag.begin(), tag.end());
        envelope.insert(envelope.end(), ciphertext.begin(), ciphertext.end());
        detail::clearSensitive(chunk_plain);
        detail::clearSensitive(ciphertext);
        detail::clearSensitive(tag);
    }
    detail::clearSensitive(nonce);
    detail::clearSensitive(aad);
    detail::clearSensitive(prefix);
    if (envelope.size() != expected_envelope_size) {
        detail::clearSensitive(envelope);
        detail::setError(error, "encrypted-volume envelope size is invalid");
        return false;
    }
    return true;
}

[[nodiscard]] inline bool decryptBytes(
    const ByteVector& envelope,
    const ByteVector& key,
    const AuthenticatedEncryptionProvider& provider,
    ByteVector& plaintext,
    std::string* error = nullptr) {
    detail::clearSensitive(plaintext);
    detail::clearError(error);
    std::uint32_t algorithm = 0;
    std::uint32_t chunk_bytes = 0;
    std::uint32_t logical_block_words = 0;
    std::uint64_t logical_size = 0;
    std::uint64_t logical_block_count = 0;
    std::uint64_t chunk_count = 0;
    std::array<std::uint8_t, kNoncePrefixBytes> prefix{};
    if (!detail::parseHeader(envelope, algorithm, chunk_bytes,
                             logical_block_words, logical_size,
                             logical_block_count, chunk_count, prefix, error)) {
        return false;
    }
    if (algorithm != provider.algorithmId() || !providerSupportsAesGcm(provider) ||
        key.size() != kAes256KeyBytes) {
        detail::clearSensitive(prefix);
        detail::setError(error, "encrypted-volume provider or key is incompatible");
        return false;
    }
    if (logical_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        detail::clearSensitive(prefix);
        detail::setError(error, "encrypted-volume plaintext is too large for host");
        return false;
    }
    ByteVector header(envelope.begin(),
                      envelope.begin() + static_cast<std::ptrdiff_t>(kEnvelopeHeaderBytes));
    ByteVector staged;
    std::size_t offset = kEnvelopeHeaderBytes;
    ByteVector nonce;
    ByteVector aad;
    ByteVector ciphertext;
    ByteVector tag;
    ByteVector chunk_plain;
    try {
        staged.reserve(static_cast<std::size_t>(logical_size));
        for (std::uint64_t expected_chunk = 0; expected_chunk < chunk_count;
             ++expected_chunk) {
            std::uint32_t chunk_index = 0;
            std::uint32_t plaintext_bytes = 0;
            std::uint32_t ciphertext_bytes = 0;
            std::uint32_t expected_plaintext_bytes = 0;
            if (!detail::readLe32(envelope, offset, chunk_index) ||
                !detail::readLe32(envelope, offset, plaintext_bytes) ||
                !detail::readLe32(envelope, offset, ciphertext_bytes) ||
                chunk_index != expected_chunk ||
                !detail::expectedChunkBytes(logical_size, chunk_bytes,
                                            chunk_count, chunk_index,
                                            expected_plaintext_bytes) ||
                plaintext_bytes != expected_plaintext_bytes ||
                ciphertext_bytes != plaintext_bytes) {
                detail::clearSensitive(staged);
                detail::clearSensitive(chunk_plain);
                detail::clearSensitive(nonce);
                detail::clearSensitive(aad);
                detail::clearSensitive(ciphertext);
                detail::clearSensitive(tag);
                detail::clearSensitive(prefix);
                detail::setError(error, "encrypted-volume chunk metadata is invalid");
                return false;
            }
            std::size_t record_bytes = 0;
            if (!detail::checkedAdd(kAesGcmTagBytes, ciphertext_bytes,
                                    record_bytes) ||
                offset > envelope.size() || envelope.size() - offset < record_bytes) {
                detail::clearSensitive(staged);
                detail::clearSensitive(chunk_plain);
                detail::clearSensitive(nonce);
                detail::clearSensitive(aad);
                detail::clearSensitive(ciphertext);
                detail::clearSensitive(tag);
                detail::clearSensitive(prefix);
                detail::setError(error, "encrypted-volume chunk is truncated");
                return false;
            }
            detail::clearSensitive(tag);
            tag.assign(envelope.begin() + static_cast<std::ptrdiff_t>(offset),
                       envelope.begin() + static_cast<std::ptrdiff_t>(
                           offset + kAesGcmTagBytes));
            offset += kAesGcmTagBytes;
            detail::clearSensitive(ciphertext);
            ciphertext.assign(
                envelope.begin() + static_cast<std::ptrdiff_t>(offset),
                envelope.begin() + static_cast<std::ptrdiff_t>(
                    offset + ciphertext_bytes));
            offset += ciphertext_bytes;
            detail::makeNonce(prefix, chunk_index, nonce);
            if (!detail::buildAad(header, chunk_index, plaintext_bytes,
                                  ciphertext_bytes, aad, error)) {
                detail::clearSensitive(staged);
                detail::clearSensitive(chunk_plain);
                detail::clearSensitive(nonce);
                detail::clearSensitive(aad);
                detail::clearSensitive(ciphertext);
                detail::clearSensitive(tag);
                detail::clearSensitive(prefix);
                return false;
            }
            detail::clearSensitive(chunk_plain);
            const bool decrypted = provider.decrypt(
                key, nonce, aad, ciphertext, tag, chunk_plain, nullptr);
            if (!decrypted || chunk_plain.size() != plaintext_bytes) {
                detail::clearSensitive(chunk_plain);
                detail::clearSensitive(staged);
                detail::clearSensitive(nonce);
                detail::clearSensitive(aad);
                detail::clearSensitive(ciphertext);
                detail::clearSensitive(tag);
                detail::clearSensitive(prefix);
                detail::setError(error, "encrypted-volume authentication failed");
                return false;
            }
            staged.insert(staged.end(), chunk_plain.begin(), chunk_plain.end());
            detail::clearSensitive(chunk_plain);
        }
    } catch (const std::bad_alloc&) {
        detail::clearSensitive(chunk_plain);
        detail::clearSensitive(staged);
        detail::clearSensitive(nonce);
        detail::clearSensitive(aad);
        detail::clearSensitive(ciphertext);
        detail::clearSensitive(tag);
        detail::clearSensitive(prefix);
        detail::setError(error, "encrypted-volume plaintext allocation failed");
        return false;
    }
    if (offset != envelope.size() || staged.size() != logical_size) {
        detail::clearSensitive(staged);
        detail::clearSensitive(chunk_plain);
        detail::clearSensitive(nonce);
        detail::clearSensitive(aad);
        detail::clearSensitive(ciphertext);
        detail::clearSensitive(tag);
        detail::clearSensitive(prefix);
        detail::setError(error, "encrypted-volume envelope has trailing or missing data");
        return false;
    }
    plaintext.swap(staged);
    detail::clearSensitive(staged);
    detail::clearSensitive(chunk_plain);
    detail::clearSensitive(nonce);
    detail::clearSensitive(aad);
    detail::clearSensitive(ciphertext);
    detail::clearSensitive(tag);
    detail::clearSensitive(prefix);
    return true;
}

namespace detail {

inline std::filesystem::path temporaryPathFor(
    const std::filesystem::path& target) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto clock = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::uint64_t serial = sequence.fetch_add(1, std::memory_order_relaxed);
    return std::filesystem::path(
        target.string() + ".tmp." + std::to_string(clock) + "." +
        std::to_string(serial));
}

class TemporaryFileCleanup final {
public:
    explicit TemporaryFileCleanup(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~TemporaryFileCleanup() {
        if (created_ && !promoted_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    void markCreated() { created_ = true; }
    void markPromoted() { promoted_ = true; }

private:
    std::filesystem::path path_;
    bool created_ = false;
    bool promoted_ = false;
};

inline bool syncFileToStableStorage(const std::filesystem::path& path,
                                    std::string* error) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        setError(error, "failed to flush encrypted-volume temporary output");
        return false;
    }
    const BOOL flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!flushed) {
        setError(error, "failed to flush encrypted-volume temporary output");
        return false;
    }
    return true;
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        setError(error, "failed to flush encrypted-volume temporary output");
        return false;
    }
    const int synced = ::fsync(descriptor);
    const int closed = ::close(descriptor);
    if (synced != 0 || closed != 0) {
        setError(error, "failed to flush encrypted-volume temporary output");
        return false;
    }
    return true;
#endif
}

// Create the unpublished replacement with exclusive creation and owner-only
// permissions.  This matters for the plaintext tDisk staging path: a
// generated name is not a security boundary if another process can pre-create
// it or if the file inherits broad permissions before promotion.
inline bool createRestrictedTemporaryFile(const std::filesystem::path& path,
                                           const ByteVector& bytes,
                                           bool& created,
                                           std::string* error) {
    created = false;
#if defined(_WIN32)
    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;OW)", SDDL_REVISION_1,
            &security_descriptor, nullptr)) {
        setError(error, "failed to create restricted encrypted-volume output");
        return false;
    }
    security_attributes.lpSecurityDescriptor = security_descriptor;
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, &security_attributes,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(security_descriptor);
    if (handle == INVALID_HANDLE_VALUE) {
        setError(error, "failed to create encrypted-volume temporary output");
        return false;
    }
    created = true;

    bool ok = true;
    std::size_t offset = 0;
    while (ok && offset < bytes.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, static_cast<std::size_t>(1U << 20)));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, request, &written, nullptr) ||
            written != request) {
            ok = false;
        } else {
            offset += written;
        }
    }
    if (ok && !FlushFileBuffers(handle)) ok = false;
    if (!CloseHandle(handle)) ok = false;
    if (!ok) {
        setError(error, "failed to write encrypted-volume temporary output");
        return false;
    }
    return true;
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        setError(error, "failed to create encrypted-volume temporary output");
        return false;
    }
    created = true;
    bool ok = ::fchmod(descriptor, S_IRUSR | S_IWUSR) == 0;
    std::size_t offset = 0;
    while (ok && offset < bytes.size()) {
        const std::size_t request = std::min<std::size_t>(
            bytes.size() - offset,
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t written = ::write(descriptor, bytes.data() + offset, request);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            ok = false;
        } else {
            offset += static_cast<std::size_t>(written);
        }
    }
    if (ok && ::fsync(descriptor) != 0) ok = false;
    if (::close(descriptor) != 0) ok = false;
    if (!ok) {
        setError(error, "failed to write encrypted-volume temporary output");
        return false;
    }
    return true;
#endif
}

inline bool promoteAtomic(const std::filesystem::path& temporary,
                          const std::filesystem::path& target,
                          bool replace_existing,
                          std::string* error) {
#if defined(_WIN32)
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (replace_existing) flags |= MOVEFILE_REPLACE_EXISTING;
    if (!MoveFileExW(temporary.c_str(), target.c_str(), flags)) {
        setError(error, "failed to commit encrypted-volume output");
        return false;
    }
    return true;
#else
    std::error_code ec;
    if (replace_existing) {
        std::filesystem::rename(temporary, target, ec);
        if (ec) {
            setError(error, "failed to commit encrypted-volume output");
            return false;
        }
    } else {
        // POSIX rename() replaces an existing destination.  link()+unlink()
        // creates the destination directory entry atomically without
        // replacement, so a destination appearing after preflight is still
        // rejected rather than overwritten.
        if (::link(temporary.c_str(), target.c_str()) != 0) {
            setError(error, "failed to commit encrypted-volume output");
            return false;
        }
        if (::unlink(temporary.c_str()) != 0) {
            setError(error, "failed to finalize encrypted-volume output");
            return false;
        }
    }
    // The rename is atomic, but POSIX does not make the parent-directory
    // entry durable until the directory itself is synced.  Keep crash
    // consistency explicit for the host-only production lifecycle.
    const std::filesystem::path parent = target.parent_path().empty()
        ? std::filesystem::path(".") : target.parent_path();
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int directory = ::open(parent.c_str(), flags);
    if (directory < 0 || ::fsync(directory) != 0) {
        if (directory >= 0) ::close(directory);
        setError(error, "failed to sync encrypted-volume output directory");
        return false;
    }
    if (::close(directory) != 0) {
        setError(error, "failed to close encrypted-volume output directory");
        return false;
    }
    return true;
#endif
}

} // namespace detail

[[nodiscard]] inline bool readKeyFile(const std::string& path,
                                      ByteVector& key,
                                      std::string* error = nullptr) {
    detail::clearSensitive(key);
    detail::clearError(error);
    if (path.empty()) {
        detail::setError(error, "encrypted-volume key file path is invalid");
        return false;
    }
    ByteVector encoded;
#if defined(_WIN32)
    const std::filesystem::path key_path(path);
    const HANDLE handle = CreateFileW(
        key_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        detail::setError(error, "failed to open encrypted-volume key file");
        return false;
    }
    auto close_handle = [&]() { CloseHandle(handle); };
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information) ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        close_handle();
        detail::setError(error, "encrypted-volume key file is not a regular file");
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0) {
        close_handle();
        detail::setError(error, "encrypted-volume key file size is invalid");
        return false;
    }
    const std::uintmax_t encoded_size = static_cast<std::uintmax_t>(size.QuadPart);
    if (encoded_size != kAes256KeyBytes &&
        encoded_size != kAes256KeyBytes * 2U) {
        close_handle();
        detail::setError(
            error, "encrypted-volume key file must contain exactly 32 raw bytes or 64 hex characters");
        return false;
    }
    try {
        encoded.resize(static_cast<std::size_t>(encoded_size));
        DWORD received = 0;
        if (!ReadFile(handle, encoded.data(), static_cast<DWORD>(encoded.size()),
                      &received, nullptr) ||
            received != static_cast<DWORD>(encoded.size())) {
            detail::clearSensitive(encoded);
            close_handle();
            detail::setError(error, "encrypted-volume key file is truncated");
            return false;
        }
        std::uint8_t extra = 0;
        DWORD extra_received = 0;
        if (ReadFile(handle, &extra, 1, &extra_received, nullptr) &&
            extra_received != 0) {
            detail::clearSensitive(encoded);
            close_handle();
            detail::setError(error, "encrypted-volume key file size changed");
            return false;
        }
        LARGE_INTEGER final_size{};
        if (!GetFileSizeEx(handle, &final_size) ||
            final_size.QuadPart != size.QuadPart) {
            detail::clearSensitive(encoded);
            close_handle();
            detail::setError(error, "encrypted-volume key file size changed");
            return false;
        }

        if (encoded.size() == kAes256KeyBytes) {
            key = encoded;
        } else if (!detail::decodeHexKey(encoded, key)) {
            detail::clearSensitive(encoded);
            detail::setError(error, "encrypted-volume key file is not valid hexadecimal");
            return false;
        }
    } catch (const std::bad_alloc&) {
        detail::clearSensitive(encoded);
        detail::clearSensitive(key);
        close_handle();
        detail::setError(error, "encrypted-volume key file allocation failed");
        return false;
    }
    detail::clearSensitive(encoded);
    close_handle();
    return key.size() == kAes256KeyBytes;
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        detail::setError(error, "failed to open encrypted-volume key file");
        return false;
    }
    auto close_descriptor = [&]() { ::close(descriptor); };
    struct stat information{};
    if (::fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_size < 0) {
        close_descriptor();
        detail::setError(error, "encrypted-volume key file is not a regular file");
        return false;
    }
    const std::uintmax_t encoded_size =
        static_cast<std::uintmax_t>(information.st_size);
    if (encoded_size != kAes256KeyBytes &&
        encoded_size != kAes256KeyBytes * 2U) {
        close_descriptor();
        detail::setError(
            error, "encrypted-volume key file must contain exactly 32 raw bytes or 64 hex characters");
        return false;
    }
    try {
        encoded.resize(static_cast<std::size_t>(encoded_size));
        std::size_t received_total = 0;
        while (received_total < encoded.size()) {
            const ssize_t received = ::read(
                descriptor, encoded.data() + received_total,
                encoded.size() - received_total);
            if (received <= 0) {
                detail::clearSensitive(encoded);
                close_descriptor();
                detail::setError(error, "encrypted-volume key file is truncated");
                return false;
            }
            received_total += static_cast<std::size_t>(received);
        }
        std::uint8_t extra = 0;
        const ssize_t extra_received = ::read(descriptor, &extra, 1);
        if (extra_received != 0) {
            detail::clearSensitive(encoded);
            close_descriptor();
            detail::setError(error, "encrypted-volume key file size changed");
            return false;
        }
        struct stat final_information{};
        if (::fstat(descriptor, &final_information) != 0 ||
            final_information.st_size != information.st_size) {
            detail::clearSensitive(encoded);
            close_descriptor();
            detail::setError(error, "encrypted-volume key file size changed");
            return false;
        }
        if (encoded.size() == kAes256KeyBytes) {
            key = encoded;
        } else if (!detail::decodeHexKey(encoded, key)) {
            detail::clearSensitive(encoded);
            close_descriptor();
            detail::setError(error, "encrypted-volume key file is not valid hexadecimal");
            return false;
        }
    } catch (const std::bad_alloc&) {
        detail::clearSensitive(encoded);
        detail::clearSensitive(key);
        close_descriptor();
        detail::setError(error, "encrypted-volume key file allocation failed");
        return false;
    }
    detail::clearSensitive(encoded);
    close_descriptor();
    return key.size() == kAes256KeyBytes;
#endif
}

[[nodiscard]] inline bool readAes256KeyFile(const std::string& path,
                                            ByteVector& key,
                                            std::string* error = nullptr) {
    return readKeyFile(path, key, error);
}

[[nodiscard]] inline bool readFile(const std::string& path,
                                   ByteVector& bytes,
                                   std::string* error = nullptr) {
    detail::clearSensitive(bytes);
    detail::clearError(error);
    if (path.empty()) {
        detail::setError(error, "encrypted-volume input path is invalid");
        return false;
    }
    std::error_code type_error;
    if (!std::filesystem::is_regular_file(path, type_error) || type_error) {
        detail::setError(error, "encrypted-volume input is not a regular file");
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        detail::setError(error, "failed to open encrypted-volume input");
        return false;
    }
    std::error_code size_error;
    const std::uintmax_t size = std::filesystem::file_size(path, size_error);
    if (size_error || size > kMaximumEnvelopeBytes ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        detail::setError(error, "encrypted-volume input size is invalid");
        return false;
    }
    try {
        bytes.resize(static_cast<std::size_t>(size));
    } catch (const std::bad_alloc&) {
        detail::clearSensitive(bytes);
        detail::setError(error, "encrypted-volume input allocation failed");
        return false;
    }
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(bytes.size()) ||
            input.bad()) {
            detail::clearSensitive(bytes);
            detail::setError(error, "encrypted-volume input is truncated");
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool writeFileAtomic(const std::string& path,
                                          const ByteVector& bytes,
                                          std::string* error = nullptr,
                                          bool replace_existing = true) {
    detail::clearError(error);
    if (path.empty() || bytes.size() > kMaximumEnvelopeBytes ||
        bytes.size() > static_cast<std::size_t>(
                            std::numeric_limits<std::streamsize>::max())) {
        detail::setError(error, "encrypted-volume output is invalid");
        return false;
    }
    const std::filesystem::path target(path);
    std::error_code ec;
    if (!target.parent_path().empty())
        std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        detail::setError(error, "failed to create encrypted-volume output directory");
        return false;
    }
    const std::filesystem::path temporary = detail::temporaryPathFor(target);
    detail::TemporaryFileCleanup cleanup(temporary);
    bool temporary_created = false;
    const bool created = detail::createRestrictedTemporaryFile(
        temporary, bytes, temporary_created, error);
    if (temporary_created) cleanup.markCreated();
    if (!created) {
        return false;
    }
    if (!detail::syncFileToStableStorage(temporary, error) ||
        !detail::promoteAtomic(temporary, target, replace_existing, error)) {
        return false;
    }
    cleanup.markPromoted();
    return true;
}

[[nodiscard]] inline bool encryptFile(
    const std::string& input_path,
    const std::string& output_path,
    const ByteVector& key,
    const AuthenticatedEncryptionProvider& provider,
    const EncryptOptions& options = {},
    std::string* error = nullptr,
    bool replace_existing = true) {
    ByteVector plain;
    ByteVector envelope;
    if (!readFile(input_path, plain, error) ||
        !encryptBytes(plain, key, provider, envelope, options, error)) {
        detail::clearSensitive(plain);
        return false;
    }
    const bool ok = writeFileAtomic(output_path, envelope, error, replace_existing);
    detail::clearSensitive(plain);
    detail::clearSensitive(envelope);
    return ok;
}

[[nodiscard]] inline bool decryptFile(
    const std::string& input_path,
    const std::string& output_path,
    const ByteVector& key,
    const AuthenticatedEncryptionProvider& provider,
    std::string* error = nullptr,
    bool replace_existing = true) {
    ByteVector envelope;
    ByteVector plain;
    if (!readFile(input_path, envelope, error) ||
        !decryptBytes(envelope, key, provider, plain, error)) {
        detail::clearSensitive(envelope);
        detail::clearSensitive(plain);
        return false;
    }
    const bool ok = writeFileAtomic(output_path, plain, error, replace_existing);
    detail::clearSensitive(envelope);
    detail::clearSensitive(plain);
    return ok;
}

// Safe image-boundary helpers.  They only accept the current canonical tDisk
// v2 header, record geometry, raw-word range, ordering, and checksum.  They
// preserve the image bytes verbatim; the existing runtime still performs its
// own load-time validation after decryption.
[[nodiscard]] inline bool encryptTdiskFile(
    const std::string& input_path,
    const std::string& output_path,
    const ByteVector& key,
    const AuthenticatedEncryptionProvider& provider,
    EncryptOptions options = {},
    std::string* error = nullptr,
    bool replace_existing = true) {
    ByteVector plain;
    ByteVector envelope;
    if (!readFile(input_path, plain, error)) return false;
    if (!isCanonicalTdiskV2(plain, error)) {
        detail::clearSensitive(plain);
        return false;
    }
    options.logical_block_words = kTdiskBlockWords;
    const bool encrypted = encryptBytes(plain, key, provider, envelope,
                                        options, error);
    detail::clearSensitive(plain);
    if (!encrypted) return false;
    const bool written = writeFileAtomic(output_path, envelope, error, replace_existing);
    detail::clearSensitive(envelope);
    return written;
}

[[nodiscard]] inline bool decryptTdiskFile(
    const std::string& input_path,
    const std::string& output_path,
    const ByteVector& key,
    const AuthenticatedEncryptionProvider& provider,
    std::string* error = nullptr,
    bool replace_existing = true) {
    ByteVector envelope;
    ByteVector plain;
    if (!readFile(input_path, envelope, error) ||
        !decryptBytes(envelope, key, provider, plain, error)) {
        detail::clearSensitive(envelope);
        detail::clearSensitive(plain);
        return false;
    }
    detail::clearSensitive(envelope);
    if (!isCanonicalTdiskV2(plain, error)) {
        detail::clearSensitive(plain);
        return false;
    }
    const bool written = writeFileAtomic(output_path, plain, error, replace_existing);
    detail::clearSensitive(plain);
    return written;
}

} // namespace sandbox::host::encrypted_volume

#endif // TRIT_ENCRYPTED_VOLUME_HOST_H
