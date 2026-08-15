#include "encrypted_volume_host.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

using sandbox::host::encrypted_volume::AuthenticatedEncryptionProvider;
using sandbox::host::encrypted_volume::ByteVector;
using sandbox::host::encrypted_volume::EncryptOptions;
using sandbox::host::encrypted_volume::kAesGcmNonceBytes;
using sandbox::host::encrypted_volume::kAesGcmTagBytes;
using sandbox::host::encrypted_volume::kAlgorithmTestOnly;
using sandbox::host::encrypted_volume::kNoncePrefixBytes;

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << "\n";
    }
}

// Deterministic provider used only by this test executable.  It intentionally
// is not a cryptographic implementation and is never exposed by production
// code.  Its purpose is to exercise envelope framing when OpenSSL is omitted.
class TestOnlyDeterministicMock final : public AuthenticatedEncryptionProvider {
public:
    [[nodiscard]] std::uint32_t algorithmId() const override {
        return kAlgorithmTestOnly;
    }
    [[nodiscard]] std::size_t keyBytes() const override { return 32; }
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
                               std::string* error) const override {
        if (key.size() != keyBytes() || nonce.size() != nonceBytes()) {
            if (error) *error = "test-only mock input size is invalid";
            ciphertext.clear();
            tag.clear();
            return false;
        }
        ciphertext.resize(plaintext.size());
        for (std::size_t i = 0; i < plaintext.size(); ++i) {
            ciphertext[i] = static_cast<std::uint8_t>(
                plaintext[i] ^ streamByte(key, nonce, i));
        }
        tag = makeTag(key, nonce, aad, ciphertext);
        return true;
    }

    [[nodiscard]] bool decrypt(const ByteVector& key,
                               const ByteVector& nonce,
                               const ByteVector& aad,
                               const ByteVector& ciphertext,
                               const ByteVector& tag,
                               ByteVector& plaintext,
                               std::string* error) const override {
        plaintext.clear();
        if (key.size() != keyBytes() || nonce.size() != nonceBytes() ||
            tag.size() != tagBytes()) {
            if (error) *error = "test-only mock input size is invalid";
            return false;
        }
        const ByteVector expected = makeTag(key, nonce, aad, ciphertext);
        if (expected != tag) {
            if (error) *error = "test-only mock authentication failed";
            return false;
        }
        plaintext.resize(ciphertext.size());
        for (std::size_t i = 0; i < ciphertext.size(); ++i) {
            plaintext[i] = static_cast<std::uint8_t>(
                ciphertext[i] ^ streamByte(key, nonce, i));
        }
        return true;
    }

    [[nodiscard]] bool randomBytes(std::uint8_t* destination,
                                   std::size_t size,
                                   std::string*) const override {
        for (std::size_t i = 0; i < size; ++i)
            destination[i] = static_cast<std::uint8_t>(0xa0U + i);
        return true;
    }

private:
    static std::uint64_t fnv(const ByteVector& bytes, std::uint64_t hash) {
        for (std::uint8_t value : bytes) {
            hash ^= value;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static ByteVector makeTag(const ByteVector& key,
                              const ByteVector& nonce,
                              const ByteVector& aad,
                              const ByteVector& ciphertext) {
        std::uint64_t hash = 1469598103934665603ULL;
        hash = fnv(key, hash);
        hash = fnv(nonce, hash);
        hash = fnv(aad, hash);
        hash = fnv(ciphertext, hash);
        ByteVector tag(kAesGcmTagBytes, 0);
        for (std::size_t i = 0; i < tag.size(); ++i) {
            tag[i] = static_cast<std::uint8_t>(hash >> ((i % 8) * 8));
            if ((i % 8) == 7) hash = hash * 6364136223846793005ULL + 1;
        }
        return tag;
    }

    static std::uint8_t streamByte(const ByteVector& key,
                                   const ByteVector& nonce,
                                   std::size_t index) {
        std::uint64_t hash = fnv(key, 1469598103934665603ULL);
        hash = fnv(nonce, hash);
        hash ^= static_cast<std::uint64_t>(index) * 0x9e3779b97f4a7c15ULL;
        hash ^= hash >> 29;
        return static_cast<std::uint8_t>(hash & 0xffU);
    }
};

ByteVector makePlaintext() {
    ByteVector bytes(200'123);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        // Include zero runs: sparse image payloads contain many zero words.
        bytes[i] = (i % 257 == 0) ? 0 :
                   static_cast<std::uint8_t>((i * 31U + 7U) & 0xffU);
    }
    return bytes;
}

ByteVector makeKey(std::uint8_t seed) {
    ByteVector key(32);
    for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<std::uint8_t>(seed + i * 3U);
    return key;
}

void exerciseProvider(const AuthenticatedEncryptionProvider& provider,
                      const std::string& label) {
    const ByteVector plain = makePlaintext();
    const ByteVector key = makeKey(0x11);
    const ByteVector wrong_key = makeKey(0x71);
    EncryptOptions options;
    options.chunk_bytes = 4096;
    options.logical_block_words = 27;
    options.nonce_prefix = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28};
    options.nonce_prefix_provided = true;

    ByteVector envelope;
    std::string error;
    expect(sandbox::host::encrypted_volume::encryptBytes(
               plain, key, provider, envelope, options, &error),
           label + " roundtrip encryption succeeds: " + error);
    expect(envelope.size() > plain.size(), label + " envelope carries framing and tags");

    ByteVector recovered;
    expect(sandbox::host::encrypted_volume::decryptBytes(
               envelope, key, provider, recovered, &error),
           label + " roundtrip decryption succeeds: " + error);
    expect(recovered == plain, label + " plaintext roundtrip matches");

    ByteVector wrong;
    error.clear();
    expect(!sandbox::host::encrypted_volume::decryptBytes(
               envelope, wrong_key, provider, wrong, &error),
           label + " wrong key is rejected");
    expect(wrong.empty(), label + " wrong-key failure does not expose partial plaintext");

    ByteVector tampered = envelope;
    tampered[tampered.size() - 1] ^= 0x80;
    error.clear();
    expect(!sandbox::host::encrypted_volume::decryptBytes(
               tampered, key, provider, wrong, &error),
           label + " ciphertext tamper is rejected");
    expect(wrong.empty(), label + " ciphertext tamper leaves no plaintext");

    tampered = envelope;
    tampered[56] ^= 0x01; // nonce prefix is authenticated header data.
    error.clear();
    expect(!sandbox::host::encrypted_volume::decryptBytes(
               tampered, key, provider, wrong, &error),
           label + " nonce/header tamper is rejected");

    EncryptOptions generated_options = options;
    generated_options.nonce_prefix_provided = false;
    generated_options.nonce_prefix.fill(0);
    ByteVector generated_envelope;
    error.clear();
    expect(sandbox::host::encrypted_volume::encryptBytes(
               plain, key, provider, generated_envelope, generated_options, &error),
           label + " provider-generated nonce prefix succeeds: " + error);
    recovered.clear();
    expect(sandbox::host::encrypted_volume::decryptBytes(
               generated_envelope, key, provider, recovered, &error) && recovered == plain,
           label + " provider-generated nonce prefix roundtrips");

    tampered = envelope;
    tampered.resize(tampered.size() - 1);
    error.clear();
    expect(!sandbox::host::encrypted_volume::decryptBytes(
               tampered, key, provider, wrong, &error),
           label + " truncation is rejected");

    tampered = envelope;
    tampered.push_back(0);
    error.clear();
    expect(!sandbox::host::encrypted_volume::decryptBytes(
               tampered, key, provider, wrong, &error),
           label + " trailing bytes are rejected");
}

void exerciseFileHelpers(const AuthenticatedEncryptionProvider& provider) {
    const auto root = std::filesystem::temp_directory_path() / "trit-encrypted-volume-tests";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    const auto input = root / "disk.tdisk";
    const auto encrypted = root / "disk.tenc";
    const auto output = root / "disk.roundtrip.tdisk";
    const auto tdisk_input = root / "canonical.tdisk";
    const auto tdisk_encrypted = root / "canonical.tenc";
    const auto tdisk_output = root / "canonical.roundtrip.tdisk";
    const ByteVector plain = makePlaintext();
    const ByteVector key = makeKey(0x31);
    std::string error;
    {
        std::ofstream file(input, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(plain.data()),
                   static_cast<std::streamsize>(plain.size()));
    }
    EncryptOptions options;
    options.chunk_bytes = 8192;
    options.logical_block_words = 27;
    options.nonce_prefix = {1, 2, 3, 4, 5, 6, 7, 8};
    options.nonce_prefix_provided = true;
    expect(sandbox::host::encrypted_volume::encryptFile(
               input.string(), encrypted.string(), key, provider, options, &error),
           "file helper encrypts opaque tDisk payload: " + error);
    expect(sandbox::host::encrypted_volume::decryptFile(
               encrypted.string(), output.string(), key, provider, &error),
           "file helper decrypts opaque tDisk payload: " + error);
    ByteVector recovered;
    expect(sandbox::host::encrypted_volume::readFile(output.string(), recovered, &error),
           "file helper reads decrypted payload: " + error);
    expect(recovered == plain, "file helper preserves sparse-image bytes");

    ByteVector canonical;
    const std::uint64_t magic = sandbox::host::encrypted_volume::kTdiskV2Magic;
    for (int shift = 0; shift < 64; shift += 8)
        canonical.push_back(static_cast<std::uint8_t>((magic >> shift) & 0xffU));
    canonical.push_back(2);
    canonical.push_back(0);
    canonical.push_back(0);
    canonical.push_back(0);
    canonical.push_back(27);
    canonical.push_back(0);
    canonical.push_back(0);
    canonical.push_back(0);
    canonical.insert(canonical.end(), plain.begin(), plain.end());
    {
        std::ofstream file(tdisk_input, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(canonical.data()),
                   static_cast<std::streamsize>(canonical.size()));
    }
    error.clear();
    expect(sandbox::host::encrypted_volume::encryptTdiskFile(
               tdisk_input.string(), tdisk_encrypted.string(), key, provider,
               options, &error),
           "tDisk helper accepts canonical v2 marker: " + error);
    expect(sandbox::host::encrypted_volume::decryptTdiskFile(
               tdisk_encrypted.string(), tdisk_output.string(), key, provider, &error),
           "tDisk helper validates decrypted marker: " + error);
    recovered.clear();
    expect(sandbox::host::encrypted_volume::readFile(
               tdisk_output.string(), recovered, &error) && recovered == canonical,
           "tDisk helper preserves canonical sparse image bytes");
    std::filesystem::remove_all(root, ec);
}

} // namespace

int main() {
    TestOnlyDeterministicMock mock;
    exerciseProvider(mock, "test-only deterministic provider");
    exerciseFileHelpers(mock);

    sandbox::host::encrypted_volume::FailClosedProvider fail_closed;
    ByteVector output;
    std::string error;
    expect(!sandbox::host::encrypted_volume::encryptBytes(
               ByteVector{1, 2, 3}, makeKey(0), fail_closed, output, {}, &error),
           "default provider fails closed without crypto dependency");

#if defined(TRIT_ENCRYPTED_VOLUME_ENABLE_OPENSSL)
    sandbox::host::encrypted_volume::OpenSslAes256GcmProvider openssl;
    exerciseProvider(openssl, "OpenSSL AES-256-GCM provider");
    exerciseFileHelpers(openssl);
#endif

    if (failures != 0) {
        std::cerr << failures << " encrypted-volume checks failed\n";
        return 1;
    }
    std::cout << "encrypted-volume checks passed\n";
    return 0;
}
