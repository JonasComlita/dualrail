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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
inline constexpr std::uint64_t kTdiskV2Magic = 0x54524954535032ULL;
inline constexpr std::uint32_t kTdiskV2Version = 2;
inline constexpr std::uint32_t kTdiskBlockWords = 27;

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
    // Callers may provide a fixed prefix in EncryptOptions for deterministic
    // fixtures, but must never reuse one with the same key.
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
    if (envelope.size() < kEnvelopeHeaderBytes) {
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
        flags != 0 || offset + prefix.size() > envelope.size()) {
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
        chunk_count > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        setError(error, "encrypted-volume logical size is unreasonable");
        return false;
    }
    const std::uint64_t expected_chunks =
        logical_size == 0 ? 0 :
        (logical_size + static_cast<std::uint64_t>(chunk_bytes) - 1) /
            static_cast<std::uint64_t>(chunk_bytes);
    if (chunk_count != expected_chunks) {
        setError(error, "encrypted-volume chunk count is inconsistent");
        return false;
    }
    if (logical_block_words != 0 && logical_block_count == 0 && logical_size != 0) {
        setError(error, "encrypted-volume block metadata is inconsistent");
        return false;
    }
    if (logical_block_words != 0) {
        constexpr std::uint64_t kBytesPerTritWord = 8;
        const std::uint64_t block_bytes =
            static_cast<std::uint64_t>(logical_block_words) * kBytesPerTritWord;
        if (block_bytes == 0 ||
            (logical_size != 0 &&
             (logical_size + block_bytes - 1) / block_bytes != logical_block_count)) {
            setError(error, "encrypted-volume block metadata is inconsistent");
            return false;
        }
    } else if (logical_block_count != 0) {
        setError(error, "encrypted-volume block metadata is inconsistent");
        return false;
    }
    return true;
}

inline bool buildAad(const ByteVector& header,
                     std::uint32_t chunk_index,
                     std::uint32_t plaintext_bytes,
                     std::uint32_t ciphertext_bytes,
                     ByteVector& aad) {
    aad = header;
    appendLe32(aad, chunk_index);
    appendLe32(aad, plaintext_bytes);
    appendLe32(aad, ciphertext_bytes);
    return true;
}

inline bool isCanonicalTdiskV2(const ByteVector& bytes) {
    if (bytes.size() < 16) return false;
    std::size_t offset = 0;
    std::uint64_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t block_words = 0;
    if (!readLe64(bytes, offset, magic) || !readLe32(bytes, offset, version) ||
        !readLe32(bytes, offset, block_words)) {
        return false;
    }
    return magic == kTdiskV2Magic && version == kTdiskV2Version &&
           block_words == kTdiskBlockWords;
}

} // namespace detail

[[nodiscard]] inline bool encryptBytes(
    const ByteVector& plaintext,
    const ByteVector& key,
    const AuthenticatedEncryptionProvider& provider,
    ByteVector& envelope,
    const EncryptOptions& options = {},
    std::string* error = nullptr) {
    envelope.clear();
    if (provider.algorithmId() == 0 || provider.keyBytes() == 0 ||
        provider.nonceBytes() != kAesGcmNonceBytes || provider.tagBytes() == 0) {
        detail::setError(error, "authenticated-encryption provider is not configured");
        return false;
    }
    if (key.size() != provider.keyBytes()) {
        detail::setError(error, "encrypted-volume key length is invalid");
        return false;
    }
    if (plaintext.size() > kMaximumPlaintextBytes ||
        options.chunk_bytes < kMinimumChunkBytes ||
        options.chunk_bytes > kMaximumChunkBytes) {
        detail::setError(error, "encrypted-volume plaintext or chunk size is invalid");
        return false;
    }
    const std::uint64_t chunk_count =
        plaintext.empty() ? 0 :
        (static_cast<std::uint64_t>(plaintext.size()) + options.chunk_bytes - 1) /
            options.chunk_bytes;
    if (chunk_count > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        detail::setError(error, "encrypted-volume has too many chunks");
        return false;
    }
    if (options.logical_block_words != 0) {
        const std::uint64_t block_bytes =
            static_cast<std::uint64_t>(options.logical_block_words) * 8ULL;
        if (block_bytes == 0) {
            detail::setError(error, "encrypted-volume block metadata is invalid");
            return false;
        }
    }

    std::array<std::uint8_t, kNoncePrefixBytes> prefix = options.nonce_prefix;
    if (!options.nonce_prefix_provided &&
        !provider.randomBytes(prefix.data(), prefix.size(), error)) {
        envelope.clear();
        return false;
    }
    std::uint64_t logical_block_count = 0;
    if (options.logical_block_words != 0 && !plaintext.empty()) {
        const std::uint64_t block_bytes =
            static_cast<std::uint64_t>(options.logical_block_words) * 8ULL;
        logical_block_count =
            (static_cast<std::uint64_t>(plaintext.size()) + block_bytes - 1) /
            block_bytes;
    }

    ByteVector header;
    detail::appendHeader(header, provider.algorithmId(), options.chunk_bytes,
                         options.logical_block_words,
                         static_cast<std::uint64_t>(plaintext.size()),
                         logical_block_count, chunk_count, prefix);
    envelope = header;
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
        if (size > std::numeric_limits<std::uint32_t>::max()) {
            detail::setError(error, "encrypted-volume chunk is too large");
            envelope.clear();
            return false;
        }
        const auto chunk_index = static_cast<std::uint32_t>(chunk);
        detail::makeNonce(prefix, chunk_index, nonce);
        const auto* begin = plaintext.data() + static_cast<std::ptrdiff_t>(offset);
        ByteVector chunk_plain(begin, begin + static_cast<std::ptrdiff_t>(size));
        detail::buildAad(header, chunk_index, static_cast<std::uint32_t>(size),
                         static_cast<std::uint32_t>(size), aad);
        ciphertext.clear();
        tag.clear();
        if (!provider.encrypt(key, nonce, aad, chunk_plain, ciphertext, tag, error) ||
            ciphertext.size() != size || tag.size() != provider.tagBytes()) {
            detail::clearSensitive(chunk_plain);
            detail::clearSensitive(nonce);
            detail::clearSensitive(aad);
            ciphertext.clear();
            tag.clear();
            envelope.clear();
            if (error && error->empty())
                *error = "encrypted-volume chunk encryption failed";
            return false;
        }
        detail::appendLe32(envelope, chunk_index);
        detail::appendLe32(envelope, static_cast<std::uint32_t>(size));
        detail::appendLe32(envelope, static_cast<std::uint32_t>(ciphertext.size()));
        envelope.insert(envelope.end(), tag.begin(), tag.end());
        envelope.insert(envelope.end(), ciphertext.begin(), ciphertext.end());
        detail::clearSensitive(chunk_plain);
    }
    detail::clearSensitive(nonce);
    detail::clearSensitive(aad);
    return true;
}

[[nodiscard]] inline bool decryptBytes(
    const ByteVector& envelope,
    const ByteVector& key,
    const AuthenticatedEncryptionProvider& provider,
    ByteVector& plaintext,
    std::string* error = nullptr) {
    plaintext.clear();
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
    if (algorithm != provider.algorithmId() || provider.keyBytes() == 0 ||
        provider.nonceBytes() != kAesGcmNonceBytes || provider.tagBytes() == 0 ||
        key.size() != provider.keyBytes()) {
        detail::setError(error, "encrypted-volume provider or key is incompatible");
        return false;
    }
    if (logical_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        detail::setError(error, "encrypted-volume plaintext is too large for host");
        return false;
    }
    ByteVector header(envelope.begin(),
                     envelope.begin() + static_cast<std::ptrdiff_t>(kEnvelopeHeaderBytes));
    ByteVector staged;
    staged.reserve(static_cast<std::size_t>(logical_size));
    std::size_t offset = kEnvelopeHeaderBytes;
    ByteVector nonce;
    ByteVector aad;
    ByteVector ciphertext;
    ByteVector tag;
    for (std::uint64_t expected_chunk = 0; expected_chunk < chunk_count;
         ++expected_chunk) {
        std::uint32_t chunk_index = 0;
        std::uint32_t plaintext_bytes = 0;
        std::uint32_t ciphertext_bytes = 0;
        if (!detail::readLe32(envelope, offset, chunk_index) ||
            !detail::readLe32(envelope, offset, plaintext_bytes) ||
            !detail::readLe32(envelope, offset, ciphertext_bytes) ||
            chunk_index != expected_chunk || plaintext_bytes == 0 ||
            plaintext_bytes > chunk_bytes || ciphertext_bytes != plaintext_bytes) {
            detail::setError(error, "encrypted-volume chunk metadata is invalid");
            return false;
        }
        std::size_t record_bytes = 0;
        if (!detail::checkedAdd(provider.tagBytes(), ciphertext_bytes, record_bytes) ||
            offset > envelope.size() || envelope.size() - offset < record_bytes) {
            detail::setError(error, "encrypted-volume chunk is truncated");
            return false;
        }
        tag.assign(envelope.begin() + static_cast<std::ptrdiff_t>(offset),
                   envelope.begin() + static_cast<std::ptrdiff_t>(offset + provider.tagBytes()));
        offset += provider.tagBytes();
        ciphertext.assign(envelope.begin() + static_cast<std::ptrdiff_t>(offset),
                          envelope.begin() + static_cast<std::ptrdiff_t>(offset + ciphertext_bytes));
        offset += ciphertext_bytes;
        detail::makeNonce(prefix, chunk_index, nonce);
        detail::buildAad(header, chunk_index, plaintext_bytes, ciphertext_bytes, aad);
        ByteVector chunk_plain;
        if (!provider.decrypt(key, nonce, aad, ciphertext, tag, chunk_plain, error) ||
            chunk_plain.size() != plaintext_bytes) {
            detail::clearSensitive(chunk_plain);
            staged.clear();
            if (error && error->empty())
                *error = "encrypted-volume authentication failed";
            return false;
        }
        staged.insert(staged.end(), chunk_plain.begin(), chunk_plain.end());
        detail::clearSensitive(chunk_plain);
    }
    if (offset != envelope.size() || staged.size() != logical_size) {
        detail::setError(error, "encrypted-volume envelope has trailing or missing data");
        staged.clear();
        return false;
    }
    plaintext.swap(staged);
    detail::clearSensitive(nonce);
    detail::clearSensitive(aad);
    ciphertext.clear();
    tag.clear();
    return true;
}

[[nodiscard]] inline bool readFile(const std::string& path,
                                   ByteVector& bytes,
                                   std::string* error = nullptr) {
    bytes.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        detail::setError(error, "failed to open encrypted-volume input");
        return false;
    }
    std::error_code size_error;
    const std::uintmax_t size = std::filesystem::file_size(path, size_error);
    if (size_error || size > kMaximumPlaintextBytes ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        detail::setError(error, "encrypted-volume input size is invalid");
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!input.good()) {
            bytes.clear();
            detail::setError(error, "encrypted-volume input is truncated");
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool writeFileAtomic(const std::string& path,
                                          const ByteVector& bytes,
                                          std::string* error = nullptr) {
    const std::filesystem::path target(path);
    std::error_code ec;
    if (!target.parent_path().empty())
        std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        detail::setError(error, "failed to create encrypted-volume output directory");
        return false;
    }
    const std::filesystem::path temporary = target.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output.good()) {
            detail::setError(error, "failed to open encrypted-volume output");
            return false;
        }
        if (!bytes.empty())
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output.good()) {
            std::error_code cleanup;
            std::filesystem::remove(temporary, cleanup);
            detail::setError(error, "failed to write encrypted-volume output");
            return false;
        }
    }
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy_file(temporary, target,
                                    std::filesystem::copy_options::overwrite_existing, ec);
        std::error_code cleanup;
        std::filesystem::remove(temporary, cleanup);
    }
    if (ec) {
        detail::setError(error, "failed to commit encrypted-volume output");
        return false;
    }
    return true;
}

[[nodiscard]] inline bool encryptFile(
    const std::string& input_path,
    const std::string& output_path,
    const ByteVector& key,
    const AuthenticatedEncryptionProvider& provider,
    const EncryptOptions& options = {},
    std::string* error = nullptr) {
    ByteVector plain;
    ByteVector envelope;
    if (!readFile(input_path, plain, error) ||
        !encryptBytes(plain, key, provider, envelope, options, error)) {
        detail::clearSensitive(plain);
        return false;
    }
    const bool ok = writeFileAtomic(output_path, envelope, error);
    detail::clearSensitive(plain);
    envelope.clear();
    return ok;
}

[[nodiscard]] inline bool decryptFile(
    const std::string& input_path,
    const std::string& output_path,
    const ByteVector& key,
    const AuthenticatedEncryptionProvider& provider,
    std::string* error = nullptr) {
    ByteVector envelope;
    ByteVector plain;
    if (!readFile(input_path, envelope, error) ||
        !decryptBytes(envelope, key, provider, plain, error)) {
        envelope.clear();
        detail::clearSensitive(plain);
        return false;
    }
    const bool ok = writeFileAtomic(output_path, plain, error);
    envelope.clear();
    detail::clearSensitive(plain);
    return ok;
}

// Safe image-boundary helpers.  They only accept the current canonical tDisk
// v2 marker and preserve the image bytes verbatim; the existing runtime still
// performs the full tDisk checksum/record validation after decryption.
[[nodiscard]] inline bool encryptTdiskFile(
    const std::string& input_path,
    const std::string& output_path,
    const ByteVector& key,
    const AuthenticatedEncryptionProvider& provider,
    EncryptOptions options = {},
    std::string* error = nullptr) {
    ByteVector plain;
    ByteVector envelope;
    if (!readFile(input_path, plain, error)) return false;
    if (!detail::isCanonicalTdiskV2(plain)) {
        detail::setError(error, "encrypted-volume input is not canonical tDisk v2");
        detail::clearSensitive(plain);
        return false;
    }
    options.logical_block_words = kTdiskBlockWords;
    const bool encrypted = encryptBytes(plain, key, provider, envelope,
                                        options, error);
    detail::clearSensitive(plain);
    if (!encrypted) return false;
    const bool written = writeFileAtomic(output_path, envelope, error);
    envelope.clear();
    return written;
}

[[nodiscard]] inline bool decryptTdiskFile(
    const std::string& input_path,
    const std::string& output_path,
    const ByteVector& key,
    const AuthenticatedEncryptionProvider& provider,
    std::string* error = nullptr) {
    ByteVector envelope;
    ByteVector plain;
    if (!readFile(input_path, envelope, error) ||
        !decryptBytes(envelope, key, provider, plain, error)) {
        envelope.clear();
        detail::clearSensitive(plain);
        return false;
    }
    envelope.clear();
    if (!detail::isCanonicalTdiskV2(plain)) {
        detail::setError(error, "decrypted encrypted-volume payload is not canonical tDisk v2");
        detail::clearSensitive(plain);
        return false;
    }
    const bool written = writeFileAtomic(output_path, plain, error);
    detail::clearSensitive(plain);
    return written;
}

} // namespace sandbox::host::encrypted_volume

#endif // TRIT_ENCRYPTED_VOLUME_HOST_H
